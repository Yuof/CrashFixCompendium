// SkyrimSE+0CEC826 / +0CEC83E (1.6.1170) in BSFixedString's copy under NiObjectNET::LoadBinary, while a mesh loads.
// Cause: a .nif block names a string index past the file's string table. NiStream's string reader only checks for
// -1 and copies table[index], so it reads whatever follows the table.
// Fix: replace the reader. A bad index gives the block an empty string, and the log names the file.

#include "fix.h"

namespace NifStringIndex
{
	namespace
	{
		constexpr REL::RelocationID kReadString{ 68995, 70352 };

		// identical on SE and AE, up to `cmp eax, -1`
		constexpr auto kExpected =
			"\x48\x8B\xC4\x57\x48\x83\xEC\x40\x48\xC7\x40\xE8\xFE\xFF\xFF\xFF\x48\x89\x58\x10\x48\x89\x70\x20\x48\x8B"
			"\xFA\x48\x8B\xF1\x48\x8B\x99\x98\x02\x00\x00\xC7\x40\x08\x04\x00\x00\x00\xC7\x40\xD8\x01\x00\x00\x00\x4C"
			"\x8D\x48\x08\x41\xB8\x04\x00\x00\x00\x48\x8D\x50\x18\x48\x8B\xCB\xFF\x53\x10\x01\x43\x08\x8B\x44\x24\x60"
			"\x83\xF8\xFF"sv;

		constexpr std::uint32_t kNoString = 0xFFFFFFFF;

		std::atomic<std::uint64_t> g_badIndices{ 0 };

		void ReadString(RE::NiStream* a_stream, RE::BSFixedString* a_out)
		{
			std::uint32_t index = kNoString;
			a_stream->iStr->read(&index, 1);

			const auto& table = a_stream->fixedStrings;
			if (index < table.capacity()) {
				*a_out = table.begin()[index];
				return;
			}

			*a_out = RE::BSFixedString();
			if (index == kNoString) {
				return;
			}
			if (const auto n = ++g_badIndices; Milestone(n)) {
				logger::warn(
					"[NifStringIndex] \"{}\": a block asks for string {} of {}, read as an empty string ({} so far)"sv,
					a_stream->inputFilePath, index, table.capacity(), n);
			}
		}
	}

	Result Install()
	{
		const auto target = kReadString.address();
		if (Bytes(target, kExpected.size()) != kExpected) {
			return { Outcome::kFailed, "the .nif string reader is not the expected code, nothing patched" };
		}

		// 16 bytes = whole instructions
		const auto patch = EntryPatch(16, reinterpret_cast<std::uintptr_t>(&ReadString));
		REL::safe_write(target, patch.data(), patch.size());
		return { Outcome::kActive,
			"replaced the .nif string reader: a string index past the file's table reads as empty" };
	}

	const bool kRegistered = Register({
		.key = "NifStringIndex",
		.install = Install,
	});
}
