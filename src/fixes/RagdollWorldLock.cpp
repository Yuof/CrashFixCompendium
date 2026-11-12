// SkyrimSE+0B6AA54 (1.6.1170), `mov rax, [rdx]` with rdx = 0, with an hkpConstraintInstance ("NPC COM" /
// "NPC L Thigh") and an hkbRagdollDriver among the objects.
// Cause: the ragdoll driver adds a joint back to the physics world and then switches it on or off. While the world's
// counter of changes in progress (hkpWorld +0xF8, criticalOperationsLockCount) is not 0, the add is only queued and
// the joint has no owner yet. The switch reads the owner.
// The counter is most likely left wrong by a race. hkpEntity::updateBroadPhaseAfterMove and
// hkpPhantom::updateBroadPhase raise and lower it without the world lock. The main thread calls them for a ragdoll's
// saved pose and for the loaded-area bound, while the physics update changes the counter on its worker. One lost
// write leaves the counter at 1 or -1, and from then on the world queues every change.
// Fix: both functions run under the world's write lock when it is free or already this thread's. When another thread
// holds it, they queue their update (operations 0x17 / 0x12), as they do themselves for a busy world. They never
// wait for the lock.
// One caller keeps the old behaviour: the world's queue runner, while the lock is busy. Queueing there would hand
// it the same update again at once. Install checks both functions' first bytes and finds the runner's call to each.

#include "fix.h"

#include <intrin.h>

namespace RagdollWorldLock
{
	namespace
	{
		constexpr REL::RelocationID kUpdateAfterMove{ 60162, 60918 };  // hkpEntity::updateBroadPhaseAfterMove
		constexpr REL::RelocationID kPhantomUpdate{ 60957, 61827 };    // hkpPhantom::updateBroadPhase
		constexpr REL::RelocationID kQueueOperation{ 60580, 61428 };   // hkpWorld::queueOperation
		constexpr REL::RelocationID kRunQueue{ 61313, 62204 };         // hkpWorldOperationQueue: runs the pending operations
		constexpr std::size_t       kRunQueueSpan = 0x8B0;             // to the next function on SE, 1.6.1170 and 1.7.104

		// position-independent prologues, copied into the stubs: mov [rsp+18h],rbx / push rdi / sub rsp,40h /
		// mov rbx,[rcx+10h], and push rbp / push rbx / push rsi / lea rbp,[rsp-47h] / sub rsp,90h
		constexpr auto kUpdateAfterMoveHead = "\x48\x89\x5C\x24\x18\x57\x48\x83\xEC\x40\x48\x8B\x59\x10"sv;
		constexpr auto kPhantomUpdateHead = "\x40\x55\x53\x56\x48\x8D\x6C\x24\xB9\x48\x81\xEC\x90\x00\x00\x00"sv;

		constexpr std::uint8_t  kOpUpdateAfterMove = 0x17;
		constexpr std::uint8_t  kOpPhantomUpdate = 0x12;
		constexpr std::uint32_t kOneWriter = 0x80000001;  // BSReadWriteLock state with one writer

		// BSReadWriteLock's members are private in CommonLib
		struct LockState
		{
			volatile std::uint32_t writerThread;
			volatile long          state;
		};
		static_assert(sizeof(LockState) == sizeof(RE::BSReadWriteLock));

		// hkpWorld's operation record: the type, the object, one argument (0x28 bytes, the queue copies all of it)
		struct Operation
		{
			std::uint8_t  type;
			std::uint8_t  pad01[7];
			void*         object;
			const void*   arg;
			std::uint64_t pad18[2];
		};
		static_assert(sizeof(Operation) == 0x28);

		using UpdateAfterMove_t = void (*)(RE::hkpEntity*);
		using PhantomUpdate_t = void (*)(RE::hkpPhantom*, const RE::hkAabb*);
		using QueueOperation_t = void (*)(RE::hkpWorld*, const Operation*);
		UpdateAfterMove_t          g_updateAfterMove = nullptr;  // the originals, through the stubs
		PhantomUpdate_t            g_phantomUpdate = nullptr;
		std::uintptr_t             g_fromRunQueue[2]{};          // the queue runner's return addresses into them
		std::atomic<std::uint64_t> g_locked{ 0 };
		std::atomic<std::uint64_t> g_queued{ 0 };

