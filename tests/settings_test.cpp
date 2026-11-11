// Offline tests of the two INI files and the code helpers in fix.h. Runs in a temp folder with a non-ASCII name: a
// Steam library can have one.
#include "fix.h"
#include "settings.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include <Windows.h>

namespace fs = std::filesystem;

namespace
{
	int g_failures = 0;

#define CHECK(cond)                                                       \
	do {                                                                  \
		if (!(cond)) {                                                    \
			std::printf("FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond); \
			++g_failures;                                                 \
		}                                                                 \
	} while (false)

	const fs::path kDir = fs::temp_directory_path() / L"CFC_Ü_test";
	const fs::path kShipped = kDir / L"CrashFixCompendium.ini";
	const fs::path kCustom = kDir / L"CrashFixCompendium_Custom.ini";

	const std::string kHeader =
		"; Crash Fix Compendium - your settings. Updates never replace this file.\r\n"
		"; Remove the ; from a line and set 1 (on) or 0 (off). A line left commented follows "
		"CrashFixCompendium.ini,\r\n"
		"; which describes each fix.\r\n"
		"\r\n"
		"[Fixes]\r\n";

	void Put(const fs::path& a_path, const std::string& a_bytes)
	{
		std::ofstream(a_path, std::ios::binary | std::ios::trunc)
			.write(a_bytes.data(), static_cast<std::streamsize>(a_bytes.size()));
	}

	std::string Get(const fs::path& a_path)
	{
		std::ifstream in(a_path, std::ios::binary);
		return { std::istreambuf_iterator<char>(in), {} };
	}

	void Reset()
	{
		std::error_code ec;
		SetFileAttributesW(kCustom.c_str(), FILE_ATTRIBUTE_NORMAL);
		fs::remove_all(kDir, ec);
		fs::create_directories(kDir);
		Put(kShipped, "[Fixes]\r\nPathIdle=1\r\nOther=0\r\n");
	}

	constexpr std::string_view kBoth[] = { "PathIdle", "Other" };

	void shipped_used_without_custom()
	{
		Reset();
		const auto a = Settings::Read(kShipped, kCustom, "PathIdle");
		CHECK(a.on);
		CHECK(a.from == Settings::From::kShipped);
		const auto b = Settings::Read(kShipped, kCustom, "Other");
		CHECK(!b.on);
		CHECK(b.from == Settings::From::kShipped);
	}

	void custom_overrides_both_ways()
	{
		Reset();
		Put(kCustom, "[Fixes]\r\nPathIdle=0\r\nOther=1\r\n");
		const auto a = Settings::Read(kShipped, kCustom, "PathIdle");
		CHECK(!a.on);
		CHECK(a.from == Settings::From::kCustom);
		const auto b = Settings::Read(kShipped, kCustom, "Other");
		CHECK(b.on);
		CHECK(b.from == Settings::From::kCustom);
	}

	void commented_line_pins_nothing()
	{
		Reset();
		Put(kCustom, "[Fixes]\r\n;PathIdle=0\r\n");
		const auto a = Settings::Read(kShipped, kCustom, "PathIdle");
		CHECK(a.on);
		CHECK(a.from == Settings::From::kShipped);
		CHECK(!a.badOverride);
	}

	void bad_override_keeps_shipped()
	{
		for (const auto* value : { "true", "0 ; crashes", "" }) {
			Reset();
			Put(kCustom, std::string("[Fixes]\r\nPathIdle=") + value + "\r\n");
			const auto a = Settings::Read(kShipped, kCustom, "PathIdle");
			CHECK(a.on);
			CHECK(a.from == Settings::From::kShipped);
			CHECK(a.badOverride);
		}
	}

	void whitespace_and_case_accepted()
	{
		for (const auto* line : { "PathIdle = 0", "pathidle=0" }) {
			Reset();
			Put(kCustom, std::string("[Fixes]\r\n") + line + "\r\n");
			const auto a = Settings::Read(kShipped, kCustom, "PathIdle");
			CHECK(!a.on);
			CHECK(a.from == Settings::From::kCustom);
		}
	}

	void missing_from_shipped_is_off()
	{
		Reset();
		fs::remove(kShipped);
		const auto a = Settings::Read(kShipped, kCustom, "PathIdle");
		CHECK(!a.on);
		CHECK(a.from == Settings::From::kMissing);

		Put(kShipped, "[Fixes]\r\nPathIdle=x\r\n");
		const auto b = Settings::Read(kShipped, kCustom, "PathIdle");
		CHECK(!b.on);
		CHECK(b.from == Settings::From::kMissing);
	}

	void generate_commented()
	{
		Reset();
		const auto s = Settings::SyncCustom(kCustom, kBoth);
		CHECK(s.generated);
		CHECK(s.added.empty());
		CHECK(s.error.empty());
		CHECK(Get(kCustom) == kHeader + ";PathIdle=1\r\n;Other=1\r\n");
		const auto a = Settings::Read(kShipped, kCustom, "PathIdle");
		CHECK(a.on);
		CHECK(a.from == Settings::From::kShipped);
		const auto b = Settings::Read(kShipped, kCustom, "Other");
		CHECK(!b.on);
		CHECK(b.from == Settings::From::kShipped);
	}

	void generate_unwritable_reports_error()
	{
		Reset();
		const auto s = Settings::SyncCustom(kDir / L"no_such_folder" / L"x.ini", kBoth);
		CHECK(!s.generated);
		CHECK(!s.error.empty());
	}

	void sync_is_idempotent()
	{
		Reset();
		Settings::SyncCustom(kCustom, kBoth);
		const auto before = Get(kCustom);
		const auto s = Settings::SyncCustom(kCustom, kBoth);
		CHECK(!s.generated);
		CHECK(s.added.empty());
		CHECK(s.error.empty());
		CHECK(Get(kCustom) == before);
	}

	void sync_adds_only_missing()
	{
		Reset();
		const std::string old = "[Fixes]\r\nPathIdle=0\r\n; my note\r\n";
		Put(kCustom, old);
		constexpr std::string_view keys[] = { "PathIdle", "New" };
		const auto                 s = Settings::SyncCustom(kCustom, keys);
		CHECK(!s.generated);
		CHECK(s.added == std::vector<std::string>{ "New" });
		CHECK(Get(kCustom) == old + ";New=1\r\n");
		CHECK(!Settings::Read(kShipped, kCustom, "PathIdle").on);
	}

	void sync_recognises_lines()
	{
		constexpr std::string_view key[] = { "PathIdle" };
		for (const auto* present : { ";pathidle=1", "  ; PathIdle = 1", "PATHIDLE=0" }) {
			Reset();
			Put(kCustom, std::string("[Fixes]\r\n") + present + "\r\n");
			const auto s = Settings::SyncCustom(kCustom, key);
			CHECK(s.added.empty());
		}
		for (const auto* lookalike : { "PathIdleOld=1", "; PathIdle does this", ";;PathIdle=1" }) {
			Reset();
			Put(kCustom, std::string("[Fixes]\r\n") + lookalike + "\r\n");
			const auto s = Settings::SyncCustom(kCustom, key);
			CHECK(s.added == std::vector<std::string>{ "PathIdle" });
		}
	}

	void sync_line_endings()
	{
		Reset();
		const std::string old = "[Fixes]\nOther=0";
		Put(kCustom, old);
		constexpr std::string_view keys[] = { "Other", "New" };
		const auto                 s = Settings::SyncCustom(kCustom, keys);
		CHECK(s.added == std::vector<std::string>{ "New" });
		CHECK(Get(kCustom) == old + "\r\n;New=1\r\n");
		CHECK(!Settings::Read(kShipped, kCustom, "Other").on);
		CHECK(Settings::Read(kShipped, kCustom, "Other").from == Settings::From::kCustom);
	}

	void sync_adds_section()
	{
		Reset();
		const std::string old = "; just a note\r\n";
		Put(kCustom, old);
		constexpr std::string_view key[] = { "PathIdle" };
		Settings::SyncCustom(kCustom, key);
		CHECK(Get(kCustom) == old + "[Fixes]\r\n;PathIdle=1\r\n");
		Put(kCustom, old + "[Fixes]\r\nPathIdle=0\r\n");
		const auto a = Settings::Read(kShipped, kCustom, "PathIdle");
		CHECK(!a.on);
		CHECK(a.from == Settings::From::kCustom);
	}

	void sync_refuses_utf16()
	{
		Reset();
		const std::wstring text = L"[Fixes]\r\nPathIdle=0\r\n";
		std::string        bytes = "\xFF\xFE";
		bytes.append(reinterpret_cast<const char*>(text.data()), text.size() * sizeof(wchar_t));
		Put(kCustom, bytes);
		constexpr std::string_view keys[] = { "PathIdle", "New" };
		const auto                 s = Settings::SyncCustom(kCustom, keys);
		CHECK(!s.error.empty());
		CHECK(s.added.empty());
		CHECK(Get(kCustom) == bytes);
		const auto a = Settings::Read(kShipped, kCustom, "PathIdle");
		CHECK(!a.on);
		CHECK(a.from == Settings::From::kCustom);
	}

	void utf8_bom_reads()
	{
		Reset();
		Put(kCustom, "\xEF\xBB\xBF" + kHeader + "PathIdle=0\r\n");
		const auto a = Settings::Read(kShipped, kCustom, "PathIdle");
		CHECK(!a.on);
		CHECK(a.from == Settings::From::kCustom);
	}

	void milestone_is_powers_of_ten()
	{
		for (const std::uint64_t n : { 1ull, 10ull, 100ull, 1000ull }) {
			CHECK(Milestone(n));
		}
		for (const std::uint64_t n : { 2ull, 11ull, 20ull, 101ull, 110ull }) {
			CHECK(!Milestone(n));
		}
	}

	void find_call_wants_exactly_one()
	{
		std::uint8_t code[32]{};
		const auto   start = reinterpret_cast<std::uintptr_t>(code);
		const auto   target = start + 20;
		const auto   putCall = [&](std::size_t a_at) {
			code[a_at] = 0xE8;
			const auto rel = static_cast<std::int32_t>(target - (start + a_at + 5));
			std::memcpy(code + a_at + 1, &rel, sizeof(rel));
		};
		const auto none = FindCall(start, sizeof(code), target);
		CHECK(none.site == 0);
		CHECK(none.count == 0);
		putCall(4);
		const auto one = FindCall(start, sizeof(code), target);
		CHECK(one.site == start + 4);
		CHECK(one.count == 1);
		CHECK(FindCall(start, 8, target).count == 0);                  // the call's last byte is outside the span
		CHECK(FindCall(start, sizeof(code), target + 1).count == 0);  // a call to somewhere else
		putCall(12);
		const auto two = FindCall(start, sizeof(code), target);
		CHECK(two.site == 0);
		CHECK(two.count == 2);
		CHECK(none.Why() != two.Why());
	}

	void abs_jump_round_trip()
	{
		std::uint8_t           code[kAbsJumpSize]{};
		constexpr std::uint8_t want[kAbsJumpSize]{ 0xFF, 0x25, 0, 0, 0, 0, 0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22,
			0x11 };
		const auto             at = reinterpret_cast<std::uintptr_t>(code);
		CHECK(!IsAbsJump(at));
		AbsJump(code, 0x1122334455667788ull);
		CHECK(std::memcmp(code, want, sizeof(want)) == 0);
		CHECK(IsAbsJump(at));
		CHECK(AbsJumpTarget(at) == 0x1122334455667788ull);
	}

	void rel32_target_goes_both_ways()
	{
		std::uint8_t code[5]{ 0xE9 };
		const auto   at = reinterpret_cast<std::uintptr_t>(code);
		for (const std::int32_t rel : { 0x10, -0x10 }) {
			std::memcpy(code + 1, &rel, sizeof(rel));
			CHECK(Rel32Target(at) == at + 5 + rel);
		}
	}

	void detour_bytes()
	{
		constexpr std::string_view prologue = "\x48\x8B\xC4\x53\x90";
		std::uint8_t               stub[prologue.size() + kAbsJumpSize]{};
		WriteOriginal(stub, prologue, 0x1000);
		CHECK(Bytes(reinterpret_cast<std::uintptr_t>(stub), prologue.size()) == prologue);
		CHECK(AbsJumpTarget(reinterpret_cast<std::uintptr_t>(stub + prologue.size())) == 0x1000 + prologue.size());

		const auto patch = EntryPatch(16, 0x2000);
		CHECK(patch.size() == 16);
		CHECK(AbsJumpTarget(reinterpret_cast<std::uintptr_t>(patch.data())) == 0x2000);
		CHECK(patch[kAbsJumpSize] == 0x90);
		CHECK(patch[15] == 0x90);
	}

	// Windows reads "BOM + [Fixes]" as no section at all: the player's lines would be ignored while the log says active
	void utf8_bom_on_section_line_reports_error()
	{
		Reset();
		const std::string bytes = "\xEF\xBB\xBF[Fixes]\r\nPathIdle=0\r\n";
		Put(kCustom, bytes);
		constexpr std::string_view keys[] = { "PathIdle", "New" };
		const auto                 s = Settings::SyncCustom(kCustom, keys);
		CHECK(!s.error.empty());
		CHECK(s.added.empty());
		CHECK(Get(kCustom) == bytes);
	}

	// Windows takes "[Fixes] ; mine" as the section and reads keys only from the first one: a second header's lines
	// would be ignored
	void section_with_a_trailing_comment_counts()
	{
		Reset();
		const std::string old = "[Fixes] ; mine\r\nPathIdle=0\r\n";
		Put(kCustom, old);
		constexpr std::string_view keys[] = { "PathIdle", "New" };
		const auto                 s = Settings::SyncCustom(kCustom, keys);
		CHECK(s.added == std::vector<std::string>{ "New" });
		CHECK(Get(kCustom) == old + ";New=1\r\n");
		CHECK(!Settings::Read(kShipped, kCustom, "PathIdle").on);
	}

	void read_by_section()
	{
		Reset();
		Put(kShipped, "[Fixes]\r\nPathIdle=1\r\n[Reports]\r\nScan=0\r\n");
		const auto a = Settings::Read(kShipped, kCustom, "Scan", "Reports");
		CHECK(!a.on);
		CHECK(a.from == Settings::From::kShipped);
		CHECK(Settings::Read(kShipped, kCustom, "Scan").from == Settings::From::kMissing);
		Put(kCustom, "[Fixes]\r\nPathIdle=0\r\n[Reports]\r\nScan=1\r\n");
		CHECK(Settings::Read(kShipped, kCustom, "Scan", "Reports").on);
		CHECK(!Settings::Read(kShipped, kCustom, "PathIdle").on);
	}

	// a file from before the Reports section gets it at the end
	void sync_adds_a_second_section()
	{
		Reset();
		const std::string old = "[Fixes]\r\nPathIdle=0\r\n";
		Put(kCustom, old);
		constexpr std::string_view key[] = { "Scan" };
		const auto                 s = Settings::SyncCustom(kCustom, key, "Reports");
		CHECK(s.added == std::vector<std::string>{ "Scan" });
		CHECK(Get(kCustom) == old + "[Reports]\r\n;Scan=1\r\n");
	}

	// a later fix goes into Fixes, not into the section that ends the file
	void sync_inserts_into_its_section()
	{
		Reset();
		const std::string head = "[Fixes]\r\nPathIdle=0\r\n";
		const std::string tail = "\r\n[Reports]\r\n;Scan=1\r\n";
		Put(kCustom, head + tail);
		constexpr std::string_view keys[] = { "PathIdle", "New" };
		const auto                 s = Settings::SyncCustom(kCustom, keys);
		CHECK(s.added == std::vector<std::string>{ "New" });
		CHECK(Get(kCustom) == head + ";New=1\r\n" + tail);

		Put(kCustom, head + "New=0\r\n" + tail);
		CHECK(Settings::Read(kShipped, kCustom, "New").from == Settings::From::kCustom);

		Put(kCustom, "[Fixes]\r\n[Reports]\r\n");  // an empty section: the line goes right under its header
		Settings::SyncCustom(kCustom, keys);
		CHECK(Get(kCustom) == "[Fixes]\r\n;PathIdle=1\r\n;New=1\r\n[Reports]\r\n");
	}

	void sync_insert_keeps_a_readonly_file()
	{
		Reset();
		const std::string old = "[Fixes]\r\nPathIdle=0\r\n[Reports]\r\n";
		Put(kCustom, old);
		SetFileAttributesW(kCustom.c_str(), FILE_ATTRIBUTE_READONLY);
		constexpr std::string_view keys[] = { "PathIdle", "New" };
		const auto                 s = Settings::SyncCustom(kCustom, keys);
		CHECK(!s.error.empty());
		CHECK(Get(kCustom) == old);
	}

	// why a line goes into the section that is there: Windows reads a key only from a section's first header
	void second_header_is_not_read()
	{
		Reset();
		Put(kCustom, "[Fixes]\r\nPathIdle=0\r\n[Reports]\r\n[Fixes]\r\nOther=1\r\n");
		CHECK(Settings::Read(kShipped, kCustom, "PathIdle").from == Settings::From::kCustom);
		CHECK(Settings::Read(kShipped, kCustom, "Other").from == Settings::From::kShipped);
	}

	// both syncs the way main.cpp runs them at start: Fixes, then Reports
	void start_up_sync()
	{
		constexpr std::string_view fixes[] = { "PathIdle", "New" };
		constexpr std::string_view reports[] = { "Scan" };

		Reset();  // a first start: no file
		CHECK(Settings::SyncCustom(kCustom, fixes).generated);
		CHECK(Settings::SyncCustom(kCustom, reports, "Reports").added == std::vector<std::string>{ "Scan" });
		CHECK(Get(kCustom) == kHeader + ";PathIdle=1\r\n;New=1\r\n[Reports]\r\n;Scan=1\r\n");

		Reset();  // a 1.3.0 player's file
		Put(kCustom, kHeader + ";PathIdle=1\r\n");
		CHECK(Settings::SyncCustom(kCustom, fixes).added == std::vector<std::string>{ "New" });
		CHECK(Settings::SyncCustom(kCustom, reports, "Reports").added == std::vector<std::string>{ "Scan" });
		CHECK(Get(kCustom) == kHeader + ";PathIdle=1\r\n;New=1\r\n[Reports]\r\n;Scan=1\r\n");

		constexpr std::string_view later[] = { "PathIdle", "New", "Later" };  // the next update's fix
		CHECK(Settings::SyncCustom(kCustom, later).added == std::vector<std::string>{ "Later" });
		CHECK(Settings::SyncCustom(kCustom, reports, "Reports").added.empty());
		CHECK(Get(kCustom) == kHeader + ";PathIdle=1\r\n;New=1\r\n;Later=1\r\n[Reports]\r\n;Scan=1\r\n");
	}

	// an empty exe path must not become relative INI paths: GetPrivateProfile* would look in the Windows folder
	void plugins_folder_from_the_exe()
	{
		CHECK(Settings::PluginsFolder(L"C:\\Games\\Stock Game\\SkyrimSE.exe") ==
			  fs::path(L"C:\\Games\\Stock Game\\Data\\SKSE\\Plugins"));
		CHECK(Settings::PluginsFolder(L"").empty());
	}

	void readonly_reports_error()
	{
		Reset();
		const std::string old = "[Fixes]\r\nPathIdle=0\r\n";
		Put(kCustom, old);
		SetFileAttributesW(kCustom.c_str(), FILE_ATTRIBUTE_READONLY);
		constexpr std::string_view keys[] = { "PathIdle", "New" };
		const auto                 s = Settings::SyncCustom(kCustom, keys);
		CHECK(!s.error.empty());
		CHECK(s.added.empty());
		CHECK(Get(kCustom) == old);
	}
}

int main()
{
	shipped_used_without_custom();
	custom_overrides_both_ways();
	commented_line_pins_nothing();
	bad_override_keeps_shipped();
	whitespace_and_case_accepted();
	missing_from_shipped_is_off();
	generate_commented();
	generate_unwritable_reports_error();
	sync_is_idempotent();
	sync_adds_only_missing();
	sync_recognises_lines();
	sync_line_endings();
	sync_adds_section();
	sync_refuses_utf16();
	utf8_bom_reads();
	readonly_reports_error();
	utf8_bom_on_section_line_reports_error();
	section_with_a_trailing_comment_counts();
	read_by_section();
	sync_adds_a_second_section();
	sync_inserts_into_its_section();
	sync_insert_keeps_a_readonly_file();
	second_header_is_not_read();
	start_up_sync();
	plugins_folder_from_the_exe();
	milestone_is_powers_of_ten();
	find_call_wants_exactly_one();
	abs_jump_round_trip();
	rel32_target_goes_both_ways();
	detour_bytes();

	std::error_code ec;
	SetFileAttributesW(kCustom.c_str(), FILE_ATTRIBUTE_NORMAL);
	fs::remove_all(kDir, ec);

	if (g_failures) {
		std::printf("%d failed\n", g_failures);
		return 1;
	}
	std::puts("all passed");
	return 0;
}
