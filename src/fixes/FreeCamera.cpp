// SkyrimSE+0CC90CF, +083D0CD or VCRUNTIME140's memmove (1.6.1170) under the player's combat-group update, while the
// free camera is on (tfc, or a mod's photo / preview camera).
// Cause: the combat manager job updates the player's combat group on a worker thread every frame. In free camera,
// Main's player step runs the same update on the main thread too. Neither takes a lock.
// Fix: nop the main thread's call. The job's call stays, so the free camera behaves like every other camera.
// Install finds the call by its target; anything else there stays unpatched.

#include "fix.h"

namespace FreeCamera
{
	namespace
	{
		// the player update Main runs only in free camera
		constexpr REL::RelocationID kFreeCameraUpdate{ 39378, 40450 };
		constexpr REL::RelocationID kGroupUpdate{ 40235, 41238 };  // the player's combat-group update
		constexpr std::size_t       kSpan = 0x220;                 // the free-camera update is 0x200-0x220 long
		constexpr auto              kNop5 = "\x0F\x1F\x44\x00\x00"sv;
	}

	Result Install()
	{
		const auto start = kFreeCameraUpdate.address();
		const auto call = FindCall(start, kSpan, kGroupUpdate.address());
		if (!call.site) {
			return { Outcome::kFailed, std::format("free-camera player update: {}, nothing patched", call.Why()) };
		}

		REL::safe_write(call.site, kNop5.data(), kNop5.size());
		return { Outcome::kActive,
			std::format(
				"the free-camera player update no longer runs the combat-group upkeep on the main thread (+0x{:X})",
				call.site - start) };
	}

	const bool kRegistered = Register({
		.key = "FreeCamera",
		.oldDll = "FreeCamCombatGroupFix.dll",
		.install = Install,
	});
}
