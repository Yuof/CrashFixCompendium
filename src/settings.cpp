#include "settings.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iterator>
#include <optional>

#include <Windows.h>

namespace
{
	constexpr wchar_t kAbsent[] = L"\x01";

	constexpr std::string_view kHeader =
		"; Crash Fix Compendium - your settings. Updates never replace this file.\r\n"
		"; Remove the ; from a line and set 1 (on) or 0 (off). A line left commented follows "
		"CrashFixCompendium.ini,\r\n"
		"; which describes each fix.\r\n"
		"\r\n";

	std::wstring Widen(std::string_view a_text)
	{
		return { a_text.begin(), a_text.end() };  // keys are ASCII
	}

	// GetPrivateProfileString, not Int: Int reads "true" as 0, which would silently disable a fix
	std::optional<std::wstring> Value(const std::filesystem::path& a_file, const std::wstring& a_section,
		const std::wstring& a_key)
	{
		wchar_t buffer[256];
		GetPrivateProfileStringW(a_section.c_str(), a_key.c_str(), kAbsent, buffer,
			static_cast<DWORD>(std::size(buffer)), a_file.c_str());
		if (std::wstring_view{ buffer } == kAbsent) {
			return std::nullopt;
		}
		return buffer;
	}

	bool IsSwitch(const std::wstring& a_value) { return a_value == L"0" || a_value == L"1"; }

	std::string_view Trim(std::string_view a_text)
	{
		constexpr std::string_view space = " \t\r\n";
		a_text.remove_prefix(std::min(a_text.find_first_not_of(space), a_text.size()));
		a_text.remove_suffix(a_text.size() - (a_text.find_last_not_of(space) + 1));
		return a_text;
	}

	bool SameText(std::string_view a_left, std::string_view a_right)
	{
		return a_left.size() == a_right.size() && _strnicmp(a_left.data(), a_right.data(), a_left.size()) == 0;
	}

	// `Key=`, `;Key=`, `  ; key = 1`, but not `KeyOld=` or `; Key does this`
	bool IsLineOf(std::string_view a_line, std::string_view a_key)
	{
		auto line = Trim(a_line);
		if (line.starts_with(';')) {
			line = Trim(line.substr(1));
		}
		return SameText(line.substr(0, a_key.size()), a_key) && Trim(line.substr(a_key.size())).starts_with('=');
	}

	// Windows accepts anything after the closing bracket
	bool IsHeaderOf(std::string_view a_line, std::string_view a_header)
	{
		return SameText(Trim(a_line).substr(0, a_header.size()), a_header);
	}

	bool IsAnyHeader(std::string_view a_line) { return Trim(a_line).starts_with('['); }

	std::vector<std::string_view> Lines(std::string_view a_text)
	{
		std::vector<std::string_view> lines;
		while (!a_text.empty()) {
			const auto end = a_text.find('\n');
			lines.push_back(a_text.substr(0, end));
			a_text = end == std::string_view::npos ? std::string_view{} : a_text.substr(end + 1);
		}
		return lines;
	}

	bool Append(const std::filesystem::path& a_file, std::string_view a_bytes)
	{
		std::ofstream out(a_file, std::ios::binary | std::ios::app);
		return out.write(a_bytes.data(), static_cast<std::streamsize>(a_bytes.size())).flush().good();
	}

	// In place, like Append: a temporary file renamed over it would leave MO2's Overwrite holding the player's file.
	// A read-only or locked file fails to open, before anything is written.
	bool Rewrite(const std::filesystem::path& a_file, std::string_view a_bytes)
	{
		std::ofstream out(a_file, std::ios::binary | std::ios::trunc);
		return out && out.write(a_bytes.data(), static_cast<std::streamsize>(a_bytes.size())).flush().good();
	}
}

namespace Settings
{
	std::filesystem::path PluginsFolder(std::wstring_view a_exe)
	{
		if (a_exe.empty()) {
			return {};
		}
		return std::filesystem::path(a_exe).parent_path() / L"Data" / L"SKSE" / L"Plugins";
	}

