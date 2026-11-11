// Not a fix: a report for AudioFormat. When AudioFormat keeps a sound silent, the game knows only the sound's id, not
// its file. This reads every sound file in Data\Sound and Data\Music, loose and in Data's .bsa archives, checks each
// header the way the game reads it (audio_header.h) and logs every file the game won't play, with the id AudioFormat
// logs for it. It runs once on its own thread after the game starts and changes nothing.

#include "audio_header.h"
#include "fix.h"
#include "settings.h"

#include <fstream>
#include <thread>

namespace AudioFormatScan
{
	namespace
	{
		namespace fs = std::filesystem;

		struct Counts
		{
			std::uint64_t loose = 0;
			std::uint64_t archived = 0;
			std::uint64_t unplayable = 0;
			std::uint64_t compressed = 0;  // in a compressed archive: not read
		};

		bool IsSound(const std::string& a_name)
		{
			if (a_name.size() < 4) {
				return false;
			}
			const auto* ext = a_name.c_str() + a_name.size() - 4;
			return _stricmp(ext, ".wav") == 0 || _stricmp(ext, ".xwm") == 0 || _stricmp(ext, ".fuz") == 0;
		}

		std::string Utf8(const fs::path& a_path)
		{
			const auto text = a_path.u8string();
			return { reinterpret_cast<const char*>(text.data()), text.size() };
		}

		// The game hashes a sound's path relative to Data, like AudioFormat's log line shows it
		std::string SoundId(const std::string& a_path)
		{
			RE::BSResource::ID id;
			id.GenerateFromPath(a_path.c_str());
			return std::format("{:08X}/{:08X}", id.dir, id.file);
		}

		void Check(Counts& a_counts, const std::string& a_path, std::string_view a_where, const AudioHeader::Read& a_read,
			std::uint64_t a_size)
		{
			const auto why = AudioHeader::WhyUnplayable(a_read, a_size);
			if (!why.empty()) {
				++a_counts.unplayable;
				logger::warn("[AudioFormatScan] \"{}\" ({}): {}; sound id {}"sv, a_path, a_where, why, SoundId(a_path));
			}
		}

		// reads the sound that starts at a_start in the file
		AudioHeader::Read ReaderAt(std::ifstream& a_file, std::uint64_t a_start)
		{
			return [&a_file, a_start](std::uint64_t a_offset, void* a_out, std::size_t a_size) {
				a_file.clear();
				a_file.seekg(static_cast<std::streamoff>(a_start + a_offset));
				return a_file.read(static_cast<char*>(a_out), static_cast<std::streamsize>(a_size)).good();
			};
		}

		void ScanLoose(Counts& a_counts, const fs::path& a_data)
		{
			for (const auto* folder : { L"Sound", L"Music" }) {
				std::error_code ec;
				for (fs::recursive_directory_iterator it(a_data / folder, fs::directory_options::skip_permission_denied, ec),
					 end;
					 !ec && it != end; it.increment(ec)) {
					if (!it->is_regular_file(ec)) {
						continue;
					}
					const auto path = Utf8(it->path().lexically_relative(a_data));
					if (!IsSound(path)) {
						continue;
					}
					++a_counts.loose;
					std::ifstream file(it->path(), std::ios::binary);
					const auto    size = it->file_size(ec);
					Check(a_counts, path, "loose", ReaderAt(file, 0), ec ? 0 : size);
				}
			}
		}

		template <class T>
		bool ReadValue(std::ifstream& a_file, T& a_out)
		{
			return a_file.read(reinterpret_cast<char*>(&a_out), sizeof(T)).good();
		}

