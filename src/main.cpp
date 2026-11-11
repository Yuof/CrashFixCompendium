// Crash Fix Compendium: one DLL holding several crash fixes, each in src/fixes/<Key>.cpp, which registers itself.
// This file reads the switches, yields to an old standalone DLL, installs each fix and writes its status line.
// A new fix is one file and one line in dist/SKSE/Plugins/CrashFixCompendium.ini, in the section it registers
// (Tools/check_source.py).
// A line logged below error level must not contain error / fail / failed / failure: a scan of the log would flag it.

#include "fix.h"
#include "settings.h"

namespace
{
	// running out of trampoline stops the game at start for every player, so a fix's count one too low must not do that
	constexpr std::size_t kTrampolineSpare = 64;

	std::size_t TrampolineSize()
	{
		std::size_t size = kTrampolineSpare;
		for (const auto& fix : AllFixes()) {
			size += fix.trampoline;
		}
		return size;
	}

	constexpr std::string_view kShippedFile = "CrashFixCompendium.ini";
	constexpr std::string_view kCustomFile = "CrashFixCompendium_Custom.ini";

	std::filesystem::path         g_plugins;
	std::vector<Settings::Switch> g_switches;  // one per fix, in AllFixes' order

	void LogResult(std::string_view a_key, const Result& a_result)
	{
		switch (a_result.outcome) {
		case Outcome::kActive:
			logger::info("[{}] active: {}"sv, a_key, a_result.detail);
			break;
		case Outcome::kNotNeeded:
			logger::info("[{}] not needed: {}"sv, a_key, a_result.detail);
			break;
		default:
			logger::error("[{}] failed: {}"sv, a_key, a_result.detail);
			break;
		}
	}

	void LogOff(std::string_view a_key, Settings::From a_from)
	{
		switch (a_from) {
		case Settings::From::kShipped:
			logger::info("[{0}] off by default ({0}=1 in {1} enables it)"sv, a_key, kCustomFile);
			break;
		case Settings::From::kCustom:
			logger::info("[{}] disabled by {}"sv, a_key, kCustomFile);
			break;
		default:
			logger::error("[{}] failed: no line in {}, reinstall the mod"sv, a_key, kShippedFile);
			break;
		}
	}

	void ReadSwitches()
	{
		// Fixes first: a new custom file then starts with them
		std::vector<std::string_view> sections{ "Fixes" };
		for (const auto& fix : AllFixes()) {
			if (std::ranges::find(sections, fix.section) == sections.end()) {
				sections.push_back(fix.section);
			}
		}

		const auto custom = g_plugins / kCustomFile;
		for (const auto section : sections) {
			std::vector<std::string_view> keys;
			for (const auto& fix : AllFixes()) {
				if (fix.section == section) {
					keys.push_back(fix.key);
				}
			}
			const auto sync = Settings::SyncCustom(custom, keys, section);
			if (sync.generated) {
				logger::info("generated {}"sv, kCustomFile);
			}
			for (const auto& key : sync.added) {
				logger::info("added to {}: {}"sv, kCustomFile, key);
			}
			if (!sync.error.empty()) {
				logger::error("failed: could not update {} ({})"sv, kCustomFile, sync.error);
			}
		}

		for (const auto& fix : AllFixes()) {
			g_switches.push_back(Settings::Read(g_plugins / kShippedFile, custom, fix.key, fix.section));
			if (g_switches.back().badOverride) {
				logger::info("[{}] value in {} is not 0 or 1: using the shipped setting"sv, fix.key, kCustomFile);
			}
		}
	}

	void Run(bool a_atLoad)
	{
		for (std::size_t i = 0; i < AllFixes().size(); ++i) {
			const auto& fix = AllFixes()[i];
			if (fix.atLoad != a_atLoad) {
				continue;
			}
			std::error_code ec;
			if (!g_switches[i].on) {
				LogOff(fix.key, g_switches[i].from);
			} else if (!fix.oldDll.empty() && std::filesystem::exists(g_plugins / fix.oldDll, ec)) {
				logger::info("[{}] skipped: old {} is installed, remove it"sv, fix.key, fix.oldDll);
			} else {
				LogResult(fix.key, fix.install());
			}
		}
	}
}

SKSE_PLUGIN_LOAD(const SKSE::LoadInterface* a_skse)
{
	SKSE::Init(a_skse, { .log = true, .trampoline = true, .trampolineSize = TrampolineSize() });
	logger::info("{} v{} on {}"sv, SKSE::GetPluginName(), SKSE::GetPluginVersion().string(),
		REL::Module::get().version().string());
	// SE and AE code was read; VR was not (no VR executable to check its ids and code against).
	// IsVR() is always false in a build without ENABLE_SKYRIM_VR, and there every id resolves to address 0.
	// Minor version 4 is VR.
	if (REL::Module::get().version()[1] == 4) {
		logger::error("failed: VR is not supported, nothing patched"sv);
		return true;
	}

	std::wstring exe(32768, L'\0');
	exe.resize(GetModuleFileNameW(nullptr, exe.data(), static_cast<DWORD>(exe.size())));
	g_plugins = Settings::PluginsFolder(exe);
	if (g_plugins.empty()) {
		logger::error("failed: could not find the game's folder, nothing patched"sv);
		return true;
	}
	std::ranges::sort(AllFixes(), {}, &Fix::key);  // they register in link order: the same order every build and log
	ReadSwitches();

	Run(true);
	const auto onMessage = [](SKSE::MessagingInterface::Message* a_msg) {
		if (a_msg->type == SKSE::MessagingInterface::kPostLoad) {
			Run(false);
		}
	};
	if (!SKSE::GetMessagingInterface()->RegisterListener(onMessage)) {
		logger::error("failed: could not listen for SKSE's messages, only the fixes that install at load are on"sv);
	}
	return true;
}
