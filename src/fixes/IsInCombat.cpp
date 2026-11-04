// SkyrimSE+0CC90CF (1.6.1170), BSReadWriteLock::LockForRead on null + 0x88 under TESObjectREFR::GetDisplayFullName,
// while a console command runs.
// Cause: in console mode IsInCombat prints "%s is not in combat" with its subject's name and never checks that the
// subject is an actor. A non-actor arrives as null. coc, cow and ConsoleUtil calls trigger it through quest
// conditions that test IsInCombat on objects.
// Fix: wrap IsInCombat's console handler and condition function in the script command table. In console mode a
// non-actor gets 0, the game's own answer. The command is found by name: no code offsets.

#include "fix.h"

namespace IsInCombat
{
	namespace
	{
		RE::SCRIPT_FUNCTION::Execute_t*   g_execute = nullptr;
		RE::SCRIPT_FUNCTION::Condition_t* g_condition = nullptr;

		std::atomic<std::uint64_t> g_catches{ 0 };

		// The case the game crashes on: console mode and a subject that is not an actor. Answers 0.
		bool Caught(RE::TESObjectREFR* a_ref, double& a_result)
		{
			const bool isActor = a_ref && a_ref->GetFormType() == RE::FormType::ActorCharacter;
			if (isActor || !RE::ConsoleLog::IsConsoleMode()) {
				return false;
			}

			a_result = 0.0;
			// one console coc can run a quest fill over thousands of references
			if (const auto n = ++g_catches; Milestone(n)) {
				logger::info(
					"[IsInCombat] IsInCombat in console mode on {:08X} \"{}\", not an actor: answered 0 ({} so far)"sv,
					a_ref ? a_ref->GetFormID() : 0, a_ref ? a_ref->GetDisplayFullName() : "", n);
			}
			return true;
		}

		bool ExecuteThunk(const RE::SCRIPT_PARAMETER* a_paramInfo, RE::SCRIPT_FUNCTION::ScriptData* a_scriptData,
			RE::TESObjectREFR* a_thisObj, RE::TESObjectREFR* a_containingObj, RE::Script* a_scriptObj,
			RE::ScriptLocals* a_locals, double& a_result, std::uint32_t& a_opcodeOffsetPtr)
		{
			if (Caught(a_thisObj, a_result)) {
				RE::ConsoleLog::GetSingleton()->Print("%s is not in combat",
					a_thisObj ? a_thisObj->GetDisplayFullName() : "");
				return true;
			}
			return g_execute(a_paramInfo, a_scriptData, a_thisObj, a_containingObj, a_scriptObj, a_locals, a_result,
				a_opcodeOffsetPtr);
		}

		bool ConditionThunk(RE::TESObjectREFR* a_thisObj, void* a_param1, void* a_param2, double& a_result)
		{
			return Caught(a_thisObj, a_result) || g_condition(a_thisObj, a_param1, a_param2, a_result);
		}
	}

	Result Install()
	{
		auto* command = RE::SCRIPT_FUNCTION::LocateScriptCommand("IsInCombat"sv);
		if (!command || !command->executeFunction || !command->conditionFunction || !command->referenceFunction ||
			command->numParams != 1) {
			return { Outcome::kFailed,
				"IsInCombat not found as expected in the script command table, nothing patched" };
		}

		g_execute = command->executeFunction;
		g_condition = command->conditionFunction;
		command->executeFunction = ExecuteThunk;
		command->conditionFunction = ConditionThunk;
		return { Outcome::kActive, "wrapped IsInCombat's console handler and condition function" };
	}

	const bool kRegistered = Register({
		.key = "IsInCombat",
		.oldDll = "IsInCombatConsoleCrashFix.dll",
		.install = Install,
	});
}
