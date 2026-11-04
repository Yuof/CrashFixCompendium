// SkyrimSE+0454CEF (1.6.1170, AE 28181), `call [r8+0x08]` executes a small address (0x2) on a job thread, with
// BGSProcedurePursue and a BGSPackageDataBool among the objects. An NPC starts to pursue the player trespassing in
// his home.
// Cause: the engine builds the trespass package's procedure tree itself (AE 29666): one Pursue whose parameters map
// to package data uids 0 -> 0, 1 -> 1, 3 -> 2. Mapping parameter 3 grows the uid array from 2 to 4 slots and leaves
// the new ones uninitialized, so parameter 2 holds leftover memory: usually 0 or 1 (both usable as a location),
// sometimes 2. Uid 2 is the package's Bool. Pursue reads parameter 2 as a location, steps back 8 bytes to an
// interface the Bool lacks, and calls through whatever lies there.
// Fix: after the builder returns, set parameter 2 (UntilWithinLocation) to a uid the package has no data for. Pursue
// then runs without an "until" location: the owner follows the player until the player leaves the cell. With the
// leftover 0 / 1 he gave up as soon as the player left his view cone.
// Only a Pursue with exactly 4 uid slots is touched. All three callers of the builder get the patch; Install finds
// each call by its target, and anything else there leaves that caller unpatched.

#include "fix.h"

namespace TrespassPursue
{
	namespace
	{
		struct Caller
		{
			REL::RelocationID id;
			std::size_t       span;  // at least the caller's size
			std::string_view  name;
			bool              aeOnly;
		};
		constexpr Caller kCallers[] = {
			{ REL::RelocationID(36420, 37414), 0x140, "trespass package for an actor"sv, false },
			// AE inlines the first builder here; SE's 36858 calls 36420 instead, so the patch above covers it
			{ REL::RelocationID(0, 37882), 0x490, "trespass package (second builder)"sv, true },
			{ REL::RelocationID(41461, 42530), 0x110, "tree rebuild by package kind"sv, false },
		};
		constexpr REL::RelocationID kBuildTree{ 28868, 29666 };
		constexpr std::uint8_t      kBoolUid = 2, kNoDataUid = 0xFF;  // the package's data uids are 0, 1, 2

		using BuildTree_t = void (*)(void*);
		BuildTree_t g_target[std::size(kCallers)] = {};

		std::atomic<std::uint64_t> g_built{ 0 }, g_fixed{ 0 };

		// TESCustomPackageData +0x20 = the tree item, item +0x10 = its procedure, procedure +0x08 = the uid array
		// (BSTArray: data, capacity, size)
		void FixParam2(void* a_data)
		{
			auto* item =
				a_data ? *reinterpret_cast<std::uint8_t**>(static_cast<std::uint8_t*>(a_data) + 0x20) : nullptr;
			auto* proc = item ? *reinterpret_cast<std::uint8_t**>(item + 0x10) : nullptr;
			if (!proc || *reinterpret_cast<std::uintptr_t*>(proc) != RE::VTABLE_BGSProcedurePursue[0].address()) {
				return;
			}
			auto* slots = *reinterpret_cast<std::uint8_t**>(proc + 0x08);  // 8 bytes a slot, the uid in the low byte
			if (!slots || *reinterpret_cast<std::uint32_t*>(proc + 0x18) != 4) {
				return;
			}

			const auto built = ++g_built;
			const bool wasBool = slots[2 * 8] == kBoolUid;
			slots[2 * 8] = kNoDataUid;
			const auto fixed = wasBool ? ++g_fixed : g_fixed.load();
			if (wasBool || Milestone(built)) {
				logger::info("[TrespassPursue] parameter 2 set to no data "
							 "({} so far, {} of them had been left at the Bool's uid)"sv,
					built, fixed);
			}
		}

		template <std::size_t I>
		void BuildTreeThunk(void* a_data)
		{
			g_target[I](a_data);
			FixParam2(a_data);
		}
		constexpr BuildTree_t kThunks[] = { BuildTreeThunk<0>, BuildTreeThunk<1>, BuildTreeThunk<2> };
		static_assert(std::size(kThunks) == std::size(kCallers));
	}

	Result Install()
	{
		std::uintptr_t sites[std::size(kCallers)] = {};
		for (std::size_t i = 0; i < std::size(kCallers); ++i) {
			const auto& caller = kCallers[i];
			if (caller.aeOnly && !REL::Module::IsAE()) {
				continue;
			}
			const auto call = FindCall(caller.id.address(), caller.span, kBuildTree.address());
			sites[i] = call.site;
			if (!call.site) {
				logger::error("[TrespassPursue] failed: the {} stays unpatched ({})"sv, caller.name, call.Why());
			}
		}

		if (std::ranges::all_of(sites, [](std::uintptr_t a_site) { return a_site == 0; })) {
			return { Outcome::kFailed, "no caller of the tree builder as expected, nothing patched" };
		}

		std::string patched;
		for (std::size_t i = 0; i < std::size(kCallers); ++i) {
			if (sites[i]) {
				g_target[i] = reinterpret_cast<BuildTree_t>(SKSE::GetTrampoline().write_call<5>(sites[i], kThunks[i]));
				patched += std::format("{}{} (+0x{:X})", patched.empty() ? "" : ", ", kCallers[i].name,
					sites[i] - kCallers[i].id.address());
			}
		}
		return { Outcome::kActive,
			std::format("patched the tree builder's calls in the {}: the trespass escort gets no 'until' place",
				patched) };
	}

	const bool kRegistered = Register({
		.key = "TrespassPursue",
		.oldDll = "TrespassPursueCrashFix.dll",
		.install = Install,
		.trampoline = std::size(kCallers) * kAbsJumpSize,  // a write_call<5> per caller
	});
}
