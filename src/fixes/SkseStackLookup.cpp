// skse64_1_6_1170.dll+5D51F (SKSE 2.2.8), mov r15, [rax+0x18] with rax = 0x80000003, during a save; skse64.log ends
// with "Saving SKSEPersistentObjectStorage data...".
// Cause: before a save writes the ModEvent, UICallback and SpawnerTask handles into the co-save, SKSE checks that the
// script stack holding each handle is still running. The check reads two pointers laid out as in Skyrim LE. On SE and
// AE the first one is the 4-byte header of a small array plus 4 bytes the game never sets. With those bytes at 0 the
// read faults at 0x8000001B. Otherwise it reads an arbitrary value, and a handle of a running script can be dropped.
// Fix: the check becomes "the stack is in the VM's stack table": the two reads become a copy of the stack pointer.
// Install finds the one place in SKSE's DLL by its bytes; an SKSE without them (one with
// https://github.com/ianpatt/skse64/pull/65) stays untouched.

#include "fix.h"

namespace SkseStackLookup
{
	namespace
	{
		// mov rax,[rax+8] / test rax,rax / je +0D / mov rax,[rax+18h] / test rax,rax / je +4 / mov r?,[rax+18h]
		// Both je land right after the last mov. 0 marks the modrm byte, which names the register.
		constexpr std::uint8_t kPattern[]{ 0x48, 0x8B, 0x40, 0x08, 0x48, 0x85, 0xC0, 0x74, 0x0D, 0x48, 0x8B, 0x40,
			0x18, 0x48, 0x85, 0xC0, 0x74, 0x04, 0x4C, 0x8B, 0x00, 0x18 };
		constexpr std::size_t  kModrm = 20;
		constexpr std::size_t  kSite = 9;  // the patch starts at mov rax,[rax+18h]
		constexpr std::size_t  kPatchSize = sizeof(kPattern) - kSite;

		bool Matches(const std::uint8_t* a_at)
		{
			for (std::size_t i = 0; i < sizeof(kPattern); ++i) {
				if (i != kModrm && a_at[i] != kPattern[i]) {
					return false;
				}
			}
			return (a_at[kModrm] & 0xC7) == 0x40;  // mod 01 (disp8), rm = rax
		}
	}

	Result Install()
	{
		HMODULE module = nullptr;
		if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
				reinterpret_cast<LPCWSTR>(SKSE::GetMessagingInterface()), &module)) {
			return { Outcome::kFailed, "SKSE's DLL not found, nothing patched" };
		}

		wchar_t path[MAX_PATH]{};
		GetModuleFileNameW(module, path, MAX_PATH);
		const auto name = std::filesystem::path(path).filename().string();

		const auto  base = reinterpret_cast<std::uintptr_t>(module);
		const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
		const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
		const auto* section = IMAGE_FIRST_SECTION(nt);

		std::uintptr_t site = 0;
		int            count = 0;
		for (WORD s = 0; s < nt->FileHeader.NumberOfSections; ++s, ++section) {
			if (!(section->Characteristics & IMAGE_SCN_MEM_EXECUTE)) {
				continue;
			}
			const auto* code = reinterpret_cast<const std::uint8_t*>(base + section->VirtualAddress);
			const auto  size = static_cast<std::size_t>(section->Misc.VirtualSize);
			for (std::size_t i = 0; i + sizeof(kPattern) <= size; ++i) {
				if (Matches(code + i)) {
					site = reinterpret_cast<std::uintptr_t>(code + i + kSite);
					++count;
				}
			}
		}

		if (count == 0) {
			return { Outcome::kNotNeeded, std::format("{} has no LE stack check (an SKSE with the fix)", name) };
		}
		if (count > 1) {
			return { Outcome::kFailed, std::format("{} places in {} look like the check, nothing patched", count, name) };
		}

		// mov r?, rax (the register of the last mov) and nops: the result is the stack pointer itself
		const auto reg = static_cast<std::uint8_t>(*reinterpret_cast<const std::uint8_t*>(site + kModrm - kSite) & 0x38);
		std::array<std::uint8_t, kPatchSize> patch{};
		patch.fill(0x90);
		patch[0] = 0x4C;
		patch[1] = 0x8B;
		patch[2] = static_cast<std::uint8_t>(0xC0 | reg);
		REL::safe_write(site, patch.data(), patch.size());
		return { Outcome::kActive, std::format("patched the stack check in {} at +{:X}", name, site - base) };
	}

	const bool kRegistered = Register({
		.key = "SkseStackLookup",
		.oldDll = "",
		.install = Install,
	});
}
