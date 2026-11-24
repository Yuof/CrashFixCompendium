// Not a fix: a report for games that stop before the main menu. Every jump or call this DLL writes into the game's
// code is logged with where it leads, and a background check once a second logs each later change: another plugin
// that hooks the same place after this one, by its module, or jumps that lead back to where they started (a loop
// that never returns). It also logs when one thread holds UIMessageQueue's lock for 5 s or more (another plugin's
// hook that waits inside the lock), each SKSE message, and every 30 s until the main menu opens. It changes nothing.

#include "fix.h"
#include "jumps.h"

#include <thread>

namespace UIMessageQueue
{
	bool LockHeld(std::uint32_t& a_thread, std::uint64_t& a_since, int& a_waiting);
#ifdef HOOKWATCH_FORCE
	void ForceHold(std::uint32_t a_ms);
#endif
}

namespace
{
	// ReadProcessMemory fails on memory that can't be read instead of faulting. A read that runs into the next page
	// keeps the bytes before it, padded with int3.
	bool ReadMemory(std::uintptr_t a_at, void* a_out, std::size_t a_size)
	{
		SIZE_T read = 0;
		if (ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(a_at), a_out, a_size, &read) &&
			read == a_size) {
			return true;
		}
		const auto toPage = 0x1000 - (a_at & 0xFFF);
		if (a_size != 16 || toPage >= a_size ||
			!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(a_at), a_out, toPage, &read) ||
			read != toPage) {
			return false;
		}
		std::memset(static_cast<std::uint8_t*>(a_out) + toPage, 0xCC, a_size - toPage);
		return true;
	}
}

std::string Owner(std::uintptr_t a_address)
{
	HMODULE module = nullptr;
	wchar_t path[MAX_PATH]{};
	if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			reinterpret_cast<LPCWSTR>(a_address), &module) ||
		!GetModuleFileNameW(module, path, MAX_PATH)) {
		return std::format("{:X}", a_address);
	}
	return std::format("{}+{:X}", std::filesystem::path(path).filename().string(),
		a_address - reinterpret_cast<std::uintptr_t>(module));
}

std::string JumpChain(std::uintptr_t a_site)
{
	const auto  chain = Jumps::Follow(a_site, ReadMemory);
	std::string text;
	for (const auto at : chain.at) {
		text += (text.empty() ? "" : " -> ") + Owner(at);
	}
	switch (chain.end) {
	case Jumps::End::kLoop:
		return text + " (a LOOP: these jumps never reach code)";
	case Jumps::End::kUnreadable:
		return text + " (can't be read)";
	case Jumps::End::kTooLong:
		return text + " (more jumps follow)";
	default:
		return text;
	}
}

namespace HookWatch
{
	namespace
	{
		struct Site
		{
			std::string    name;
			std::uintptr_t at;
			std::string    chain;  // as last logged
		};

		std::atomic<bool> g_on{ false };
		std::mutex        g_sitesLock;
		std::vector<Site> g_sites;
		std::uint64_t     g_start = 0;  // when the DLL loaded
		std::uint32_t     g_mainThread = 0;

		std::atomic<std::uint32_t> g_lastMessage{ UINT32_MAX };
		std::atomic<std::uint32_t> g_seenMessages{ 0 };  // one bit per message type: each is logged once
		std::atomic<bool>          g_started{ false };   // the main menu opened, or a game was loaded or started

		double Seconds() { return (GetTickCount64() - g_start) / 1000.0; }

		std::string_view MessageName(std::uint32_t a_type)
		{
			using M = SKSE::MessagingInterface;
			switch (a_type) {
			case M::kPostLoad:
				return "PostLoad"sv;
			case M::kPostPostLoad:
				return "PostPostLoad"sv;
			case M::kInputLoaded:
				return "InputLoaded"sv;
			case M::kDataLoaded:
				return "DataLoaded"sv;
			case M::kNewGame:
				return "NewGame"sv;
			case M::kPreLoadGame:
				return "PreLoadGame"sv;
			case M::kPostLoadGame:
				return "PostLoadGame"sv;
			default:
				return "another message"sv;
			}
		}

		std::string LockText()
		{
			std::uint32_t thread = 0;
			std::uint64_t since = 0;
			int           waiting = 0;
			if (!UIMessageQueue::LockHeld(thread, since, waiting)) {
				return "UIMessageQueue's lock is free"s;
			}
			return std::format("UIMessageQueue's lock is held by thread {}{} for {:.1f} s, {} thread(s) waiting", thread,
				thread == g_mainThread ? " (the main thread)" : "", (GetTickCount64() - since) / 1000.0, waiting);
		}

