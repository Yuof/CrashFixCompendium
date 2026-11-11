// Tests for audio_header.h: which sound files the game plays, which it doesn't, and which crash it.
#include "audio_header.h"

#include <cstdio>
#include <cstring>
#include <string>

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

	void Put16(std::string& a_out, std::uint16_t a_v) { a_out.append(reinterpret_cast<const char*>(&a_v), 2); }
	void Put32(std::string& a_out, std::uint32_t a_v) { a_out.append(reinterpret_cast<const char*>(&a_v), 4); }

	std::string Wav(std::uint16_t a_channels, std::uint16_t a_tag = 1, std::uint32_t a_fmtSize = 16,
		std::uint32_t a_dataSize = 64, std::uint16_t a_blockAlign = 0xFFFF)
	{
		std::string fmt;
		Put16(fmt, a_tag);
		Put16(fmt, a_channels);
		Put32(fmt, 44100);
		Put32(fmt, 88200);
		Put16(fmt, a_blockAlign == 0xFFFF ? static_cast<std::uint16_t>(2 * a_channels) : a_blockAlign);
		Put16(fmt, 16);
		fmt.resize(a_fmtSize, '\0');
		std::string body = "WAVEfmt ";
		Put32(body, a_fmtSize);
		body += fmt + "data";
		Put32(body, a_dataSize);
		body.append(a_dataSize, '\0');
		std::string riff = "RIFF";
		Put32(riff, static_cast<std::uint32_t>(body.size()));
		return riff + body;
	}

	std::string Why(const std::string& a_file)
	{
		return AudioHeader::WhyUnplayable(
			[&](std::uint64_t a_offset, void* a_out, std::size_t a_size) {
				if (a_offset > a_file.size() || a_size > a_file.size() - a_offset) {
					return false;
				}
				std::memcpy(a_out, a_file.data() + a_offset, a_size);
				return true;
			},
			a_file.size());
	}

	bool Says(const std::string& a_file, const char* a_text) { return Why(a_file).find(a_text) != std::string::npos; }
}

int main()
{
	CHECK(Why(Wav(1)).empty());
	CHECK(Why(Wav(2)).empty());
	CHECK(Why(Wav(2, 0xFFFE, 0x28)).empty());

	// crash without AudioFormat
	CHECK(Says(Wav(0), "0 channels"));
	CHECK(Says(Wav(0, 0xFFFE, 0x28), "0 channels"));
	CHECK(Says(Wav(2, 1, 16, 64, 0), "0 bytes per sample frame"));

	// not read: the async open went on with an unset format
	CHECK(Says(Wav(2, 3), "32-bit float"));
	CHECK(Says(Wav(2, 2), "ADPCM"));
	CHECK(Says(Wav(2, 1, 14), "fmt chunk of 14 bytes"));
	CHECK(Says(Wav(2, 0xFFFE, 0x16), "fmt chunk of 22 bytes"));
	CHECK(Says(Wav(2, 1, 16, 0), "only 44 bytes"));  // the 47-byte minimum comes first
	std::string noData = Wav(2);
	noData.replace(0x24, 4, "junk");
	CHECK(Says(noData, "no sound data"));
	CHECK(Says("", "only 0 bytes"));
	CHECK(Says(std::string("ID3") + std::string(100, '\0'), "not a WAVE or xWMA file"));
	CHECK(Says(std::string("OggS") + std::string(100, '\0'), "not a WAVE or xWMA file"));
	CHECK(Says(Wav(2).substr(0, 0x20), "only 32 bytes"));

	// .fuz: the sound after the lip data
	std::string fuz = "FUZE";
	Put32(fuz, 1);
	Put32(fuz, 4);
	fuz += "LIPx";
	CHECK(Why(fuz + Wav(1)).empty());
	CHECK(Says(fuz + Wav(0), "0 channels"));
	CHECK(Says(fuz.substr(0, 6), "cut off"));

	// xWMA needs its packet table (one entry per block-align bytes of data) and a bit count
	std::string xwma = Wav(2, 0x161, 18);  // 64 bytes of data, block align 4
	xwma.replace(8, 4, "XWMA");
	CHECK(Says(xwma, "packet table"));
	std::string packets = "dpds";
	Put32(packets, 16 * 4);
	packets.append(16 * 4, '\0');
	CHECK(Why(xwma + packets).empty());
	std::string noBits = xwma + packets;
	noBits[0x22] = noBits[0x23] = '\0';  // wBitsPerSample
	CHECK(Says(noBits, "0 bits per sample"));
	std::string fewPackets = "dpds";
	Put32(fewPackets, 4);
	fewPackets.append(4, '\0');
	CHECK(Says(xwma + fewPackets, "does not match"));

	if (g_failures == 0) {
		std::printf("audio header OK\n");
	}
	return g_failures == 0 ? 0 : 1;
}
