// SkyrimSE+0A8497F `call [r10+0x08]` or +05434A0 `call [r10+0x10]` (1.6.1170) in an actor animation job, with
// hkaFootPlacementIkSolver in RDI. Starts once more than 50 characters are loaded nearby.
// Cause: BGSFootIkRaycastInterfaceDB starts with 50 entries and hands out pointers into its array. A lookup that
// finds no free slot grows the array by 50. A grow past the capacity reallocates the array and frees the block other
// characters still cast through.
// Fix: the constructor's resize(50) becomes resize(1024), then resize(50): capacity 1024, size 50. Later grows stay
// inside that block. No per-frame cost, 56 KB of memory. Install finds the call by its target and the
// `mov edx, 50; mov rcx, rbx` before it; anything else there stays unpatched.

#include "fix.h"

namespace FootIkRaycast
{
	namespace
	{
		constexpr REL::RelocationID kCreate{ 31978, 32736 };       // the resize(50) call: SE +0x110, AE +0x109
		constexpr std::size_t       kSpan = 0x140;                 // at least the function's size (SE 0x130, AE 0x13A)
		constexpr REL::RelocationID kResize{ 32017, 32769 };       // BSTArray<CachedRaycastData>::resize (element 0x38)
		constexpr REL::RelocationID kSingleton{ 520993, 407511 };  // the pointer the foot-IK lookup reads, written last
		constexpr std::uint32_t     kReserve = 1024;

		// mov edx, 50; mov rcx, rbx
		constexpr auto kBeforeCall = "\xBA\x32\x00\x00\x00\x48\x8B\xCB"sv;

		using Resize_t = bool (*)(void*, std::uint32_t);
		Resize_t g_resize = nullptr;

		struct Array  // BSTArray at table + 0x40
		{
			void*         data;
			std::uint32_t capacity;
			std::uint32_t pad;
			std::uint32_t size;
		};

		// Runs inside the constructor, during game init: no character exists yet, so nothing reads the table.
		bool ResizeThunk(void* a_array, std::uint32_t a_size)
		{
			g_resize(a_array, kReserve);
			const bool result = g_resize(a_array, a_size);

			const auto* array = static_cast<const Array*>(a_array);
			if (array->capacity >= kReserve) {
				logger::info("[FootIkRaycast] table created: capacity {} reserved, size {}"sv, array->capacity,
					array->size);
			} else {
				logger::warn("[FootIkRaycast] table created without the reserve: capacity {}, size {}"sv,
					array->capacity, array->size);
			}
			return result;
		}
	}

	Result Install()
	{
		// changing it now could free a block in use
		if (*reinterpret_cast<void* const*>(kSingleton.address())) {
			return { Outcome::kFailed, "the table already exists, nothing patched" };
		}

		const auto start = kCreate.address();
		const auto call = FindCall(start, kSpan, kResize.address());
		if (!call.site) {
			return { Outcome::kFailed, std::format("{}, nothing patched", call.Why()) };
		}
		if (Bytes(call.site - kBeforeCall.size(), kBeforeCall.size()) != kBeforeCall) {
			return { Outcome::kFailed, "the resize is not resize(50) on the table's array, nothing patched" };
		}

		g_resize = reinterpret_cast<Resize_t>(SKSE::GetTrampoline().write_call<5>(call.site, ResizeThunk));
		return { Outcome::kActive,
			std::format("patched the foot-IK raycast table's resize(50) (+0x{:X}): room for {} entries",
				call.site - start, kReserve) };
	}

	const bool kRegistered = Register({
		.key = "FootIkRaycast",
		.install = Install,
		.trampoline = kAbsJumpSize,  // one write_call<5>
	});
}