		void CheckSites()
		{
			const std::scoped_lock lock{ g_sitesLock };
			for (auto& site : g_sites) {
				auto chain = JumpChain(site.at);
				if (chain != site.chain) {
					logger::warn("[HookWatch] {} changed after this DLL patched it ({:.1f} s): {} (was: {})"sv, site.name,
						Seconds(), chain, site.chain);
					site.chain = std::move(chain);
				}
			}
		}

		void CheckLock(std::uint64_t& a_reported)
		{
			std::uint32_t thread = 0;
			std::uint64_t since = 0;
			int           waiting = 0;
			if (UIMessageQueue::LockHeld(thread, since, waiting) && GetTickCount64() - since >= 5000 &&
				since != a_reported) {
				a_reported = since;
				logger::warn("[HookWatch] {:.1f} s: {}"sv, Seconds(), LockText());
			}
		}

		void Run()
		{
			std::uint64_t lastBeat = GetTickCount64();
			std::uint64_t reported = 0;
			for (;;) {
				Sleep(1000);
				CheckSites();
				CheckLock(reported);
				if (!g_started && GetTickCount64() - lastBeat >= 30000) {
					lastBeat = GetTickCount64();
					const auto last = g_lastMessage.load();
					logger::info("[HookWatch] {:.0f} s: no main menu yet, last SKSE message {}, {}"sv, Seconds(),
						last == UINT32_MAX ? "none"sv : MessageName(last), LockText());
				}
			}
		}

		struct MenuSink : RE::BSTEventSink<RE::MenuOpenCloseEvent>
		{
			RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent* a_event,
				RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override
			{
				if (a_event && a_event->opening && a_event->menuName == RE::MainMenu::MENU_NAME &&
					!g_started.exchange(true)) {
					const std::scoped_lock lock{ g_sitesLock };
					logger::info("[HookWatch] {:.1f} s: main menu open, {} patched sites watched"sv, Seconds(),
						g_sites.size());
				}
				return RE::BSEventNotifyControl::kContinue;
			}
		};
		MenuSink g_menuSink;

#ifdef HOOKWATCH_FORCE
		// Test build only, at DataLoaded: a site that jumps to code, rewritten 3 s later into a loop; and the UI
		// message lock held for 6 s
		void Force()
		{
			auto* mem = static_cast<std::uint8_t*>(
				VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
			if (!mem) {
				return;
			}
			const auto a = reinterpret_cast<std::uintptr_t>(mem);
			mem[48] = 0xC3;  // ret
			AbsJump(mem, a + 48);
			AbsJump(mem + 16, a);  // jumps back to the site
			WatchSite("test site"sv, a);
			std::thread([mem, a] {
				Sleep(3000);
				*reinterpret_cast<volatile std::uintptr_t*>(mem + sizeof(kAbsJumpOp)) = a + 16;
				Sleep(2000);
				UIMessageQueue::ForceHold(6000);
			}).detach();
		}
#endif
	}

	Result Install()
	{
		g_start = GetTickCount64();
		g_mainThread = GetCurrentThreadId();  // SKSE loads plugins on the main thread
		g_on = true;
		std::thread(Run).detach();
		return { Outcome::kActive,
			"diagnostic build: every patched site is logged with where it leads, and each later change to it; "
			"UIMessageQueue's lock held for 5 s or more; SKSE's messages, and every 30 s until the main menu" };
	}

	const bool kRegistered = Register({
		.key = "HookWatch",
		.install = Install,
		.atLoad = true,  // before every fix that patches, so the first line of each site is this DLL's own jump
		.section = "Reports",
	});
}

void WatchSite(std::string_view a_name, std::uintptr_t a_site)
{
	if (!HookWatch::g_on) {
		return;
	}
	auto chain = JumpChain(a_site);
	logger::info("[HookWatch] {}: {}"sv, a_name, chain);
	const std::scoped_lock lock{ HookWatch::g_sitesLock };
	HookWatch::g_sites.push_back({ std::string(a_name), a_site, std::move(chain) });
}

void HookWatchMessage(std::uint32_t a_type)
{
	if (!HookWatch::g_on) {
		return;
	}
	using M = SKSE::MessagingInterface;
	HookWatch::g_lastMessage = a_type;
	const auto bit = a_type < 32 ? 1u << a_type : 0u;
	if (bit && !(HookWatch::g_seenMessages.fetch_or(bit) & bit)) {
		logger::info("[HookWatch] {:.1f} s: SKSE message {}"sv, HookWatch::Seconds(), HookWatch::MessageName(a_type));
	}
	if (a_type == M::kNewGame || a_type == M::kPostLoadGame) {
		HookWatch::g_started = true;
	}
	if (a_type == SKSE::MessagingInterface::kDataLoaded) {
		if (auto* ui = RE::UI::GetSingleton()) {
			ui->AddEventSink<RE::MenuOpenCloseEvent>(&HookWatch::g_menuSink);
		}
#ifdef HOOKWATCH_FORCE
		HookWatch::Force();
#endif
	}
}
