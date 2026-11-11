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

inline void AbsJump(std::uint8_t* a_at, std::uintptr_t a_to)
{
	std::memcpy(a_at, kAbsJumpOp, sizeof(kAbsJumpOp));
	std::memcpy(a_at + sizeof(kAbsJumpOp), &a_to, sizeof(a_to));
}

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

// a_prologue must be position-independent
inline void WriteOriginal(std::uint8_t* a_stub, std::string_view a_prologue, std::uintptr_t a_entry)
{
	std::memcpy(a_stub, a_prologue.data(), a_prologue.size());
	AbsJump(a_stub + a_prologue.size(), a_entry + a_prologue.size());
}

inline std::vector<std::uint8_t> EntryPatch(std::size_t a_size, std::uintptr_t a_hook)
{
	std::vector<std::uint8_t> patch(a_size, 0x90);
	AbsJump(patch.data(), a_hook);
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