	Switch Read(const std::filesystem::path& a_shipped, const std::filesystem::path& a_custom, std::string_view a_key,
		std::string_view a_section)
	{
		const auto key = Widen(a_key);
		const auto section = Widen(a_section);
		Switch     result{ false, From::kMissing, false };
		if (const auto custom = Value(a_custom, section, key)) {
			if (IsSwitch(*custom)) {
				return { *custom == L"1", From::kCustom, false };
			}
			result.badOverride = true;
		}
		if (const auto shipped = Value(a_shipped, section, key); shipped && IsSwitch(*shipped)) {
			result.on = *shipped == L"1";
			result.from = From::kShipped;
		}
		return result;
	}

	Sync SyncCustom(const std::filesystem::path& a_custom, std::span<const std::string_view> a_keys,
		std::string_view a_section)
	{
		const auto header = "[" + std::string(a_section) + "]";
		std::error_code ec;
		const bool      exists = std::filesystem::exists(a_custom, ec);
		if (ec) {
			return { .error = "could not check for the file" };
		}

		std::string text;
		if (exists) {
			std::ifstream in(a_custom, std::ios::binary);
			if (!in) {
				return { .error = "could not read the file" };
			}
			text.assign(std::istreambuf_iterator<char>(in), {});
		}
		// appending ASCII would corrupt it, and a player can fix this by saving it as ANSI or UTF-8
		if (text.starts_with("\xFF\xFE") || text.starts_with("\xFE\xFF")) {
			return { .error = "saved as UTF-16, save it as ANSI or UTF-8" };
		}

		std::string_view body = text;
		if (body.starts_with("\xEF\xBB\xBF")) {
			body.remove_prefix(3);
			// Windows reads a BOM glued to a section header as no section: the player's lines would be ignored
			if (IsAnyHeader(body.substr(0, body.find('\n')))) {
				return { .error = "saved as UTF-8 with BOM, so Windows ignores its settings and the shipped ones "
					              "apply: save it as ANSI or UTF-8" };
			}
		}
		const auto lines = Lines(body);

		std::vector<std::string_view> missing;
		for (const auto key : a_keys) {
			if (std::ranges::none_of(lines, [&](std::string_view line) { return IsLineOf(line, key); })) {
				missing.push_back(key);
			}
		}
		if (exists && missing.empty()) {
			return {};
		}

		std::string newLines;
		for (const auto key : missing) {
			newLines += ';';
			newLines += key;
			newLines += "=1\r\n";
		}

		// Windows reads only the first header of a section, so a line goes into the section that is there. Before the
		// next section when there is one, after its last line that isn't blank.
		const auto own = std::ranges::find_if(lines, [&](std::string_view line) { return IsHeaderOf(line, header); });
		const auto next = own == lines.end() ? lines.end() : std::find_if(own + 1, lines.end(), IsAnyHeader);
		if (next != lines.end()) {
			auto last = next - 1;
			while (last != own && Trim(*last).empty()) {
				--last;
			}
			const auto at = static_cast<std::size_t>(last->data() - text.data()) + last->size() + 1;
			if (!Rewrite(a_custom, text.substr(0, at) + newLines + text.substr(at))) {
				return { .error = "the file is read-only or locked" };
			}
			return { .added = std::vector<std::string>(missing.begin(), missing.end()) };
		}

		std::string addition;
		if (!exists) {
			addition = kHeader;
		} else if (!text.empty() && text.back() != '\n') {
			addition = "\r\n";
		}
		if (own == lines.end()) {
			addition += header + "\r\n";
		}
		addition += newLines;

		if (!Append(a_custom, addition)) {
			return { .error = exists ? "the file is read-only or locked" : "could not create the file" };
		}
		if (!exists) {
			return { .generated = true };
		}
		return { .added = std::vector<std::string>(missing.begin(), missing.end()) };
	}
}