		// An SSE archive (version 104 or 105) lists its folders, then each folder's name and files, then the file names.
		// A sound is only read when it is stored uncompressed.
		void ScanArchive(Counts& a_counts, const fs::path& a_archive)
		{
			constexpr std::uint32_t kFolderNames = 0x1, kFileNames = 0x2, kCompressed = 0x4, kNamesInData = 0x100;
			constexpr std::uint32_t kOtherCompression = 0x40000000, kSizeBits = 0x3FFFFFFF;

			std::ifstream file(a_archive, std::ios::binary);
			std::uint32_t header[9];
			if (!file.read(reinterpret_cast<char*>(header), sizeof(header)) || header[0] != 0x00415342) {  // "BSA\0"
				return;
			}
			const auto version = header[1], flags = header[3], folderCount = header[4];
			const auto name = Utf8(a_archive.filename());
			if (version != 104 && version != 105) {
				return;
			}
			if ((flags & (kFolderNames | kFileNames)) != (kFolderNames | kFileNames)) {
				logger::info("[AudioFormatScan] {}: skipped, the archive stores no file names"sv, name);
				return;
			}

			std::vector<std::uint32_t> filesPerFolder(folderCount);
			for (auto& count : filesPerFolder) {
				std::uint64_t hash = 0, offset = 0;
				std::uint32_t pad = 0, offset32 = 0;
				if (!ReadValue(file, hash) || !ReadValue(file, count)) {
					return;
				}
				// SE's version 105 pads the count and widens the offset to 64 bits
				if (!(version == 105 ? ReadValue(file, pad) && ReadValue(file, offset) : ReadValue(file, offset32))) {
					return;
				}
			}

			struct Entry
			{
				std::string   folder;
				std::uint32_t size;
				std::uint32_t offset;
			};
			std::vector<Entry> entries;
			for (const auto count : filesPerFolder) {
				std::uint8_t length = 0;
				if (!ReadValue(file, length)) {
					return;
				}
				std::string folder(length, '\0');
				if (!file.read(folder.data(), length)) {
					return;
				}
				folder.resize(std::strlen(folder.c_str()));
				for (std::uint32_t i = 0; i < count; ++i) {
					std::uint64_t hash = 0;
					std::uint32_t size = 0, offset = 0;
					if (!ReadValue(file, hash) || !ReadValue(file, size) || !ReadValue(file, offset)) {
						return;
					}
					entries.push_back({ folder, size, offset });
				}
			}

			// the file names come in the same order as the entries
			for (auto& entry : entries) {
				std::string fileName;
				if (!std::getline(file, fileName, '\0')) {
					return;
				}
				if (!IsSound(fileName)) {
					continue;
				}
				++a_counts.archived;
				if (static_cast<bool>(flags & kCompressed) != static_cast<bool>(entry.size & kOtherCompression)) {
					++a_counts.compressed;
					continue;
				}
				std::uint64_t start = entry.offset;
				std::uint64_t size = entry.size & kSizeBits;
				if (flags & kNamesInData) {  // the sound's data starts with its path
					std::uint8_t length = 0;
					file.clear();
					file.seekg(static_cast<std::streamoff>(start));
					if (!ReadValue(file, length)) {
						continue;
					}
					start += 1ull + length;
					size -= std::min<std::uint64_t>(size, 1ull + length);
				}
				const auto nextName = file.tellg();
				Check(a_counts, entry.folder + '\\' + fileName, name, ReaderAt(file, start), size);
				file.clear();
				file.seekg(nextName);
			}
		}

		void Scan(fs::path a_data)
		{
			const auto started = std::chrono::steady_clock::now();
			Counts     counts;
			ScanLoose(counts, a_data);
			std::error_code ec;
			for (fs::directory_iterator it(a_data, ec), end; !ec && it != end; it.increment(ec)) {
				if (_wcsicmp(it->path().extension().c_str(), L".bsa") == 0) {
					ScanArchive(counts, it->path());
				}
			}
			const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);
			logger::info(
				"[AudioFormatScan] done in {} ms: {} loose and {} archived sound files, {} the game won't play{}"sv,
				ms.count(), counts.loose, counts.archived, counts.unplayable,
				counts.compressed ? std::format(", {} in compressed archives not read", counts.compressed) : "");
		}
	}

	Result Install()
	{
		std::wstring exe(32768, L'\0');
		exe.resize(GetModuleFileNameW(nullptr, exe.data(), static_cast<DWORD>(exe.size())));
		const auto data = Settings::PluginsFolder(exe).parent_path().parent_path();
		if (data.empty()) {
			return { Outcome::kFailed, "could not find the game's Data folder" };
		}
		std::thread(Scan, data).detach();
		return { Outcome::kActive, "reading Data\\Sound, Data\\Music and the .bsa archives in the background" };
	}

	const bool kRegistered = Register({
		.key = "AudioFormatScan",
		.install = Install,
		.section = "Reports",
	});
}
