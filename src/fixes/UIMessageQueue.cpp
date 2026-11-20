// Crashes in whatever reads a menu's name next ("Fader Menu", "HUD Menu", ...): the name has an invalid length or text.
// Cause: UIMessageQueue::AddMessage takes no lock, and job threads queue UI messages too. Two threads that read the
// same pool count (+0x378) fill the same pooled message, and each releases the menu name it held. The string pool
// then frees a name that the UI still uses. ProcessCommands resets the count without a lock as well.
// Fix: both functions run under one lock. Install checks their first bytes and the pool-count instruction, which are
// the same in 1.5.97, 1.6.1170 and 1.7.104. A jump that another plugin put at an entry is chained, so its code runs
// inside the lock. Any other code at an entry stays unpatched.

#include "fix.h"
#include "hook.h"

namespace UIMessageQueue
{
	namespace
	{
		struct Target
		{
			REL::RelocationID id;
			std::string_view  prologue;  // the function's first bytes in the game's code
			std::size_t       markerAt;
			std::string_view  marker;  // the pool-count instruction
			std::string_view  name;
		};

		constexpr Target kAdd{
			REL::RelocationID(13530, 13631),
			// push rsi/rdi/r14; sub rsp,30h; mov [rsp+20h],-2
			"\x40\x56\x57\x41\x56\x48\x83\xEC\x30\x48\xC7\x44\x24\x20\xFE\xFF\xFF\xFF"sv,
			0x28,
			"\x8B\x81\x78\x03\x00\x00"sv,  // mov eax,[rcx+378h]
			"UIMessageQueue::AddMessage"sv
		};
		constexpr Target kProcess{
			REL::RelocationID(80059, 82167),
			// mov [rsp+10h],rbx; mov [rsp+18h],rbp; push rsi; sub rsp,20h
			"\x48\x89\x5C\x24\x10\x48\x89\x6C\x24\x18\x56\x48\x83\xEC\x20"sv,
			0x16,
			"\x39\x99\x78\x03\x00\x00"sv,  // cmp [rcx+378h],ebx
			"UIMessageQueue::ProcessCommands"sv
		};

		using AddMessage_t = void (*)(RE::UIMessageQueue*, const RE::BSFixedString&, RE::UI_MESSAGE_TYPE,
			RE::IUIMessageData*);
		using ProcessCommands_t = void (*)(RE::UIMessageQueue*);
		AddMessage_t      g_addMessage = nullptr;
		ProcessCommands_t g_processCommands = nullptr;
		SafetyHookInline  g_addHook;
		SafetyHookInline  g_processHook;

		std::recursive_mutex g_lock;

		void AddMessageThunk(RE::UIMessageQueue* a_queue, const RE::BSFixedString& a_menu, RE::UI_MESSAGE_TYPE a_type,
			RE::IUIMessageData* a_data)
		{
			const std::scoped_lock lock{ g_lock };
			g_addMessage(a_queue, a_menu, a_type, a_data);
		}

		void ProcessCommandsThunk(RE::UIMessageQueue* a_queue)
		{
			const std::scoped_lock lock{ g_lock };
			g_processCommands(a_queue);
		}

		enum class Entry
		{
			kOriginal,
			kJump,  // a jmp rel32 or a jmp [rip] from another plugin
			kUnknown
		};

		// Every case needs the pool-count instruction. It lies past the bytes that an entry jump replaces.
		Entry Classify(const Target& a_t)
		{
			const auto entry = a_t.id.address();
			if (Bytes(entry + a_t.markerAt, a_t.marker.size()) != a_t.marker) {
				return Entry::kUnknown;
			}
			if (Bytes(entry, a_t.prologue.size()) == a_t.prologue) {
				return Entry::kOriginal;
			}
			const bool jump = *reinterpret_cast<const std::uint8_t*>(entry) == 0xE9 || IsAbsJump(entry);
			return jump ? Entry::kJump : Entry::kUnknown;
		}

		std::string_view Describe(Entry a_e)
		{
			return a_e == Entry::kOriginal ? "original code"sv : "chained to another plugin's jump at its entry"sv;
		}
	}

	Result Install()
	{
		const auto add = Classify(kAdd);
		const auto process = Classify(kProcess);
		if (add == Entry::kUnknown || process == Entry::kUnknown) {
			return { Outcome::kFailed, std::format("{}: unexpected code at its entry, nothing patched",
										   add == Entry::kUnknown ? kAdd.name : kProcess.name) };
		}

		if (const auto why = HookEntry(g_addHook, kAdd.id.address(), &AddMessageThunk, g_addMessage); !why.empty()) {
			return { Outcome::kFailed, std::format("{}: {}, nothing patched", kAdd.name, why) };
		}
		if (const auto why = HookEntry(g_processHook, kProcess.id.address(), &ProcessCommandsThunk, g_processCommands);
			!why.empty()) {
			return { Outcome::kFailed, std::format("{}: {}; only {} runs under the lock", kProcess.name, why, kAdd.name) };
		}
		return { Outcome::kActive, std::format("{} ({}) and {} ({}) both run under one lock", kAdd.name, Describe(add),
									   kProcess.name, Describe(process)) };
	}

	const bool kRegistered = Register({
		.key = "UIMessageQueue",
		.oldDll = "UIMessageQueueRaceFix.dll",
		.install = Install,
	});
}
