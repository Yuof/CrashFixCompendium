// SkyrimSE+0D1EEFE (1.6.1170), `lock xadd [rcx+8]` on a job thread: NiTObjectArray::SetAt releases a small number
// (1, 0x10000) as an object, under NiNode::AttachChild <- BGSDecalNode's constructor <- BGSDecalManager::ApplyDecal.
// Two projectiles or blood drops hit one tree or actor in the same frame, on its first decals.
// Cause: the game applies skinned decals on job threads. With no skinned BGSDecalNode under the object's root, the
// walk (AE 15234 -> 15252 -> 15206) builds one and attaches it, without a lock. Two workers both find none and
// attach at once. NiTObjectArray's grow stores the new capacity before the new buffer, so the second attach writes
// one slot past the old buffer and releases what lay there.
// Fix: run ApplyDecal's call of the walk under one lock. A thread that waits 100 ms goes on without it (logged), so
// an unknown lock order can delay a decal but never hang the game. Install finds the call by its target; anything
// else there stays unpatched.

#include "fix.h"

namespace SkinnedDecal
{
	namespace
	{
		constexpr REL::RelocationID kCaller{ 15029, 15203 };  // BGSDecalManager::ApplyDecal
		constexpr REL::RelocationID kTarget{ 15060, 15234 };  // the attach-node walk over the hit object
		constexpr std::size_t       kSpan = 0x14D;            // ApplyDecal's size; the call sits at +0x12A on SE and AE
		constexpr auto              kMaxWait = 100ms;

		using Target_t = std::uint64_t (*)(void*, void*);
		Target_t g_target = nullptr;  // what the call reached before: another plugin may hook it too

		std::recursive_timed_mutex g_lock;  // recursive: the walk can re-enter on the same thread
		std::atomic<std::uint64_t> g_timeouts{ 0 };

		std::uint64_t TargetThunk(void* a_object, void* a_data)
		{
			const std::unique_lock lock{ g_lock, kMaxWait };
			if (!lock.owns_lock()) {
				if (const auto n = ++g_timeouts; Milestone(n)) {
					logger::warn("[SkinnedDecal] a decal went on unlocked after {} ms ({} so far)"sv, kMaxWait.count(),
						n);
				}
			}
			return g_target(a_object, a_data);
		}
	}

	Result Install()
	{
		const auto start = kCaller.address();
		const auto call = FindCall(start, kSpan, kTarget.address());
		if (!call.site) {
			return { Outcome::kFailed, std::format("{}, nothing patched", call.Why()) };
		}

		g_target = reinterpret_cast<Target_t>(SKSE::GetTrampoline().write_call<5>(call.site, TargetThunk));
		return { Outcome::kActive,
			std::format("patched ApplyDecal's call of the attach-node walk (+0x{:X}): "
						"skinned decal nodes are created under one lock",
				call.site - start) };
	}

	const bool kRegistered = Register({
		.key = "SkinnedDecal",
		.oldDll = "SkinnedDecalCrashFix.dll",
		.install = Install,
		.trampoline = kAbsJumpSize,  // the call's write_call<5>
	});
}
