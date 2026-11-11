// StartupHook: the whole test plugin. Never install this to play. Widens the race in CommonLib's Trampoline::write_5branch for hooks installed while
// other threads run the hooked code: write_5branch redirects the call site first (REL::safe_write = VirtualProtect,
// memcpy, VirtualProtect back) and only then fills the trampoline slot, which SKSE's branch pool leaves as 0xCC.
// The probe replaces KernelBase!VirtualProtect; when the protection of a watched call site is restored - right after
// its new call bytes were written, before the slot is filled - it sleeps DelayMs, so a thread that runs the call in
// that time executes int3. Watched sites (LateHookRaceProbe.ini [Sites], 1 = on):
//   SmartTalk    = 68617+0x7B  (SmartTalk 1.0.6 ProcessInputHook, kDataLoaded; input poll on a job thread)
//   LoadProgress = 82084+0x18A (Skyrim Load Progress 1.0.10 Scaleform-begin hook, kDataLoaded; loading screen render)
// Self = 1: the probe itself redirects 68617+0x7B at kDataLoaded with this build's CommonLib (write_call<5> to the
// call's current target, so behaviour is unchanged) and only its own write is delayed - SmartTalk = 0 then, so its
// write keeps its natural window. Built against stock CommonLib this should crash; against the fixed one it must not.

namespace
{
	thread_local bool selfWrite = false;
	bool              self = false;
	using NtProtect = LONG(NTAPI*)(HANDLE, PVOID*, PSIZE_T, ULONG, PULONG);
	using NtToDos = ULONG(NTAPI*)(LONG);

	NtProtect                  ntProtect = nullptr;
	NtToDos                    ntToDos = nullptr;
	std::vector<std::uintptr_t> sites;
	DWORD                      delayMs = 500;
	std::atomic<std::uint32_t> hits{ 0 };

	BOOL WINAPI VirtualProtectHook(LPVOID a_address, SIZE_T a_size, DWORD a_protect, PDWORD a_old)
	{
		PVOID  address = a_address;
		SIZE_T size = a_size;
		const auto status = ntProtect(GetCurrentProcess(), &address, &size, a_protect, a_old);
		if (status < 0) {
			SetLastError(ntToDos(status));
			return FALSE;
		}
		const auto site = reinterpret_cast<std::uintptr_t>(a_address);
		if (a_protect != PAGE_EXECUTE_READWRITE && (self ? selfWrite : std::ranges::find(sites, site) != sites.end())) {
			const auto* code = reinterpret_cast<const std::uint8_t*>(site);
			const auto  target = site + 5 + *reinterpret_cast<const std::int32_t*>(code + 1);
			logger::info("site {:X} restored -> call {:X} (first byte {:02X}), thread {}: sleeping {} ms"sv, site, target,
				*reinterpret_cast<const std::uint8_t*>(target), GetCurrentThreadId(), delayMs);
			++hits;
			Sleep(delayMs);
			logger::info("site {:X}: woke, slot first byte now {:02X}"sv, site, *reinterpret_cast<const std::uint8_t*>(target));
		}
		return TRUE;
	}

	bool Install()
	{
		const auto ntdll = GetModuleHandleW(L"ntdll.dll");
		const auto kernelBase = GetModuleHandleW(L"KernelBase.dll");
		ntProtect = reinterpret_cast<NtProtect>(GetProcAddress(ntdll, "NtProtectVirtualMemory"));
		ntToDos = reinterpret_cast<NtToDos>(GetProcAddress(ntdll, "RtlNtStatusToDosError"));
		auto* const target = reinterpret_cast<std::uint8_t*>(GetProcAddress(kernelBase, "VirtualProtect"));
		if (!ntProtect || !ntToDos || !target) {
			return false;
		}
		std::array<std::uint8_t, 14> jmp{ 0xFF, 0x25, 0, 0, 0, 0 };  // jmp [rip]
		const auto                   hook = reinterpret_cast<std::uint64_t>(&VirtualProtectHook);
		std::memcpy(jmp.data() + 6, &hook, sizeof(hook));
		DWORD old = 0;
		if (!VirtualProtect(target, jmp.size(), PAGE_EXECUTE_READWRITE, &old)) {
			return false;
		}
		std::memcpy(target, jmp.data(), jmp.size());
		VirtualProtect(target, jmp.size(), old, &old);
		FlushInstructionCache(GetCurrentProcess(), target, jmp.size());
		return true;
	}

	void SelfHook(SKSE::MessagingInterface::Message* a_msg)
	{
		if (a_msg->type != SKSE::MessagingInterface::kDataLoaded) {
			return;
		}
		const auto  site = REL::ID(68617).address() + 0x7B;
		const auto* code = reinterpret_cast<const std::uint8_t*>(site);
		const auto  target = site + 5 + *reinterpret_cast<const std::int32_t*>(code + 1);
		logger::info("self: redirecting {:X} to its current target {:X} through the trampoline"sv, site, target);
		selfWrite = true;
		SKSE::GetTrampoline().write_call<5>(site, target);
		selfWrite = false;
		logger::info("self: done, {} delayed write(s)"sv, hits.load());
	}
}

SKSEPluginInfo(
	.Version = Plugin::VERSION,
	.Name = Plugin::NAME,
	.Author = "Yuof"sv,
	.RuntimeCompatibility = SKSE::VersionIndependence::AddressLibrary)

SKSE_PLUGIN_LOAD(const SKSE::LoadInterface* a_skse)
{
	SKSE::Init(a_skse, { .log = true });
	wchar_t ini[MAX_PATH]{};  // GetPrivateProfileInt reads a relative path from the Windows directory
	GetFullPathNameW(L"Data\\SKSE\\Plugins\\LateHookRaceProbe.ini", MAX_PATH, ini, nullptr);
	delayMs = GetPrivateProfileIntW(L"Sites", L"DelayMs", 500, ini);
	if (GetPrivateProfileIntW(L"Sites", L"SmartTalk", 1, ini)) {
		sites.push_back(REL::ID(68617).address() + 0x7B);
	}
	if (GetPrivateProfileIntW(L"Sites", L"LoadProgress", 1, ini)) {
		sites.push_back(REL::ID(82084).address() + 0x18A);
	}
	self = GetPrivateProfileIntW(L"Sites", L"Self", 0, ini) != 0;
	logger::info("ini {}: Self={} DelayMs={}"sv, std::filesystem::path(ini).string(), self, delayMs);
	if (self) {
		SKSE::AllocTrampoline(14);
		SKSE::GetMessagingInterface()->RegisterListener(SelfHook);
	}
	for (const auto site : sites) {
		logger::info("watching call site {:X} (first byte {:02X})"sv, site, *reinterpret_cast<const std::uint8_t*>(site));
	}
	logger::info("{} v{}: VirtualProtect hook {}, delay {} ms"sv, Plugin::NAME, Plugin::VERSION.string(),
		Install() ? "installed" : "FAILED", delayMs);
	return true;
}
