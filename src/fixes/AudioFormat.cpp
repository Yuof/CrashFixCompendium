// XAudio2_7.dll+25181 `mov ecx, [rbp]` in CLeapBufferPool::GetBuffer, on the audio thread when a sound starts.
// Cause: the game gives XAudio2 a voice with 0 channels. XAudio2 2.7 doesn't check: it looks up a buffer at index
// channels - 1 and reads far past its table. The channel count comes from the sound file's header. When the game
// can't read the header (a 32-bit float or ADPCM .wav, a renamed .mp3, a cut-off file), the async open still reports
// success and the format keeps whatever the memory held, usually zeros. A header can also just say 0 channels.
// With 0 bytes per sample frame (PCM) or 0 bits per sample (xWMA) Prepare divides by zero first.
// Fix: wrap BSXAudio2GameSound::Prepare. A sound with such a format fails the way a missing file does and stays
// silent. The log gives its id; AudioFormatScan finds the file.

#include "fix.h"

namespace AudioFormat
{
	namespace
	{
		constexpr REL::RelocationID kPrepare{ 66762, 68003 };
		constexpr REL::RelocationID kOpenDone{ 66626, 67872 };  // (sound, ok): what each open step calls last
		constexpr std::size_t       kPrepareSlot = 0xB;

		// the whole function; SE and AE differ
		constexpr auto kOpenDoneSE =
			"\x84\xD2\x74\x13\x8B\x41\x5C\x0F\xBA\xF0\x0C\x0D\x01\x01\x00\x00\x89\x41\x5C\x0F\xB6\xC2\xC3\x81\x61\x5C\xFF"
			"\xEE\xFF\xFF\x0F\xB6\xC2\x83\x49\x5C\x11\xC7\x41\x60\x01\x00\x00\x00\xC3"sv;
		constexpr auto kOpenDoneAE =
			"\x8B\x41\x5C\x84\xD2\x74\x10\x0F\xBA\xF0\x0C\x0D\x01\x01\x00\x00\x89\x41\x5C\x0F\xB6\xC2\xC3\x25\xFF\xEE\xFF"
			"\xFF\xC7\x41\x60\x01\x00\x00\x00\x83\xC8\x11\x89\x41\x5C\x0F\xB6\xC2\xC3"sv;

		constexpr std::uint32_t kHeaderRead = 1;  // the header parser found a fmt and a data chunk
		constexpr std::uint16_t kPcm = 1;
		constexpr std::uint16_t kXwma = 0x161;

		using Prepare_t = bool (*)(RE::BSXAudio2GameSound*);
		using OpenDone_t = bool (*)(RE::BSGameSound*, bool);

		Prepare_t                  g_prepare = nullptr;
		OpenDone_t                 g_openDone = nullptr;
		std::atomic<std::uint64_t> g_silenced{ 0 };

		// empty when XAudio2 can take the sound
		std::string_view WhyUnplayable(const RE::BSXAudio2DataSrc* a_data)
		{
			if (!a_data) {
				return "it has no data";
			}
			if (!(a_data->unk78 & kHeaderRead)) {
				return "the game could not read its file header";
			}
			const auto& format = a_data->format;
			if (format.nChannels == 0) {
				return "its file header says 0 channels";
			}
			if (format.wFormatTag == kPcm && format.nBlockAlign == 0) {
				return "its file header says 0 bytes per sample frame";
			}
			if (format.wFormatTag == kXwma && format.wBitsPerSample == 0) {
				return "its file header says 0 bits per sample";
			}
			return {};
		}

		bool PrepareThunk(RE::BSXAudio2GameSound* a_sound)
		{
			const auto why = WhyUnplayable(a_sound->src);
			if (why.empty()) {
				return g_prepare(a_sound);
			}
			if (const auto n = ++g_silenced; Milestone(n)) {
				const auto& id = a_sound->resourceID;
				logger::warn(
					"[AudioFormat] a sound was not played: {} (sound id {:08X}/{:08X}; use AudioFormatScan=1 to scan the "
					"file) ({} so far)"sv,
					why, id.dir, id.file, n);
			}
			return g_openDone(a_sound, false);
		}
	}

	Result Install()
	{
		const auto expected = REL::Module::IsAE() ? kOpenDoneAE : kOpenDoneSE;
		if (Bytes(kOpenDone.address(), expected.size()) != expected) {
			return { Outcome::kFailed, "the sound open step's last call is not the expected code, nothing patched" };
		}
		REL::Relocation<std::uintptr_t> vtbl{ RE::VTABLE_BSXAudio2GameSound[0] };
		const auto current = *reinterpret_cast<const std::uintptr_t*>(vtbl.address() + kPrepareSlot * sizeof(void*));
		if (current != kPrepare.address()) {
			return { Outcome::kFailed,
				std::format("BSXAudio2GameSound::Prepare is already replaced (by {:X}), nothing patched", current) };
		}

		g_openDone = reinterpret_cast<OpenDone_t>(kOpenDone.address());
		g_prepare = reinterpret_cast<Prepare_t>(vtbl.write_vfunc(kPrepareSlot, PrepareThunk));
		return { Outcome::kActive,
			"hooked BSXAudio2GameSound::Prepare: a sound whose file header is unreadable or says 0 channels stays "
			"silent" };
	}

	const bool kRegistered = Register({
		.key = "AudioFormat",
		.install = Install,
	});
}
