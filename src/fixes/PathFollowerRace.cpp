// SkyrimSE+11BCBB0 (1.6.1170), `mov r8d, [rcx+0x80]` with rcx = 0, under ProcessLists::UpdateNonHighActors, with a
// MovementAgentPathFollowerVirtual among the objects. Typical trigger: fast travel.
// Cause: a path follower's writers (SetPath, ClearPath, SetPathFollowing) take its lock. Its update reads the path
// several times without the lock. Far-away actors are updated by the main thread and three job threads at once. A
// Patrol package that teleports its group during a time skip clears a member's path from the leader's thread. A clear
// between two of the update's reads leaves the second read with null. MovementAgentPathFollowerFlight has the same
// update. MovementAgentPathFollowerStandard takes its lock around its update.
// Fix: the game's own fix from 1.7.104, where the virtual follower's update takes the follower's lock for write around
// its whole loop. The fix does the same on the builds before 1.7.104, and for the flight follower, which no build
// locks. Install checks that the vtables point at the hooked functions (or at another plugin's slot hook), that the
// writers take the lock at the expected offset, and the first bytes of both updates. A virtual update that takes the
// lock itself stays as it is.

#include "fix.h"
#include "hook.h"

namespace PathFollowerRace
{
	namespace
	{
		// MovementAgentPathFollowerVirtual: IMovementPlannerAgent at +0x18, IMovementPathManagerAgent at +0x20,
		// IMovementSetPathFollowing at +0x30, the lock at +0x38
		constexpr REL::RelocationID kVirtualUpdate{ 90053, 92541 };        // planner slot 3
		constexpr REL::RelocationID kVirtualSetPath{ 90054, 92542 };       // path manager slot 2
		constexpr REL::RelocationID kVirtualClearPath{ 90057, 92545 };     // path manager slot 6
		constexpr REL::RelocationID kVirtualSetFollowing{ 90065, 92553 };  // set path following slot 1
		// MovementAgentPathFollowerFlight: the planner's slot 3 is a stub that jumps to the update with the object;
		// IMovementPathManagerAgent at +0x20, the lock at +0xA0
		constexpr REL::RelocationID kFlightUpdate{ 90024, 92509 };
		constexpr REL::RelocationID kFlightSetPath{ 90008, 92493 };
		constexpr REL::RelocationID kFlightClearPath{ 90010, 92495 };

		constexpr std::ptrdiff_t kVirtualLock = 0x20;  // from the planner interface the update gets (+0x38 in the object)
		constexpr std::ptrdiff_t kFlightLock = 0xA0;   // from the object the flight update gets

		// the stub in Flight's planner slot 3: cmp dword ptr [rcx+20h],1 / jne +9 / add rcx,-18h / jmp rel32
		constexpr auto kFlightStub = "\x83\x79\x20\x01\x75\x09\x48\x83\xC1\xE8\xE9"sv;
		// the instructions the entry jumps replace, the same on 1.5.97, 1.6.1170 and 1.7.104: mov [rsp+8],rbx, and
		// mov rax,rsp / push rbp / push rsi
		constexpr auto kVirtualUpdateHead = "\x48\x89\x5C\x24\x08"sv;
		constexpr auto kFlightUpdateHead = "\x48\x8B\xC4\x55\x56"sv;
		// a virtual update that takes the follower's lock itself (1.7.104): lea rbx,[rcx+20h] / mov [rsp+40h],rbx /
		// mov rcx,rbx / call
		constexpr auto kLockingUpdate = "\x48\x8D\x59\x20\x48\x89\x5C\x24\x40\x48\x8B\xCB\xE8"sv;

		using Update_t = void (*)(void*, void*, void*, void*);
		Update_t g_virtualUpdate = nullptr;  // the original functions, through the stubs
		Update_t g_flightUpdate = nullptr;

		void VirtualUpdate(void* a_planner, void* a_dt, void* a3, void* a4)
		{
			auto* lock = reinterpret_cast<RE::BSReadWriteLock*>(static_cast<char*>(a_planner) + kVirtualLock);
			lock->LockForWrite();
			g_virtualUpdate(a_planner, a_dt, a3, a4);
			lock->UnlockForWrite();
		}

		void FlightUpdate(void* a_follower, void* a_dt, void* a3, void* a4)
		{
			auto* lock = reinterpret_cast<RE::BSReadWriteLock*>(static_cast<char*>(a_follower) + kFlightLock);
			lock->LockForWrite();
			g_flightUpdate(a_follower, a_dt, a3, a4);
			lock->UnlockForWrite();
		}

