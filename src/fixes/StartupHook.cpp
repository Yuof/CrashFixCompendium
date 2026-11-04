// EXCEPTION_BREAKPOINT before the main menu (SkyrimSE 1.6.x): a thread runs a hook that another SKSE plugin is still
// writing.
// Cause: CommonLibSSE-NG's write_5branch redirects the call site first and fills its trampoline slot after
// (CommonLibSSE-NG PR #465). Unused slot memory is 0xCC. A plugin that hooks at kDataLoaded or later (SmartTalk
// 1.0.6, Skyrim Load Progress 1.0.10) does so while the loading screen polls input and renders on other threads.
// Fix: a vectored exception handler. On an int3 in non-image executable memory it waits up to 1 s for a complete
// slot (jmp [rip] to an executable address), then resumes there. Everything else passes on unchanged.
// Not covered: a thread that reads the slot mid-fill (four plain stores, nanoseconds apart) still faults. Add an
// access-violation path if that shows up.

#include "fix.h"

namespace StartupHook
{
	namespace
	{
		constexpr DWORD kWaitMs = 1000;

		std::atomic<std::uint32_t> g_resumed{ 0 };

		bool Executable(std::uintptr_t a_address, DWORD a_type)
		{
			MEMORY_BASIC_INFORMATION mbi{};
			if (!VirtualQuery(reinterpret_cast<void*>(a_address), &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT) {
				return false;
			}
			constexpr DWORD exec = PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
			return (mbi.Protect & exec) != 0 && (a_type == 0 || mbi.Type == a_type);
		}

		// jmp [rip] + its 8-byte target, as write_5branch leaves a finished slot
		bool CompleteSlot(std::uintptr_t a_address)
		{
			return IsAbsJump(a_address) && Executable(AbsJumpTarget(a_address), 0);
		}

		// "<module>+<offset>" of the hook's target: names the plugin that wrote the hook late
		std::string Owner(std::uintptr_t a_target)
		{
			HMODULE module = nullptr;
			wchar_t path[MAX_PATH]{};
			if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
										GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
					reinterpret_cast<LPCWSTR>(a_target), &module) ||
				!GetModuleFileNameW(module, path, MAX_PATH)) {
				return std::format("{:X}", a_target);
			}
			return std::format("{}+{:X}", std::filesystem::path(path).filename().string(),
				a_target - reinterpret_cast<std::uintptr_t>(module));
		}

		LONG CALLBACK Handler(EXCEPTION_POINTERS* a_info)
		{
			const auto* record = a_info->ExceptionRecord;
			if (record->ExceptionCode != EXCEPTION_BREAKPOINT) {
				return EXCEPTION_CONTINUE_SEARCH;
			}
			const auto address = reinterpret_cast<std::uintptr_t>(record->ExceptionAddress);
			if (!Executable(address, MEM_PRIVATE)) {  // an int3 inside a module image is deliberate: not ours
				return EXCEPTION_CONTINUE_SEARCH;
			}

			const auto start = GetTickCount64();
			while (!CompleteSlot(address)) {
				if (GetTickCount64() - start > kWaitMs) {
					return EXCEPTION_CONTINUE_SEARCH;
				}
				Sleep(1);
			}

			a_info->ContextRecord->Rip = address;
			const auto n = ++g_resumed;
			logger::info("[StartupHook] #{}: thread {} ran a hook slot at {:X} before it was written; "
						 "resumed after {} ms -> {}"sv,
				n, GetCurrentThreadId(), address, GetTickCount64() - start, Owner(AbsJumpTarget(address)));
			return EXCEPTION_CONTINUE_EXECUTION;
		}
	}

	Result Install()
	{
		if (AddVectoredExceptionHandler(1, Handler)) {
			return { Outcome::kActive, "watching for hook slots run before they are written" };
		}
		return { Outcome::kFailed, "AddVectoredExceptionHandler, nothing patched" };
	}

	const bool kRegistered = Register({
		.key = "StartupHook",
		.oldDll = "StartupHookCrashFix.dll",
		.install = Install,
		.atLoad = true,
	});
}
