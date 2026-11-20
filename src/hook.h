#pragma once

#include <safetyhook.hpp>

#include <cstdint>
#include <format>
#include <string>

inline std::string HookError(const safetyhook::InlineHook::Error& a_error)
{
	using E = safetyhook::InlineHook::Error;
	switch (a_error.type) {
	case E::BAD_ALLOCATION:
		return "no memory for the hook near the function";
	case E::FAILED_TO_UNPROTECT:
		return "could not unprotect the function's code";
	case E::NOT_ENOUGH_SPACE:
		return "the function is too short to hook";
	default:
		return std::format("cannot relocate the instruction at {:X} (SafetyHook code {})",
			reinterpret_cast<std::uintptr_t>(a_error.ip), static_cast<int>(a_error.type));
	}
}

// Hooks a function's entry with SafetyHook. SafetyHook writes a 5-byte jump, followed by NOPs up to the end of the
// last instruction it replaces, and moves the replaced instructions into a trampoline. A jump that another plugin put
// at the entry moves too, so that plugin's code runs when a_detour calls a_original.
// With no free memory near the function, SafetyHook writes a 14-byte jump instead. A plugin that hooks the entry later
// and resumes at entry+5 would land in the middle of that jump, so this function removes such a hook again.
// a_original is set before the hook goes live. a_detour calls it directly: InlineHook::call takes a mutex on every call.
// Returns an empty string, or the reason that nothing was patched.
template <class F>
std::string HookEntry(SafetyHookInline& a_hook, std::uintptr_t a_target, F a_detour, F& a_original)
{
	auto hook = SafetyHookInline::create(reinterpret_cast<void*>(a_target), reinterpret_cast<void*>(a_detour),
		SafetyHookInline::StartDisabled);
	if (!hook) {
		return HookError(hook.error());
	}
	a_original = hook->original<F>();
	if (auto on = hook->enable(); !on) {
		return HookError(on.error());
	}
	if (*reinterpret_cast<const std::uint8_t*>(a_target) != 0xE9) {
		return "no free memory near the function for a 5-byte jump";  // the hook's destructor restores the entry
	}
	a_hook = std::move(*hook);
	return {};
}

// for a detour that replaces the function and never calls the original
template <class F>
std::string HookEntry(SafetyHookInline& a_hook, std::uintptr_t a_target, F a_detour)
{
	F unused = nullptr;
	return HookEntry(a_hook, a_target, a_detour, unused);
}
