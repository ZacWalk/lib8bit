// lib8bit by Zac Walker
//
// C64 disk image (.d64/.d71/.d81) parsing: directory listing and PRG
// extraction from caller-provided image bytes.

#include "disk.h"

#include <array>
#include <utility>

namespace
{
	// Track/sector pairs are indexed with a stride of 256 because sector bytes
	// read from an image are unconstrained, and a smaller stride lets distinct
	// pairs alias in the cycle detectors.
	constexpr size_t visit_stride = 256;
	constexpr size_t max_tracks = 80;

	size_t visit_index(const int track, const int sector)
	{
		return static_cast<size_t>(track) * visit_stride + static_cast<size_t>(sector);
	}

	struct disk_geometry
	{
		disk_image_format format = disk_image_format::unknown;
		int tracks = 0;
		size_t data_size = 0;
		// Sectors preceding each 1-based track, so offsets are a table lookup.
		std::array<uint16_t, max_tracks + 1> sectors_before{};
	};

	int sectors_on_track(const disk_geometry& geometry, int track)
	{
		if (track < 1 || track > geometry.tracks) return 0;
		if (geometry.format == disk_image_format::d81) return 40;
		if (geometry.format == disk_image_format::d71 && track > 35) track -= 35;
		if (track <= 17) return 21;
		if (track <= 24) return 19;
		if (track <= 30) return 18;
		return 17;
	}

	disk_geometry detect_geometry(const size_t size)
	{
		constexpr std::array<std::pair<size_t, int>, 6> d64_sizes = {
			{
				{174848, 35}, {175531, 35},
				{196608, 40}, {197376, 40},
				{205312, 42}, {206114, 42},
			}
		};

		disk_geometry geometry;
		for (const auto [image_size, tracks] : d64_sizes)
			if (size == image_size)
			{
				geometry = {
					disk_image_format::d64, tracks,
					static_cast<size_t>(tracks == 35 ? 174848 : tracks == 40 ? 196608 : 205312)
				};
				break;
			}

		if (geometry.format == disk_image_format::unknown)
		{
			if (size == 349696 || size == 351062) geometry = {disk_image_format::d71, 70, 349696};
			else if (size == 819200 || size == 822400) geometry = {disk_image_format::d81, 80, 819200};
			else return geometry;
		}

		uint16_t preceding = 0;
		for (auto track = 1; track <= geometry.tracks; ++track)
		{
			geometry.sectors_before[track] = preceding;
			preceding = static_cast<uint16_t>(preceding + sectors_on_track(geometry, track));
		}
		return geometry;
	}

	bool sector_offset(const disk_geometry& geometry, const int track, const int sector, size_t& result)
	{
		const auto sector_count = sectors_on_track(geometry, track);
		if (sector < 0 || sector >= sector_count) return false;

		result = (static_cast<size_t>(geometry.sectors_before[track]) + sector) * 256;
		return result + 256 <= geometry.data_size;
	}

	const char* file_type_name(const uint8_t type)
	{
		constexpr std::array names = {"DEL", "SEQ", "PRG", "USR", "REL", "CBM", "DIR", "???"};
		return names[type & 0x07];
	}

	std::string to_upper_ascii(const std::string_view text)
	{
		std::string result(text);
		for (auto& c : result)
			if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
		return result;
	}

	// CBM DOS pattern matching: '?' matches any single character and '*' matches
	// the rest of the name, so "A*" and "A?C*" behave as a real drive would.
	bool name_matches(const std::string& pattern, const std::string& name)
	{
		size_t p = 0, n = 0;
		for (; p < pattern.size(); ++p, ++n)
		{
			if (pattern[p] == '*') return true;
			if (n >= name.size()) return false;
			if (pattern[p] != '?' && pattern[p] != name[n]) return false;
		}
		return n == name.size();
	}

	// Free sectors per track live in the BAM: one entry per track whose first
	// byte is the count. The directory track itself is never counted.
	uint16_t count_free_blocks(const disk_geometry& geometry, const uint8_t* data, const size_t header_offset)
	{
		uint16_t free_blocks = 0;
		if (geometry.format == disk_image_format::d81)
		{
			// D81 keeps two BAM sectors (tracks 1-40 and 41-80) at 40/1 and 40/2.
			for (auto half = 0; half < 2; ++half)
			{
				size_t bam_offset = 0;
				if (!sector_offset(geometry, 40, half + 1, bam_offset)) continue;
				for (auto track = 1; track <= 40; ++track)
					if (half * 40 + track != 40)
						free_blocks += data[bam_offset + 0x10 + (track - 1) * 6];
			}
			return free_blocks;
		}

		for (auto track = 1; track <= 35; ++track)
			if (track != 18) free_blocks += data[header_offset + 0x04 + (track - 1) * 4];

		if (geometry.format == disk_image_format::d71)
			for (auto track = 36; track <= 70; ++track)
				if (track != 53) free_blocks += data[header_offset + 0xDD + (track - 36)];

		return free_blocks;
	}

	// A directory entry with the location of its first data block.
	struct disk_file_ref
	{
		uint8_t type = 0;
		int track = 0;
		int sector = 0;
		std::string name;
	};
}

const char* disk_format_name(const disk_image_format format)
{
	switch (format)
	{
	case disk_image_format::d64: return "D64";
	case disk_image_format::d71: return "D71";
	case disk_image_format::d81: return "D81";
	default: return "unknown";
	}
}

std::string petscii_to_ascii(const uint8_t* data, const size_t length)
{
	std::string result;
	result.reserve(length);
	for (size_t index = 0; index < length; ++index)
	{
		auto value = data[index];
		if (value == 0 || value == 0xa0) break;
		if (value >= 0xc1 && value <= 0xda) value &= 0x7f;
		result.push_back(value >= 0x20 && value <= 0x7e ? static_cast<char>(value) : '?');
	}
	while (!result.empty() && result.back() == ' ') result.pop_back();
	return result;
}

