// SkyrimSE+093BA03 (1.6.1170), `mov rax, [r14]` with r14 = 0 in MagicMenu::ProcessMessage: the unlock key on a mod's
// shout with fewer than three words.
// Cause: the helper that returns a shout's last known word index skips empty slots as if they were known. A one-word
// shout with its word learned answers 2, and the Magic menu calls into the empty slots.
// Fix: replace the helper. Trailing empty slots no longer count. It still skips an empty slot in front of a word
// (some dragon shouts).

#include "fix.h"

namespace ShoutEmptyWord
{
	namespace
	{
		constexpr REL::RelocationID kLastKnownWord{ 22900, 23357 };

		// identical on SE and AE, up to the end of the loop
		constexpr auto kExpected =
			"\x48\x89\x5C\x24\x08\x57\x48\x83\xEC\x20\x33\xDB\x48\x8D\x79\x60\x48\x8B\x0F\x48\x85\xC9\x74\x0D\x48\x8B"
			"\x01\xFF\x90\xB8\x00\x00\x00\x84\xC0\x74\x0B\xFF\xC3\x48\x83\xC7\x18\x83\xFB\x03\x7C\xE0"sv;

		std::int32_t LastKnownWord(const RE::TESShout* a_shout)
		{
			const auto& slots = a_shout->variations;

			std::int32_t count = RE::TESShout::VariationIDs::kTotal;
			while (count > 0 && !slots[count - 1].word) {
				--count;
			}

			std::int32_t i = 0;
			while (i < count && (!slots[i].word || slots[i].word->GetKnown())) {
				++i;
			}
			return i - 1;
		}
	}

	Result Install()
	{
		const auto target = kLastKnownWord.address();
		if (Bytes(target, kExpected.size()) != kExpected) {
			return { Outcome::kFailed, "the shout word helper is not the expected code, nothing patched" };
		}

		// 16 bytes = whole instructions
		const auto patch = EntryPatch(16, reinterpret_cast<std::uintptr_t>(&LastKnownWord));
		REL::safe_write(target, patch.data(), patch.size());
		return { Outcome::kActive, "replaced the shout word helper: its answer ends at a shout's last word" };
	}

	const bool kRegistered = Register({
		.key = "ShoutEmptyWord",
		.install = Install,
	});
}
