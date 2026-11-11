#pragma once

// How the game reads a sound file's header (BSXAudio2DataSrc's parser, 1.6.1170 id 68066), then AudioFormat's checks
// and the open step's xWMA check: AudioFormatScan and its test see a file the way the game does.

#include <algorithm>
#include <cstdint>
#include <format>
#include <functional>
#include <string>

namespace AudioHeader
{
	// a_size bytes at a_offset, or false when the file ends before them
	using Read = std::function<bool(std::uint64_t a_offset, void* a_out, std::size_t a_size)>;

	inline std::uint32_t Swap32(std::uint32_t a_v)
	{
		return (a_v >> 24) | ((a_v >> 8) & 0xFF00) | ((a_v << 8) & 0xFF0000) | (a_v << 24);
	}

	inline std::uint16_t Swap16(std::uint16_t a_v) { return static_cast<std::uint16_t>((a_v >> 8) | (a_v << 8)); }

	constexpr std::uint32_t Tag(const char (&a_text)[5])
	{
		return static_cast<std::uint8_t>(a_text[0]) | static_cast<std::uint8_t>(a_text[1]) << 8 |
		       static_cast<std::uint8_t>(a_text[2]) << 16 | static_cast<std::uint32_t>(static_cast<std::uint8_t>(a_text[3])) << 24;
	}

	inline std::string FormatName(std::uint16_t a_tag)
	{
		switch (a_tag) {
		case 2:
			return "ADPCM";
		case 3:
			return "32-bit float";
		case 0x11:
			return "IMA ADPCM";
		case 0x55:
			return "MP3";
		default:
			return std::format("format {:#x}", a_tag);
		}
	}

	// The fmt chunk sizes the parser takes for each format. It takes no other format.
	inline bool Accepted(std::uint16_t a_tag, std::uint32_t a_size)
	{
		switch (a_tag) {
		case 1:
			return a_size >= 0x10;
		case 0x161:
			return a_size != 0x28;
		case 0x166:
			return a_size == 0x34;
		case 0xFFFE:
			return a_size >= 0x28;
		default:
			return false;
		}
	}

	// Empty when the game plays the file. Otherwise why it doesn't, or why it would crash without AudioFormat.
	inline std::string WhyUnplayable(const Read& a_read, std::uint64_t a_size)
	{
		std::uint64_t soundStart = 0;  // a .fuz holds lip data first
		std::uint32_t magic = 0;
		if (!a_read(0, &magic, 4)) {
			return std::format("the file is only {} bytes", a_size);
		}
		if (magic == Tag("FUZE") || magic == Tag("EZUF")) {
			std::uint32_t lip = 0;
			if (!a_read(8, &lip, 4)) {
				return "its .fuz header is cut off";
			}
			soundStart = 12ull + lip;
		}
		if (a_size < soundStart || a_size - soundStart < 0x2F) {
			return std::format("only {} bytes of sound data", a_size < soundStart ? 0 : a_size - soundStart);
		}
		const auto soundSize = a_size - soundStart;

		std::uint32_t riff[3];
		a_read(soundStart, riff, sizeof(riff));
		const bool bigEndian = riff[0] == Tag("FFIR");  // an Xbox 360 file
		const auto u32 = [&](std::uint32_t a_v) { return bigEndian ? Swap32(a_v) : a_v; };
		const auto u16 = [&](std::uint16_t a_v) { return bigEndian ? Swap16(a_v) : a_v; };
		if (u32(riff[2]) != Tag("WAVE") && u32(riff[2]) != Tag("XWMA")) {
			return "it is not a WAVE or xWMA file (the header names no such form)";
		}

		// the parser walks the chunks to the end; the last fmt / data / dpds chunk counts
		bool          fmtSeen = false;
		std::uint16_t tag = 0, channels = 0, blockAlign = 0, bits = 0;
		std::uint32_t dataSize = 0, packets = 0;
		for (std::uint64_t chunkAt = 0xC; chunkAt + 8 < soundSize;) {
			std::uint32_t chunk[2];
			a_read(soundStart + chunkAt, chunk, sizeof(chunk));
			const auto id = u32(chunk[0]);
			const auto length = u32(chunk[1]);
			const auto body = chunkAt + 8;
			if (id == Tag("fmt ")) {
				std::uint16_t fields[8]{};  // tag, channels, rate (2), bytes per second (2), block align, bits
				a_read(soundStart + body, fields, std::min<std::uint64_t>({ length, sizeof(fields), soundSize - body }));
				tag = u16(fields[0]);
				if (length < 0xE || !Accepted(tag, length)) {
					return std::format("the game does not play {} sound (fmt chunk of {} bytes)", FormatName(tag), length);
				}
				channels = u16(fields[1]);
				blockAlign = u16(fields[6]);
				bits = u16(fields[7]);
				fmtSeen = true;
			} else if (id == Tag("data")) {
				if (!fmtSeen) {
					return "its data chunk comes before its fmt chunk";
				}
				dataSize = length;
			} else if (id == Tag("dpds")) {
				packets = length >> 2;
			}
			chunkAt = body + length;
		}

		if (!fmtSeen) {
			return "it has no fmt chunk";
		}
		if (dataSize == 0) {
			return "it has no sound data (no data chunk, or an empty one)";
		}
		if (tag == 0x161 && packets == 0) {
			return "an xWMA file without its packet table (dpds chunk)";
		}
		if (channels == 0) {
			return "its file header says 0 channels";
		}
		if (tag == 1 && blockAlign == 0) {
			return "its file header says 0 bytes per sample frame";
		}
		if (tag == 0x161 && bits == 0) {
			return "its file header says 0 bits per sample";
		}
		if (tag == 0x161 && (blockAlign == 0 || dataSize / blockAlign != packets)) {
			return "its xWMA packet table does not match its data";
		}
		return {};
	}
}
