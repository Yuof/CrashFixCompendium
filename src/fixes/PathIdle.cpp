// SkyrimSE+0CEC680 (1.6.1170), MovementMessagePlayIdle's constructor reads null + 0xE8, right after loading a save.
// Cause: MovementAgentPathFollowerStandard state 14 waits to play a prepared start / stop / turn idle, then enters
// PlayIdle without checking it. SaveGame / LoadGame keep the state but not the idle.
// Fix: give state 14 the check states 13 and 15 already have. Without its idle, reset it the way SetPath does (drop
// the idle, state 1).
// The field offsets are the same in 1.5.97, 1.6.1170 and 1.7.104. Install looks for the instructions that use them;
// a build where they differ stays unpatched.

#include "fix.h"

namespace PathIdle
{
	namespace
	{
		constexpr std::ptrdiff_t kLock = 0x110;      // BSReadWriteLock taken by Update / SetPath / ClearPath
		constexpr std::ptrdiff_t kState = 0x118;     // std::uint32_t
		constexpr std::ptrdiff_t kIdleData = 0x130;  // smart pointer to the prepared idle
		constexpr std::ptrdiff_t kStateObj = 0x148;  // current state object
		constexpr std::uint32_t  kWaitIdle = 14;
		constexpr std::uint32_t  kFollowPath = 1;

		constexpr REL::RelocationID kUpdate{ 88300, 90711 };
		constexpr REL::RelocationID kEnterPlayIdle{ 88339, 90750 };
		constexpr REL::RelocationID kLoadGame{ 88328, 90739 };
		constexpr REL::RelocationID kClearIdle{ 88361, 90780 };

		using ClearIdle_t = void (*)(void*);
		ClearIdle_t g_clearIdle = nullptr;  // SetPath calls it before it sets state 1

		using Update_t = std::uintptr_t (*)(void*, void*, void*);
		Update_t g_update = nullptr;

		struct Expect
		{
			REL::RelocationID id;
			std::size_t       span;
			std::string_view  bytes;
			std::string_view  what;
		};

		// the instructions that use the offsets above, identical in 1.5.97, 1.6.1170 and 1.7.104
		constexpr Expect kExpect[]{
			{ kUpdate, 0xA00, "\x48\x81\xC1\xF8\x00\x00\x00"sv, "Update: lock at +0x110"sv },
			{ kUpdate, 0xA00, "\x49\x63\x86\x00\x01\x00\x00"sv, "Update: state at +0x118"sv },
			{ kUpdate, 0xA00, "\x49\x8B\x86\x18\x01\x00\x00"sv, "Update: idle at +0x130"sv },
			{ kEnterPlayIdle, 0xB0, "\x4C\x8D\x87\x30\x01\x00\x00"sv, "PlayIdle entry: idle at +0x130"sv },
			{ kClearIdle, 0x90, "\x48\x8B\x9F\x30\x01\x00\x00"sv, "ClearIdle: idle at +0x130"sv },
			{ kLoadGame, 0x330, "\x89\x85\x18\x01\x00\x00"sv, "LoadGame: state at +0x118"sv },
		};

		// state 14's handler: cmp [r14+0x148],0 / jne rel32 / lea rcx,[r14-0x18] / lea rdx,[rsp+imm8] (AE) or
		// lea rdx,[rbp+disp8] (SE) / call EnterPlayIdle
		bool HasState14Handler()
		{
			constexpr auto      head = "\x41\x83\xBE\x48\x01\x00\x00\x00\x0F\x85"sv;
			constexpr auto      leaRcx = "\x49\x8D\x4E\xE8"sv;
			constexpr std::array leaRdx{ "\x48\x8D\x54\x24"sv, "\x48\x8D\x55"sv };
			constexpr std::size_t rel32 = 4, enough = 16;

			const auto code = Bytes(kUpdate.address(), 0xA00);
			const auto target = kEnterPlayIdle.address();
			for (auto at = code.find(head); at != code.npos; at = code.find(head, at + 1)) {
				const auto next = at + head.size() + rel32;
				if (next + enough > code.size() || !code.substr(next).starts_with(leaRcx)) {
					continue;
				}
				const auto afterRcx = code.substr(next + leaRcx.size());
				for (const auto lea : leaRdx) {
					const auto call = reinterpret_cast<std::uintptr_t>(afterRcx.data()) + lea.size() + 1;
					if (afterRcx.starts_with(lea) && *reinterpret_cast<const std::uint8_t*>(call) == 0xE8 &&
						Rel32Target(call) == target) {
						return true;
					}
				}
			}
			return false;
		}

		std::optional<std::string_view> SelfCheck()
		{
			for (const auto& e : kExpect) {
				if (Bytes(e.id.address(), e.span).find(e.bytes) == std::string_view::npos) {
					return e.what;
				}
			}
			if (!HasState14Handler()) {
				return "Update: state 14 entering PlayIdle"sv;
			}
			return std::nullopt;
		}

		template <class T>
		T& At(void* a_agent, std::ptrdiff_t a_off)
		{
			return *reinterpret_cast<T*>(static_cast<std::byte*>(a_agent) + a_off);
		}

		bool LostIdle(void* a_agent)
		{
			return At<std::uint32_t>(a_agent, kState) == kWaitIdle && !At<void*>(a_agent, kIdleData);
		}

		// vfunc 2 of the agent's +0x18 subobject
		std::uintptr_t UpdateThunk(void* a_sub, void* a_2, void* a_3)
		{
			auto* agent = static_cast<std::byte*>(a_sub) - 0x18;
			if (LostIdle(agent)) {
				auto& lock = At<RE::BSReadWriteLock>(agent, kLock);
				lock.LockForWrite();
				const bool reset = LostIdle(agent);  // checked again under the lock
				if (reset) {
					g_clearIdle(agent);
					At<std::uint32_t>(agent, kState) = kFollowPath;
					At<void*>(agent, kStateObj) = nullptr;
				}
				lock.UnlockForWrite();

				if (reset) {
					logger::info("[PathIdle] path follower {:X}: state 14 without its idle -> state 1"sv,
						reinterpret_cast<std::uintptr_t>(agent));
				}
			}
			return g_update(a_sub, a_2, a_3);
		}
	}

	Result Install()
	{
		if (const auto missing = SelfCheck()) {
			return { Outcome::kFailed, std::format("{} not found in this build, nothing patched", *missing) };
		}

		g_clearIdle = reinterpret_cast<ClearIdle_t>(kClearIdle.address());
		REL::Relocation<std::uintptr_t> vtbl{ RE::VTABLE_MovementAgentPathFollowerStandard[1] };
		g_update = reinterpret_cast<Update_t>(vtbl.write_vfunc(2, UpdateThunk));
		return { Outcome::kActive, "hooked MovementAgentPathFollowerStandard::Update" };
	}

	const bool kRegistered = Register({
		.key = "PathIdle",
		.oldDll = "PathIdleCrashFix.dll",
		.install = Install,
	});
}
