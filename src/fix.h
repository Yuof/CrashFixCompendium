#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

// 1, 10, 100, ...: logging only these keeps a busy game from growing the log. a_n must be at least 1.
inline bool Milestone(std::uint64_t a_n)
{
	while (a_n % 10 == 0) {
		a_n /= 10;
	}
	return a_n == 1;
}

inline std::string_view Bytes(std::uintptr_t a_at, std::size_t a_size)
{
	return { reinterpret_cast<const char*>(a_at), a_size };
}

inline std::uintptr_t Rel32Target(std::uintptr_t a_at)
{
	return a_at + 5 + *reinterpret_cast<const std::int32_t*>(a_at + 1);
}

struct CallSearch
{
	std::uintptr_t site;  // 0 unless there was exactly one
	int            count;

	std::string_view Why() const
	{
		return count ? "more than one call to the target" : "no call to the target (changed by another plugin?)";
	}
};

inline CallSearch FindCall(std::uintptr_t a_start, std::size_t a_span, std::uintptr_t a_target)
{
	CallSearch found{ 0, 0 };
	for (std::size_t i = 0; i + 5 <= a_span; ++i) {
		const auto at = a_start + i;
		if (*reinterpret_cast<const std::uint8_t*>(at) == 0xE8 && Rel32Target(at) == a_target) {
			found.site = at;
			++found.count;
		}
	}
	if (found.count != 1) {
		found.site = 0;
	}
	return found;
}

// jmp qword ptr [rip+0] / dq a_to
constexpr std::uint8_t kAbsJumpOp[]{ 0xFF, 0x25, 0, 0, 0, 0 };
constexpr std::size_t  kAbsJumpSize = sizeof(kAbsJumpOp) + sizeof(std::uintptr_t);

// volatile: StartupHook reads a slot that another thread is writing
inline bool IsAbsJump(std::uintptr_t a_at)
{
	const auto* code = reinterpret_cast<const volatile std::uint8_t*>(a_at);
	for (std::size_t i = 0; i < sizeof(kAbsJumpOp); ++i) {
		if (code[i] != kAbsJumpOp[i]) {
			return false;
		}
	}
	return true;
}

inline std::uintptr_t AbsJumpTarget(std::uintptr_t a_at)
{
	return *reinterpret_cast<const volatile std::uintptr_t*>(a_at + sizeof(kAbsJumpOp));
}

inline void AbsJump(std::uint8_t* a_at, std::uintptr_t a_to)
{
	std::memcpy(a_at, kAbsJumpOp, sizeof(kAbsJumpOp));
	std::memcpy(a_at + sizeof(kAbsJumpOp), &a_to, sizeof(a_to));
}

// The original function's stub: a_head (the function's first whole instructions, position-independent), then a jump
// to the rest of the function at a_entry + a_head.size()
inline void WriteOriginal(std::uint8_t* a_stub, std::string_view a_head, std::uintptr_t a_entry)
{
	std::memcpy(a_stub, a_head.data(), a_head.size());
	AbsJump(a_stub + a_head.size(), a_entry + a_head.size());
}

// A function's new first a_size bytes: jmp rel32 to a_to, then NOPs to the end of the last instruction it covers.
// Empty when a_size is under 5 or a_to is out of rel32 reach.
inline std::vector<std::uint8_t> EntryJump(std::uintptr_t a_entry, std::uintptr_t a_to, std::size_t a_size)
{
	const auto rel = static_cast<std::int64_t>(a_to - (a_entry + 5));
	if (a_size < 5 || rel != static_cast<std::int32_t>(rel)) {
		return {};
	}
	std::vector<std::uint8_t> patch(a_size, 0x90);
	patch[0] = 0xE9;
	const auto rel32 = static_cast<std::int32_t>(rel);
	std::memcpy(patch.data() + 1, &rel32, sizeof(rel32));
	return patch;
}

// The same with a 14-byte jmp [rip] to a_to, which reaches any address. Only for code outside SkyrimSE.exe that no
// other plugin hooks (Precision's PendingHit::Run): a plugin that later hooks the entry with a 5-byte jump and resumes
// at entry+5 would land inside this one.
inline std::vector<std::uint8_t> EntryPatch(std::size_t a_size, std::uintptr_t a_to)
{
	std::vector<std::uint8_t> patch(a_size, 0x90);
	AbsJump(patch.data(), a_to);
	return patch;
}

enum class Outcome
{
	kActive,
	kNotNeeded,  // its target mod isn't there or isn't a build it covers, or the bug can't occur
	kFailed
};

struct Result
{
	Outcome     outcome;
	std::string detail;
};

struct Fix
{
	std::string_view key;
	std::string_view oldDll;  // the standalone DLL it yields to; empty for a fix that never had one
	Result (*install)();
	bool             atLoad = false;       // the others install at kPostLoad, so nothing depends on where this DLL's name sorts
	std::size_t      trampoline = 0;       // bytes it allocates: SKSE::Init reserves the sum, once
	std::string_view section = "Fixes";  // its switch's INI section: Reports for a check that fixes nothing
};

// Local static: other files' globals call Register while the DLL loads
inline std::vector<Fix>& AllFixes()
{
	static std::vector<Fix> fixes;
	return fixes;
}

inline bool Register(const Fix& a_fix)
{
	AllFixes().push_back(a_fix);
	return true;
}

// HookWatch.cpp
std::string Owner(std::uintptr_t a_address);   // "<module>+<offset>", or the bare address outside every module
std::string JumpChain(std::uintptr_t a_site);  // the jumps from a_site to the code they reach, each as Owner()
// Call right after writing a jump or call at a_site: HookWatch logs what it leads to now and every later change.
void WatchSite(std::string_view a_name, std::uintptr_t a_site);
void HookWatchMessage(std::uint32_t a_type);  // every SKSE message, for the start-up timeline
