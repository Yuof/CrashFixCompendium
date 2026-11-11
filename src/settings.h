#pragma once

#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// The per-fix switches: CrashFixCompendium.ini is shipped and holds the defaults, an update replaces it.
// CrashFixCompendium_Custom.ini is the player's and overrides it. Only a line the player activated counts there.
namespace Settings
{
	enum class From
	{
		kShipped,
		kCustom,
		kMissing  // no usable line in the shipped file: the fix stays off
	};

	struct Switch
	{
		bool on;
		From from;
		bool badOverride;  // the custom file has a value for it that isn't 0 or 1
	};

	// The folder SKSE itself loads plugins from, however a mod manager reports this DLL's own path.
	// Empty for an empty exe path.
	std::filesystem::path PluginsFolder(std::wstring_view a_exe);

	Switch Read(const std::filesystem::path& a_shipped, const std::filesystem::path& a_custom, std::string_view a_key,
		std::string_view a_section = "Fixes");

	struct Sync
	{
		bool                     generated = false;
		std::vector<std::string> added;
		std::string              error;
	};

	// Generates the custom file, or adds a commented-out line for each key of a_section it has no line for, commented
	// or not, at the end of that section. Never changes a line that is there.
	Sync SyncCustom(const std::filesystem::path& a_custom, std::span<const std::string_view> a_keys,
		std::string_view a_section = "Fixes");
}
