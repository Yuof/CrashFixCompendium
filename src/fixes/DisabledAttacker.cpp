// SkyrimSE+070DB20 (1.6.1170), null + 0x8 under Actor::CalculateCurrentHitTargetForWeaponSwing, with Precision 2.0.6.
// Cause: Precision queues a melee hit and runs it on a later update (PendingHit::Run). An attacker disabled or
// unloaded in between has no AI process, and Run reads through it.
// Fix: wrap PendingHit::Run in Precision.dll and drop a hit whose attacker has no process.
// Upstream: https://github.com/ersh1/Precision/pull/6. Only the released 2.0.6 build gets the patch (PE timestamp and
// the function's first bytes); any other build stays untouched.

#include "fix.h"
#include "hook.h"

namespace DisabledAttacker
{
	namespace
	{
		constexpr std::uint32_t  kTimestamp = 0x6A967CB2;  // Precision.dll 2.0.6 (Nexus 72347)
		constexpr std::uintptr_t kRun = 0x5CA50;           // PendingHit::Run (Precision.pdb)

		// mov rax,rsp / mov [rax+10h],rbx / mov [rax+18h],rsi / mov [rax+20h],rdi
		constexpr auto kPrologue = "\x48\x8B\xC4\x48\x89\x58\x10\x48\x89\x70\x18\x48\x89\x78\x20"sv;

		struct PendingHit
		{
			RE::NiPointer<RE::Actor> attacker;  // first member in 2.0.6
		};

		using Run_t = void (*)(PendingHit*);
		Run_t            g_run = nullptr;  // Precision's original function, through the trampoline
		SafetyHookInline g_hook;

		std::atomic<std::uint64_t> g_skipped{ 0 };

		void RunThunk(PendingHit* a_hit)
		{
			const auto* attacker = a_hit->attacker.get();
			if (attacker && attacker->GetActorRuntimeData().currentProcess) {
				g_run(a_hit);
				return;
			}

			if (const auto n = ++g_skipped; Milestone(n)) {
				logger::info(
					"[DisabledAttacker] pending hit of {:08X} dropped: "
					"the attacker has no AI process ({} so far)"sv,
					attacker ? attacker->GetFormID() : 0, n);
			}
		}
	}

	Result Install()
	{
		const auto module = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(L"Precision.dll"));
		if (!module) {
			return { Outcome::kNotNeeded, "Precision not installed" };
		}

		const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(module);
		const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(module + dos->e_lfanew);
		if (nt->FileHeader.TimeDateStamp != kTimestamp) {
			return { Outcome::kNotNeeded, "this fix only covers the Precision 2.0.6 build" };
		}

		const auto target = module + kRun;
		if (Bytes(target, kPrologue.size()) != kPrologue) {
			return { Outcome::kFailed, "PendingHit::Run's first bytes differ, nothing patched" };
		}

		if (const auto why = HookEntry(g_hook, target, &RunThunk, g_run); !why.empty()) {
			return { Outcome::kFailed, std::format("PendingHit::Run: {}, nothing patched"sv, why) };
		}
		return { Outcome::kActive, "wrapped Precision 2.0.6 PendingHit::Run" };
	}

	const bool kRegistered = Register({
		.key = "DisabledAttacker",
		.oldDll = "DisabledAttackerCrashFix.dll",
		.install = Install,
	});
}