		// Runs a_now under a_world's write lock when this thread owns it or can take it at once. Otherwise it queues
		// a_op. The queue runner (a_fromRunQueue) is the exception: it runs a_now as before.
		template <class Now>
		void WithWorldLock(RE::hkpWorld* a_world, const Operation& a_op, bool a_fromRunQueue, Now a_now)
		{
			auto* bhk = a_world ? static_cast<RE::ahkpWorld*>(a_world)->userData : nullptr;
			if (!bhk) {
				a_now();
				return;
			}
			auto&      lock = reinterpret_cast<LockState&>(bhk->worldLock);
			const auto me = GetCurrentThreadId();
			if (lock.writerThread == me) {
				a_now();
				return;
			}
			// LockForWrite's own step, tried once instead of in a loop
			if (_InterlockedCompareExchange(&lock.state, static_cast<long>(kOneWriter), 0) == 0) {
				lock.writerThread = me;
				a_now();
				bhk->worldLock.UnlockForWrite();
				if (const auto n = ++g_locked; Milestone(n)) {
					logger::info("[RagdollWorldLock] {} broadphase updates from outside the physics update ran under the "
								 "world lock"sv,
						n);
				}
				return;
			}
			if (a_fromRunQueue) {
				a_now();
				return;
			}
			static REL::Relocation<QueueOperation_t> queue{ kQueueOperation };
			queue(a_world, &a_op);
			if (const auto n = ++g_queued; Milestone(n)) {
				logger::info("[RagdollWorldLock] {} broadphase updates queued while another thread held the world lock"sv,
					n);
			}
		}

		void UpdateAfterMoveThunk(RE::hkpEntity* a_entity)
		{
			const Operation op{ .type = kOpUpdateAfterMove, .object = a_entity };
			const bool fromRunQueue = reinterpret_cast<std::uintptr_t>(_ReturnAddress()) == g_fromRunQueue[0];
			WithWorldLock(a_entity->world, op, fromRunQueue, [&] { g_updateAfterMove(a_entity); });
		}

		void PhantomUpdateThunk(RE::hkpPhantom* a_phantom, const RE::hkAabb* a_aabb)
		{
			const Operation op{ .type = kOpPhantomUpdate, .object = a_phantom, .arg = a_aabb };
			const bool fromRunQueue = reinterpret_cast<std::uintptr_t>(_ReturnAddress()) == g_fromRunQueue[1];
			WithWorldLock(a_phantom->world, op, fromRunQueue, [&] { g_phantomUpdate(a_phantom, a_aabb); });
		}

		template <class F>
		F Detour(std::uintptr_t a_entry, std::string_view a_head, F a_hook)
		{
			auto* stub = static_cast<std::uint8_t*>(SKSE::GetTrampoline().allocate(a_head.size() + kAbsJumpSize));
			WriteOriginal(stub, a_head, a_entry);
			const auto patch = EntryPatch(a_head.size(), reinterpret_cast<std::uintptr_t>(a_hook));
			REL::safe_write(a_entry, patch.data(), patch.size());
			return reinterpret_cast<F>(stub);
		}
	}

	Result Install()
	{
		const auto updateAfterMove = kUpdateAfterMove.address();
		const auto phantomUpdate = kPhantomUpdate.address();
		if (Bytes(updateAfterMove, kUpdateAfterMoveHead.size()) != kUpdateAfterMoveHead ||
			Bytes(phantomUpdate, kPhantomUpdateHead.size()) != kPhantomUpdateHead) {
			return { Outcome::kFailed, "a broadphase update's first bytes differ (another plugin?), nothing patched" };
		}
		const auto runQueue = kRunQueue.address();
		const auto bodyCall = FindCall(runQueue, kRunQueueSpan, updateAfterMove);
		const auto phantomCall = FindCall(runQueue, kRunQueueSpan, phantomUpdate);
		if (!bodyCall.site || !phantomCall.site) {
			return { Outcome::kFailed, std::format("the world's queue runner: {}, nothing patched",
										   (bodyCall.site ? phantomCall : bodyCall).Why()) };
		}
		g_fromRunQueue[0] = bodyCall.site + 5;
		g_fromRunQueue[1] = phantomCall.site + 5;
		g_updateAfterMove = Detour(updateAfterMove, kUpdateAfterMoveHead, &UpdateAfterMoveThunk);
		g_phantomUpdate = Detour(phantomUpdate, kPhantomUpdateHead, &PhantomUpdateThunk);
		return { Outcome::kActive, "the two broadphase updates take the physics world lock, or queue while it is held" };
	}

	const bool kRegistered = Register({
		.key = "RagdollWorldLock",
		.install = Install,
		.trampoline = 2 * (kAbsJumpSize + 16),  // two stubs: a prologue of up to 16 bytes and a jump back
	});
}
