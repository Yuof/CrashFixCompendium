#pragma once

#include "fix.h"

#include <cstdint>
#include <string>
#include <string_view>

// Function entry hooks in SkyrimSE.exe. The entry gets a 5-byte jump to a slot in SKSE's trampoline, and the slot
// jumps to the detour. With 5 bytes, a plugin that hooks the same entry later with its own 5-byte jump and resumes the
// game's code at entry+5 doesn't land in the middle of this jump (MiniMap Menu 4.2.6 does this with AddMessage).
// a_size: the end of the last instruction the jump covers. Its bytes after the jump become NOPs.
// Trampoline use per hook: kAbsJumpSize, plus a_head.size() + kAbsJumpSize for HookEntry. The fix's .trampoline
// counts them.
// Returns an empty string, or the reason that nothing was patched.
inline std::string JumpToDetour(std::uintptr_t a_entry, std::size_t a_size, std::uintptr_t a_detour)
{
	auto* slot = static_cast<std::uint8_t*>(SKSE::GetTrampoline().allocate(kAbsJumpSize));
	AbsJump(slot, a_detour);
	const auto patch = EntryJump(a_entry, reinterpret_cast<std::uintptr_t>(slot), a_size);
	if (patch.empty()) {
		return "SKSE's trampoline is out of jump range";
	}
	REL::safe_write(a_entry, patch.data(), patch.size());
	return {};
}

// For a detour that calls the original function. a_head = the function's first whole instructions, at least 5 bytes,
// none of them rip-relative; the caller has checked that they are the game's bytes. They run in a stub that then
// jumps to entry + a_head.size(). a_original is set before the entry changes.
template <class F>
std::string HookEntry(std::uintptr_t a_entry, std::string_view a_head, F a_detour, F& a_original)
{
	auto* stub = static_cast<std::uint8_t*>(SKSE::GetTrampoline().allocate(a_head.size() + kAbsJumpSize));
	WriteOriginal(stub, a_head, a_entry);
	a_original = reinterpret_cast<F>(stub);
	return JumpToDetour(a_entry, a_head.size(), reinterpret_cast<std::uintptr_t>(a_detour));
}

constexpr std::size_t kReplaceSize = kAbsJumpSize;  // trampoline bytes for JumpToDetour

constexpr std::size_t HookSize(std::string_view a_head)  // trampoline bytes for HookEntry
{
	return a_head.size() + 2 * kAbsJumpSize;
}
