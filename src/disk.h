#pragma once

// lib8bit by Zac Walker
//
// C64 disk image (.d64/.d71/.d81) parsing interface: image formats, directory
// entries and PRG extraction from caller-provided image bytes.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

enum class disk_image_format
{
	unknown,
	d64,
	d71,
	d81,
};

struct disk_directory_entry
{
	std::string filename;
	std::string type;
	uint16_t blocks = 0;
	bool closed = false;
	bool locked = false;
};

struct disk_directory
{
	disk_image_format format = disk_image_format::unknown;
	std::string name;
	std::string id;
	uint16_t free_blocks = 0;
	std::vector<disk_directory_entry> entries;
};

const char* disk_format_name(disk_image_format format);

// All of these take a caller-owned image buffer and require `size` readable
// bytes at `data`. The image format is identified from `size` alone, so only
// exactly-sized images are accepted and every read stays inside the buffer.
bool read_disk_directory(const uint8_t* data, size_t size, disk_directory& result);

// Extract a file from a disk image by following its sector chain. The returned
// bytes are in PRG form (2-byte little-endian load address followed by the
// program data), ready to hand to machine::load_prg. When filename is empty or
// "*" the first program on the disk is used (matching C64 LOAD"*",8); otherwise
// CBM DOS pattern matching applies ('*' matches the rest of the name, '?' any
// single character). Returns false if no file matches.
bool read_disk_file(const uint8_t* data, size_t size, std::string_view filename,
	std::vector<uint8_t>& result);

// Convert `length` PETSCII bytes to printable ASCII, stopping at a NUL or the
// $A0 shifted-space padding CBM DOS writes, and trimming trailing spaces.
std::string petscii_to_ascii(const uint8_t* data, size_t length);
