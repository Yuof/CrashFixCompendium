// A crash inside EngineFixes.dll (tbbmalloc's large-object cache): `movdqa` to a stack slot 8 off 16-byte alignment.
// Cause: Engine Fixes' BSLightingShaderPropertyShadowMap fix jumps out of
// BSLightingShaderProperty::GetRenderPasses_ShadowMapOrMask into a stub that calls its detour after
// `push rbx; sub rsp, 0x20`. The push misaligns the stack on every call. tbbmalloc's cache cleanup stores SSE
// registers with aligned moves and faults. Upstream: aers/EngineFixesSkyrim64 issue #102.
// Fix: find Engine Fixes' stub through the jump at the hook site, compare it byte for byte, and point the jump at our
// own stub: same register setup, same continuation, aligned stack. Anything else there (no Engine Fixes, its fix
// off, a release with a changed stub) stays unpatched.

#include "fix.h"

namespace ShadowMapStackAlign
{
	namespace
	{
		// Engine Fixes 7.0.19 / 7.0.20 (src/fixes/bslightingshaderproperty_shadowmap.h, Patch): ? = an address
		constexpr std::array<std::int16_t, 51> kEFStub{
			0x48, 0x89, 0xF1,                                // mov rcx, rsi
			0x48, 0x8B, 0x94, 0x24, 0xF8, 0x00, 0x00, 0x00,  // mov rdx, [rsp+0xF8]
			0x49, 0xB8, -1, -1, -1, -1, -1, -1, -1, -1,      // mov r8, detour
			0x53,                                            // push rbx
			0x48, 0x83, 0xEC, 0x20,                          // sub rsp, 0x20
			0x41, 0xFF, 0xD0,                                // call r8
			0x48, 0x83, 0xC4, 0x20,                          // add rsp, 0x20
			0x5B,                                            // pop rbx
			0x49, 0x89, 0xC7,                                // mov r15, rax
			0xFF, 0x25, 0x00, 0x00, 0x00, 0x00,              // jmp [rip]
			-1, -1, -1, -1, -1, -1, -1, -1                   // continuation
		};
		constexpr std::size_t kDetourAt = 13;
		constexpr std::size_t kContinueAt = 43;

		constexpr REL::RelocationID kFunction{ 99872, 106517 };  // GetRenderPasses_ShadowMapOrMask

		bool Matches(const std::uint8_t* a_code)
		{
			for (std::size_t i = 0; i < kEFStub.size(); ++i) {
				if (kEFStub[i] >= 0 && a_code[i] != kEFStub[i]) {
					return false;
				}
			}
			return true;
		}

		// The jump's target. write_jmp<5> may have gone through CommonLib's 14-byte `jmp [rip]` thunk.
		const std::uint8_t* Follow(std::uintptr_t a_site)
		{
			if (*reinterpret_cast<const std::uint8_t*>(a_site) != 0xE9) {
				return nullptr;
			}
			const auto to = Rel32Target(a_site);
			return reinterpret_cast<const std::uint8_t*>(IsAbsJump(to) ? AbsJumpTarget(to) : to);
		}

		void Append(std::vector<std::uint8_t>& a_code, std::uintptr_t a_value)
		{
			const auto* p = reinterpret_cast<const std::uint8_t*>(&a_value);
			a_code.insert(a_code.end(), p, p + sizeof(a_value));
		}

		std::vector<std::uint8_t> Stub(std::uintptr_t a_detour, std::uintptr_t a_continue)
		{
			std::vector<std::uint8_t> s{
				0x48, 0x89, 0xF1,                                // mov rcx, rsi
				0x48, 0x8B, 0x94, 0x24, 0xF8, 0x00, 0x00, 0x00,  // mov rdx, [rsp+0xF8] (before any push)
				0x53,                                            // push rbx
				0x55,                                            // push rbp
				0x48, 0x89, 0xE5,                                // mov rbp, rsp
				0x48, 0x83, 0xE4, 0xF0,                          // and rsp, -16
				0x48, 0x83, 0xEC, 0x20,                          // sub rsp, 0x20 (shadow space)
				0x48, 0xB8                                       // mov rax, detour
			};
			Append(s, a_detour);
			// clang-format off
			s.insert(s.end(), {
				0xFF, 0xD0,                         // call rax
				0x48, 0x89, 0xEC,                   // mov rsp, rbp
				0x5D,                               // pop rbp
				0x5B,                               // pop rbx
				0x49, 0x89, 0xC7,                   // mov r15, rax
				0xFF, 0x25, 0x00, 0x00, 0x00, 0x00  // jmp [rip]
			});
			// clang-format on
			Append(s, a_continue);
			return s;
		}
	}

	Result Install()
	{
		if (!GetModuleHandleW(L"EngineFixes.dll")) {
			return { Outcome::kNotNeeded, "Engine Fixes not installed" };
		}

		// Engine Fixes installs its fixes in its preload, before any plugin loads: its jump is already there
		const auto site = kFunction.address() + REL::VariantOffset(0x291, 0x295, 0).offset();
		const auto continuation = site + REL::VariantOffset(0x7EB, 0x802, 0).offset();  // EF: a_target (= the site) + V

		const auto* ef = Follow(site);
		if (!ef) {
			return { Outcome::kNotNeeded, "Engine Fixes' ShadowMap fix is off" };
		}
		if (!Matches(ef) || *reinterpret_cast<const std::uintptr_t*>(ef + kContinueAt) != continuation) {
			std::string bytes;
			for (std::size_t i = 0; i < kEFStub.size(); ++i) {
				bytes += std::format("{:02X} ", ef[i]);
			}
			return { Outcome::kNotNeeded,
				std::format("the code behind the ShadowMap hook is not the Engine Fixes stub this fix knows "
							"(fixed upstream, or another hook), nothing patched, found: {}",
					bytes) };
		}

		const auto detour = *reinterpret_cast<const std::uintptr_t*>(ef + kDetourAt);
		const auto stub = Stub(detour, continuation);
		auto&      trampoline = SKSE::GetTrampoline();
		auto*      mem = trampoline.allocate(stub.size());
		std::memcpy(mem, stub.data(), stub.size());
		trampoline.write_branch<5>(site, reinterpret_cast<std::uintptr_t>(mem));
		return { Outcome::kActive,
			std::format("Engine Fixes' ShadowMap detour at {:X} now called with an aligned stack", detour) };
	}

	const bool kRegistered = Register({
		.key = "ShadowMapStackAlign",
		.oldDll = "ShadowMapStackAlignFix.dll",
		.install = Install,
		.trampoline = 58 + kAbsJumpSize,  // Stub's 58 bytes and the write_branch<5> to them
	});
}
