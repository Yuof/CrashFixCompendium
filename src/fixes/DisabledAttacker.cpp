// SkyrimSE+070DB20 (1.6.1170), null + 0x8 under Actor::CalculateCurrentHitTargetForWeaponSwing, with Precision 2.0.6.
// Cause: Precision queues a melee hit and runs it on a later update (PendingHit::Run). An attacker disabled or
// unloaded in between has no AI process, and Run reads through it.
// Fix: wrap PendingHit::Run in Precision.dll and drop a hit whose attacker has no process.
// Upstream: https://github.com/ersh1/Precision/pull/6. Only the released 2.0.6 build gets the patch (PE timestamp and
// the function's first bytes); any other build stays untouched.

#include "fix.h"

namespace DisabledAttacker
{
	namespace
	{
		constexpr std::uint32_t  kTimestamp = 0x6A967CB2;  // Precision.dll 2.0.6 (Nexus 72347)
		constexpr std::uintptr_t kRun = 0x5CA50;           // PendingHit::Run (Precision.pdb)

		// mov rax,rsp / mov [rax+10h],rbx / mov [rax+18h],rsi / mov [rax+20h],rdi: position-independent, so the
		// stub can run them
		constexpr auto kPrologue = "\x48\x8B\xC4\x48\x89\x58\x10\x48\x89\x70\x18\x48\x89\x78\x20"sv;

		struct PendingHit
		{
			RE::NiPointer<RE::Actor> attacker;  // first member in 2.0.6
		};

		using Run_t = void (*)(PendingHit*);
		Run_t g_run = nullptr;  // the original, through the stub

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

		// SKSE's trampoline sits near SkyrimSE.exe, out of rel32 reach of Precision.dll: absolute jumps both ways
		const auto stubSize = kPrologue.size() + kAbsJumpSize;
		auto*      stub =
			static_cast<std::uint8_t*>(VirtualAlloc(nullptr, stubSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
		if (!stub) {
			return { Outcome::kFailed, "no memory for the stub, nothing patched" };
		}
		WriteOriginal(stub, kPrologue, target);

		DWORD oldProtect = 0;
		if (!VirtualProtect(stub, stubSize, PAGE_EXECUTE_READ, &oldProtect)) {
			return { Outcome::kFailed, "could not make the stub executable, nothing patched" };
		}
		g_run = reinterpret_cast<Run_t>(stub);

		const auto patch = EntryPatch(kPrologue.size(), reinterpret_cast<std::uintptr_t>(&RunThunk));
		REL::safe_write(target, patch.data(), patch.size());
		return { Outcome::kActive, "wrapped Precision 2.0.6 PendingHit::Run" };
	}

	const bool kRegistered = Register({
		.key = "DisabledAttacker",
		.oldDll = "DisabledAttackerCrashFix.dll",
		.install = Install,
	});
}
