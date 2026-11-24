// Crashes in whatever reads a menu's name next ("Fader Menu", "HUD Menu", ...): the name has an invalid length or text.
// Cause: UIMessageQueue::AddMessage takes no lock, and job threads queue UI messages too. Two threads that read the
// same pool count (+0x378) fill the same pooled message, and each releases the menu name it held. The string pool
// then frees a name that the UI still uses. ProcessCommands resets the count without a lock as well.
// Fix: both functions run under one lock. Install checks their first bytes and the pool-count instruction, which are
// the same in 1.5.97, 1.6.1170 and 1.7.104. A jump that another plugin put at an entry is chained, so its code runs
// inside the lock. Any other code at an entry stays unpatched.

#include "fix.h"
#include "hook.h"

#include <thread>

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

		// the entry jump replaces push rsi / push rdi / push r14 and mov [rsp+10h],rbx: 5 bytes in both
		constexpr std::size_t kJumped = 5;

		std::recursive_mutex g_lock;

		// For HookWatch: which thread holds the lock, since when, and how many threads wait for it
		std::atomic<std::uint32_t> g_holder{ 0 };
		std::atomic<std::uint64_t> g_heldSince{ 0 };
		std::atomic<int>           g_waiting{ 0 };
		thread_local int           t_depth = 0;

		struct Held
		{
			Held()
			{
				++g_waiting;
				g_lock.lock();
				--g_waiting;
				if (t_depth++ == 0) {
					g_heldSince = GetTickCount64();
					g_holder = GetCurrentThreadId();
				}
			}
			~Held()
			{
				if (--t_depth == 0) {
					g_holder = 0;
				}
				g_lock.unlock();
			}
			Held(const Held&) = delete;
			Held& operator=(const Held&) = delete;
		};

		void AddMessageThunk(RE::UIMessageQueue* a_queue, const RE::BSFixedString& a_menu, RE::UI_MESSAGE_TYPE a_type,
			RE::IUIMessageData* a_data)
		{
			const Held lock;
			g_addMessage(a_queue, a_menu, a_type, a_data);
		}

		void ProcessCommandsThunk(RE::UIMessageQueue* a_queue)
		{
			const Held lock;
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

		// Read before the entry is hooked: a chained jump is named by the code it leads to
		std::string Describe(const Target& a_t, Entry a_e)
		{
			return a_e == Entry::kOriginal ?
			           "original code"s :
			           "chained to another plugin's jump at its entry: " + JumpChain(a_t.id.address());
		}

		// Original code: the replaced instructions run in a stub. Another plugin's jump: its target becomes a_original,
		// so that plugin's code runs inside the lock.
		template <class F>
		std::string Hook(const Target& a_t, Entry a_e, F a_detour, F& a_original)
		{
			const auto entry = a_t.id.address();
			if (a_e == Entry::kOriginal) {
				return HookEntry(a_t.name, entry, a_t.prologue.substr(0, kJumped), a_detour, a_original);
			}
			a_original = reinterpret_cast<F>(IsAbsJump(entry) ? AbsJumpTarget(entry) : Rel32Target(entry));
			return JumpToDetour(a_t.name, entry, kJumped, reinterpret_cast<std::uintptr_t>(a_detour));
		}
	}

	bool LockHeld(std::uint32_t& a_thread, std::uint64_t& a_since, int& a_waiting)
	{
		a_thread = g_holder;
		a_since = g_heldSince;
		a_waiting = g_waiting;
		return a_thread != 0;
	}

#ifdef HOOKWATCH_FORCE
	// Test build only: a thread holds the lock for a_ms, as a hook that waits inside it would
	void ForceHold(std::uint32_t a_ms)
	{
		std::thread([a_ms] {
			const Held lock;
			Sleep(a_ms);
		}).detach();
	}
#endif

	Result Install()
	{
		const auto add = Classify(kAdd);
		const auto process = Classify(kProcess);
		if (add == Entry::kUnknown || process == Entry::kUnknown) {
			return { Outcome::kFailed, std::format("{}: unexpected code at its entry, nothing patched",
										   add == Entry::kUnknown ? kAdd.name : kProcess.name) };
		}
		const auto addText = Describe(kAdd, add);
		const auto processText = Describe(kProcess, process);

		if (const auto why = Hook(kAdd, add, &AddMessageThunk, g_addMessage); !why.empty()) {
			return { Outcome::kFailed, std::format("{}: {}, nothing patched", kAdd.name, why) };
		}
		if (const auto why = Hook(kProcess, process, &ProcessCommandsThunk, g_processCommands); !why.empty()) {
			return { Outcome::kFailed, std::format("{}: {}; only {} runs under the lock", kProcess.name, why, kAdd.name) };
		}
		return { Outcome::kActive, std::format("{} ({}) and {} ({}) both run under one lock", kAdd.name, addText,
									   kProcess.name, processText) };
	}

	const bool kRegistered = Register({
		.key = "UIMessageQueue",
		.oldDll = "UIMessageQueueRaceFix.dll",
		.install = Install,
		.trampoline = HookSize(kAdd.prologue.substr(0, kJumped)) + HookSize(kProcess.prologue.substr(0, kJumped)),
	});
}
