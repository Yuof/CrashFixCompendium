// Crashes in whatever reads a menu's name next ("Fader Menu", "HUD Menu", ...): garbage length or text.
// Cause: UIMessageQueue::AddMessage takes the next of its 64 pooled messages by reading the pool count (+0x378),
// assigns the menu name (releasing the name the slot held) and increments the count. No lock, although job threads
// queue UI messages too. Two threads take the same slot and both release its old name; the string pool frees a name
// the UI still uses. ProcessCommands resets the count without a lock as well.
// Fix: run both functions under one lock.
// Both are the same code in 1.5.97, 1.6.1170 and 1.7.104; Install checks their prologues and the pool-count
// instruction. An entry another plugin already detoured (jmp [rip] or a 5-byte jmp) gets chained, so that detour
// runs inside the lock. Anything else stays unpatched.

#include "fix.h"

namespace UIMessageQueue
{
	namespace
	{
		struct Target
		{
			REL::RelocationID id;
			std::string_view  prologue;  // copied to the trampoline: no rip-relative bytes
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
			kJmpAbs,  // FF 25 00000000 <abs64>
			kJmpRel,  // E9 <rel32>
			kUnknown
		};

		Entry Classify(const Target& a_t)
		{
			const auto entry = a_t.id.address();
			if (Bytes(entry, a_t.prologue.size()) == a_t.prologue) {
				const bool marked = Bytes(entry + a_t.markerAt, a_t.marker.size()) == a_t.marker;
				return marked ? Entry::kOriginal : Entry::kUnknown;
			}
			if (IsAbsJump(entry)) {
				return Entry::kJmpAbs;
			}
			return *reinterpret_cast<const std::uint8_t*>(entry) == 0xE9 ? Entry::kJmpRel : Entry::kUnknown;
		}

		std::string_view Describe(Entry a_e)
		{
			return a_e == Entry::kOriginal ? "original code"sv : "chained to the detour already at its entry"sv;
		}

		// Routes the entry to a_hook and sets a_next to what a_hook must call. Sets a_next before it changes the entry.
		template <class F>
		void Detour(const Target& a_t, Entry a_e, F a_hook, F& a_next)
		{
			auto&      trampoline = SKSE::GetTrampoline();
			const auto entry = a_t.id.address();
			const auto hook = reinterpret_cast<std::uintptr_t>(a_hook);
			switch (a_e) {
			case Entry::kOriginal:
				{
					const auto size = a_t.prologue.size();
					auto*      stub = static_cast<std::uint8_t*>(trampoline.allocate(size + kAbsJumpSize));
					WriteOriginal(stub, a_t.prologue, entry);
					a_next = reinterpret_cast<F>(stub);
					const auto patch = EntryPatch(size, hook);
					REL::safe_write(entry, patch.data(), patch.size());
					break;
				}
			case Entry::kJmpAbs:
				{
					a_next = reinterpret_cast<F>(AbsJumpTarget(entry));
					REL::safe_write(entry + sizeof(kAbsJumpOp), &hook, sizeof(hook));
					break;
				}
			case Entry::kJmpRel:
				{
					a_next = reinterpret_cast<F>(Rel32Target(entry));
					// within 2 GB of the exe
					auto* stub = static_cast<std::uint8_t*>(trampoline.allocate(kAbsJumpSize));
					AbsJump(stub, hook);
					const auto rel = static_cast<std::int32_t>(
						reinterpret_cast<std::intptr_t>(stub) - static_cast<std::intptr_t>(entry + 5));
					REL::safe_write(entry + 1, &rel, sizeof(rel));
					break;
				}
			default:
				break;
			}
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

		Detour(kAdd, add, &AddMessageThunk, g_addMessage);
		Detour(kProcess, process, &ProcessCommandsThunk, g_processCommands);
		return { Outcome::kActive, std::format("{} ({}) and {} ({}) both run under one lock", kAdd.name, Describe(add),
									   kProcess.name, Describe(process)) };
	}

	const bool kRegistered = Register({
		.key = "UIMessageQueue",
		.oldDll = "UIMessageQueueRaceFix.dll",
		.install = Install,
		.trampoline = kAdd.prologue.size() + kProcess.prologue.size() + 2 * kAbsJumpSize,
	});
}