		// The writer takes its follower's lock at this + a_disp: lea r64,[rcx+disp8], add rcx,disp8, or sub rcx,-0x80
		// for 0x80, within its first bytes. The updates' lock offsets above rely on the same layout.
		bool LocksAt(std::uintptr_t a_fn, std::uint8_t a_disp)
		{
			const auto code = Bytes(a_fn, 0x40);
			for (std::size_t i = 0; i + 4 <= code.size(); ++i) {
				const auto b = [&](std::size_t k) { return static_cast<std::uint8_t>(code[i + k]); };
				if ((b(0) == 0x48 && b(1) == 0x8D && (b(2) & 0xC7) == 0x41 && b(3) == a_disp) ||
					(b(0) == 0x48 && b(1) == 0x83 && b(2) == 0xC1 && b(3) == a_disp) ||
					(b(0) == 0x48 && b(1) == 0x83 && b(2) == 0xE9 && b(3) == 0x80 && a_disp == 0x80)) {
					return true;
				}
			}
			return false;
		}

		std::uintptr_t Slot(const REL::VariantID& a_vtable, std::size_t a_slot)
		{
			return reinterpret_cast<const std::uintptr_t*>(a_vtable.address())[a_slot];
		}

		bool InGame(std::uintptr_t a_at)
		{
			const auto base = REL::Module::get().base();
			const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(
				base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
			return a_at >= base && a_at < base + nt->OptionalHeader.SizeOfImage;
		}

		// The slot holds the function, or another plugin's hook of the slot (outside SkyrimSE.exe) that calls on into it.
		// The fix hooks the function's entry, so a slot hook in front of it changes nothing for the fix.
		bool SlotIs(const REL::VariantID& a_vtable, std::size_t a_slot, const REL::RelocationID& a_fn)
		{
			const auto at = Slot(a_vtable, a_slot);
			return at == a_fn.address() || !InGame(at);
		}
	}

	Result Install()
	{
		const auto& virt = RE::VTABLE_MovementAgentPathFollowerVirtual;  // [1] +0x18, [2] +0x20, [4] +0x30
		const auto& flight = RE::VTABLE_MovementAgentPathFollowerFlight;
		const auto  flightStub = Slot(flight[1], 3);
		const auto  stubOk = !InGame(flightStub) || (Bytes(flightStub, kFlightStub.size()) == kFlightStub &&
														Rel32Target(flightStub + kFlightStub.size() - 1) == kFlightUpdate.address());
		if (!SlotIs(virt[1], 3, kVirtualUpdate) || !SlotIs(virt[2], 2, kVirtualSetPath) ||
			!SlotIs(virt[2], 6, kVirtualClearPath) || !SlotIs(virt[4], 1, kVirtualSetFollowing) ||
			!SlotIs(flight[2], 2, kFlightSetPath) || !SlotIs(flight[2], 6, kFlightClearPath) || !stubOk) {
			return { Outcome::kFailed, "a path follower's vtable points at other game code (another game build?), "
									   "nothing patched" };
		}
		if (!LocksAt(kVirtualSetPath.address(), 0x18) || !LocksAt(kVirtualClearPath.address(), 0x18) ||
			!LocksAt(kVirtualSetFollowing.address(), 0x08) || !LocksAt(kFlightSetPath.address(), 0x80) ||
			!LocksAt(kFlightClearPath.address(), 0x80)) {
			return { Outcome::kFailed, "a path writer takes its lock at another offset than expected, nothing patched" };
		}
		const auto virtualLocks = Bytes(kVirtualUpdate.address(), 0x40).find(kLockingUpdate) != std::string_view::npos;
		if ((!virtualLocks && Bytes(kVirtualUpdate.address(), kVirtualUpdateHead.size()) != kVirtualUpdateHead) ||
			Bytes(kFlightUpdate.address(), kFlightUpdateHead.size()) != kFlightUpdateHead) {
			return { Outcome::kFailed, "a follower's update does not start with the expected instructions (another plugin?), "
									   "nothing patched" };
		}
		if (const auto why = HookEntry(kFlightUpdate.address(), kFlightUpdateHead, &FlightUpdate, g_flightUpdate);
			!why.empty()) {
			return { Outcome::kFailed, std::format("the flight follower's update: {}, nothing patched"sv, why) };
		}
		if (virtualLocks) {
			return { Outcome::kActive, "the flight follower's update takes its lock; this game's virtual follower already "
									   "locks its own" };
		}
		if (const auto why = HookEntry(kVirtualUpdate.address(), kVirtualUpdateHead, &VirtualUpdate, g_virtualUpdate);
			!why.empty()) {
			return { Outcome::kFailed,
				std::format("the virtual follower's update: {}; only the flight follower's update takes its lock"sv, why) };
		}
		return { Outcome::kActive, "the virtual and the flight follower's updates take the follower's lock" };
	}

	const bool kRegistered = Register({
		.key = "PathFollowerRace",
		.install = Install,
		.trampoline = HookSize(kVirtualUpdateHead) + HookSize(kFlightUpdateHead),
	});
}