bool read_disk_directory(const uint8_t* data, const size_t size, disk_directory& result)
{
	result = {};
	if (!data) return false;
	const auto geometry = detect_geometry(size);
	if (geometry.format == disk_image_format::unknown) return false;

	const auto header_track = geometry.format == disk_image_format::d81 ? 40 : 18;
	size_t header_offset = 0;
	if (!sector_offset(geometry, header_track, 0, header_offset)) return false;

	const auto name_offset = geometry.format == disk_image_format::d81 ? 0x04 : 0x90;
	const auto id_offset = geometry.format == disk_image_format::d81 ? 0x16 : 0xa2;
	result.format = geometry.format;
	result.name = petscii_to_ascii(data + header_offset + name_offset, 16);
	result.id = petscii_to_ascii(data + header_offset + id_offset, 2);
	result.free_blocks = count_free_blocks(geometry, data, header_offset);

	int directory_track = data[header_offset];
	int directory_sector = data[header_offset + 1];
	std::vector<bool> visited(static_cast<size_t>(geometry.tracks + 1) * visit_stride);

	while (directory_track != 0)
	{
		// A damaged chain keeps the entries already listed rather than losing the
		// whole directory, matching how read_disk_file recovers.
		const auto index = visit_index(directory_track, directory_sector);
		if (index >= visited.size() || visited[index]) break;
		visited[index] = true;

		size_t directory_offset = 0;
		if (!sector_offset(geometry, directory_track, directory_sector, directory_offset)) break;
		for (auto slot = 0; slot < 8; ++slot)
		{
			const auto* entry = data + directory_offset + slot * 32 + 2;
			const auto raw_type = entry[0];
			if ((raw_type & 0x07) == 0) continue;
			result.entries.push_back({
				petscii_to_ascii(entry + 3, 16),
				file_type_name(raw_type),
				static_cast<uint16_t>(entry[28] | entry[29] << 8),
				(raw_type & 0x80) != 0,
				(raw_type & 0x40) != 0,
			});
		}

		directory_track = data[directory_offset];
		directory_sector = data[directory_offset + 1];
	}

	return true;
}

bool read_disk_file(const uint8_t* data, const size_t size, const std::string_view filename,
	std::vector<uint8_t>& result)
{
	result.clear();
	if (!data) return false;
	const auto geometry = detect_geometry(size);
	if (geometry.format == disk_image_format::unknown) return false;

	const auto header_track = geometry.format == disk_image_format::d81 ? 40 : 18;
	size_t header_offset = 0;
	if (!sector_offset(geometry, header_track, 0, header_offset)) return false;

	// Walk the directory, collecting each file's first data-block location.
	std::vector<disk_file_ref> files;
	int directory_track = data[header_offset];
	int directory_sector = data[header_offset + 1];
	std::vector<bool> visited(static_cast<size_t>(geometry.tracks + 1) * visit_stride);

	while (directory_track != 0)
	{
		const auto index = visit_index(directory_track, directory_sector);
		if (index >= visited.size() || visited[index]) break;
		visited[index] = true;

		size_t directory_offset = 0;
		if (!sector_offset(geometry, directory_track, directory_sector, directory_offset)) break;
		for (auto slot = 0; slot < 8; ++slot)
		{
			const auto* entry = data + directory_offset + slot * 32 + 2;
			const auto raw_type = entry[0];
			if ((raw_type & 0x07) == 0) continue; // DEL / empty slot
			if (entry[1] == 0) continue;          // no first data block
			files.push_back({
				raw_type,
				entry[1],
				entry[2],
				to_upper_ascii(petscii_to_ascii(entry + 3, 16)),
			});
		}

		directory_track = data[directory_offset];
		directory_sector = data[directory_offset + 1];
	}

	if (files.empty()) return false;

	// Select the requested file. "*"/empty -> first PRG, else first file.
	const auto pattern = to_upper_ascii(filename);
	const disk_file_ref* chosen = nullptr;
	if (pattern.empty() || pattern == "*")
	{
		for (const auto& file : files)
			if ((file.type & 0x07) == 2) { chosen = &file; break; } // PRG
		if (!chosen) chosen = &files.front();
	}
	else
	{
		for (const auto& file : files)
			if (name_matches(pattern, file.name)) { chosen = &file; break; }
	}
	if (!chosen) return false;

	// Follow the file's sector chain.
	int track = chosen->track;
	int sector = chosen->sector;
	std::vector<bool> seen(static_cast<size_t>(geometry.tracks + 1) * visit_stride);
	while (track != 0)
	{
		const auto seen_index = visit_index(track, sector);
		if (seen_index >= seen.size() || seen[seen_index]) break;
		seen[seen_index] = true;

		size_t offset = 0;
		if (!sector_offset(geometry, track, sector, offset)) break;
		const int next_track = data[offset];
		const int next_sector = data[offset + 1];

		if (next_track == 0)
		{
			// Last block: next_sector is the index of the last used byte. A value of
			// 1 is a legitimate empty final block (the file ended on a block
			// boundary); only 0 is corrupt, and either way keep what we already have.
			for (int i = 2; i <= next_sector && offset + i < geometry.data_size; ++i)
				result.push_back(data[offset + i]);
			break;
		}

		for (int i = 2; i < 256 && offset + i < geometry.data_size; ++i)
			result.push_back(data[offset + i]);
		track = next_track;
		sector = next_sector;
	}

	return !result.empty();
}
