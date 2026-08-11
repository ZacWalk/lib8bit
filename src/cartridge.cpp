// lib8bit by Zac Walker
//
// C64 .CRT cartridge support: parses the CRT header + CHIP packets, tracks the
// ROML/ROMH banks and implements per-type bank switching through the $DE00-$DFFF
// I/O window.

#include "cartridge.h"

#include <cstring>

namespace
{
	uint16_t be16(const uint8_t* p)
	{
		return static_cast<uint16_t>((p[0] << 8) | p[1]);
	}

	uint32_t be32(const uint8_t* p)
	{
		return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
			(static_cast<uint32_t>(p[2]) << 8) | p[3];
	}

	// Sanity cap so a malformed .crt cannot drive unbounded allocation.
	constexpr size_t max_banks = 512;
}

bool cartridge::load(const uint8_t* bytes, const size_t length)
{
	eject();
	if (!bytes || length < 0x40) return false;

	if (std::memcmp(bytes, "C64 CARTRIDGE   ", 16) != 0) return false;

	uint32_t header_length = be32(bytes + 0x10);
	if (header_length < 0x40 || header_length > length) return false;

	hw_type = be16(bytes + 0x16);
	exrom = bytes[0x18];
	game = bytes[0x19];
	header_exrom = exrom;
	header_game = game;

	cart_name.clear();
	for (int i = 0; i < 32; i++)
	{
		const uint8_t c = bytes[0x20 + i];
		if (c == 0) break;
		cart_name.push_back(static_cast<char>(c));
	}

	banks.clear();
	size_t offset = header_length; // <= length, and every accepted packet keeps it so
	while (length - offset >= 16)
	{
		if (banks.size() >= max_banks) break;
		if (std::memcmp(bytes + offset, "CHIP", 4) != 0) break;

		const uint32_t packet_length = be32(bytes + offset + 4);
		const uint16_t bank_number = be16(bytes + offset + 10);
		const uint16_t load_address = be16(bytes + offset + 12);
		const uint16_t rom_size = be16(bytes + offset + 14);

		if (rom_size == 0 || rom_size > 0x4000) break;
		// Compare by subtraction: offset + packet_length wraps where size_t is 32-bit.
		// Passing both tests gives 16 + rom_size <= packet_length <= length - offset,
		// so the payload is in bounds and offset always advances by at least 17.
		if (packet_length < 16u + rom_size || packet_length > length - offset) break;

		chip_bank bank;
		bank.bank_number = bank_number;
		bank.load_address = load_address;
		bank.size = rom_size;
		bank.data.assign(bytes + offset + 16, bytes + offset + 16 + rom_size);
		banks.push_back(std::move(bank));

		offset += packet_length;
	}

	if (banks.empty()) return false;

	enabled = true;
	current_bank = 0;
	ultimax_mode = (exrom == 1 && game == 0);

	if (hw_type == cart_type::EASYFLASH)
	{
		easyflash_ram.assign(256, 0);
		easyflash_control = 0;
	}

	update_bank_mapping();
	return true;
}

void cartridge::eject()
{
	cart_name.clear();
	hw_type = cart_type::NORMAL;
	banks.clear();
	current_bank = 0;
	roml_index = -1;
	romh_index = -1;
	enabled = false;
	exrom = 1;
	game = 1;
	ultimax_mode = false;
	easyflash_ram.clear();
	easyflash_control = 0;
}

void cartridge::reset()
{
	current_bank = 0;
	enabled = !banks.empty();
	exrom = header_exrom;
	game = header_game;
	ultimax_mode = (exrom == 1 && game == 0);

	if (hw_type == cart_type::EASYFLASH)
	{
		easyflash_control = 0;
		std::fill(easyflash_ram.begin(), easyflash_ram.end(), static_cast<uint8_t>(0));
	}

	update_bank_mapping();
}

void cartridge::update_bank_mapping()
{
	roml_index = -1;
	romh_index = -1;

	for (int i = 0; i < static_cast<int>(banks.size()); i++)
	{
		const chip_bank& bank = banks[i];
		if (bank.bank_number != current_bank) continue;

		if (bank.load_address == 0x8000)
		{
			roml_index = i;
			// A 16KB bank supplies ROMH as well (data at offset $2000).
			if (bank.size == 16384) romh_index = i;
		}
		else if (bank.load_address == 0xA000)
		{
			romh_index = i;
		}
		else if (bank.load_address == 0xE000 || bank.load_address == 0xF000)
		{
			romh_index = i;
		}
	}
}

void cartridge::get_mem_config(int& roml_addr, int& romh_addr) const
{
	if (exrom == 0 && game == 1) { roml_addr = 0x8000; romh_addr = -1; }       // 8K
	else if (exrom == 0 && game == 0) { roml_addr = 0x8000; romh_addr = 0xA000; } // 16K
	else if (exrom == 1 && game == 0) { roml_addr = 0x8000; romh_addr = 0xE000; } // Ultimax
	else { roml_addr = -1; romh_addr = -1; }                                     // off
}

bool cartridge::read(const uint16_t addr, uint8_t& out) const
{
	if (!enabled) return false;

	int roml_addr = -1, romh_addr = -1;
	get_mem_config(roml_addr, romh_addr);

	if (roml_index >= 0 && addr >= 0x8000 && addr <= 0x9FFF)
	{
		const chip_bank& b = banks[roml_index];
		const int off = addr - 0x8000;
		if (off < b.size) { out = b.data[off]; return true; }
	}

	if (romh_index >= 0)
	{
		const chip_bank& b = banks[romh_index];
		if (romh_addr == 0xA000 && addr >= 0xA000 && addr <= 0xBFFF)
		{
			if (b.load_address == 0x8000 && b.size == 16384)
			{
				const int off = 0x2000 + (addr - 0xA000);
				if (off < b.size) { out = b.data[off]; return true; }
			}
			else
			{
				const int off = addr - 0xA000;
				if (off < b.size) { out = b.data[off]; return true; }
			}
		}
		else if (romh_addr == 0xE000 && addr >= 0xE000 && addr <= 0xFFFF)
		{
			const int off = addr - 0xE000;
			if (off < b.size) { out = b.data[off]; return true; }
		}
	}

	return false;
}

bool cartridge::read_io(const uint16_t addr, uint8_t& out)
{
	if (!enabled) return false;

	switch (hw_type)
	{
	case cart_type::C64_GAME_SYSTEM:
	case cart_type::DINAMIC:
		if (addr >= 0xDE00 && addr <= 0xDEFF)
		{
			current_bank = addr & 0x3F;
			update_bank_mapping();
		}
		out = 0;
		return true;

	case cart_type::WARPSPEED:
		if (addr >= 0xDE00 && addr <= 0xDFFF && roml_index >= 0)
		{
			const chip_bank& b = banks[roml_index];
			const int off = 0x1E00 + (addr & 0x1FF);
			if (off < b.size) { out = b.data[off]; return true; }
		}
		break;

	case cart_type::ROSS:
		if (addr >= 0xDE00 && addr <= 0xDEFF)
		{
			current_bank = 1;
			update_bank_mapping();
		}
		else if (addr >= 0xDF00 && addr <= 0xDFFF)
		{
			enabled = false;
		}
		out = 0;
		return true;

	case cart_type::EASYFLASH:
		if (addr >= 0xDF00 && addr <= 0xDFFF && !easyflash_ram.empty())
		{
			out = easyflash_ram[addr & 0xFF];
			return true;
		}
		break;

	default:
		break;
	}

	return false;
}

void cartridge::write_io(const uint16_t addr, const uint8_t value)
{
	if (!enabled) return;

	switch (hw_type)
	{
	case cart_type::OCEAN_TYPE_1:
		if (addr == 0xDE00) { current_bank = value & 0x3F; update_bank_mapping(); }
		break;

	case cart_type::FUN_PLAY:
		if (addr == 0xDE00)
		{
			if (value == 0x86) enabled = false;
			else { current_bank = ((value >> 3) & 0x07) | ((value & 0x01) << 3); update_bank_mapping(); }
		}
		break;

	case cart_type::SUPER_GAMES:
		if (addr == 0xDF00)
		{
			current_bank = value & 0x03;
			if ((value & 0x0C) == 0x0C) enabled = false;
			update_bank_mapping();
		}
		break;

	case cart_type::C64_GAME_SYSTEM:
		if (addr >= 0xDE00 && addr <= 0xDEFF) { current_bank = addr & 0x3F; update_bank_mapping(); }
		break;

	case cart_type::DINAMIC:
		if (addr >= 0xDE00 && addr <= 0xDEFF) { current_bank = addr & 0x0F; update_bank_mapping(); }
		break;

	case cart_type::MAGIC_DESK:
		if (addr == 0xDE00)
		{
			if (value & 0x80) enabled = false;
			else { current_bank = value & 0x3F; update_bank_mapping(); }
		}
		break;

	case cart_type::FINAL_CARTRIDGE_III:
		if (addr == 0xDFFF) { current_bank = value & 0x03; update_bank_mapping(); }
		break;

	case cart_type::ACTION_REPLAY:
	case cart_type::ATOMIC_POWER:
		if (addr == 0xDE00) { current_bank = value & 0x03; update_bank_mapping(); }
		break;

	case cart_type::SIMONS_BASIC:
		if (addr == 0xDE00)
		{
			game = (value == 0x01) ? 0 : 1;
			update_bank_mapping();
		}
		break;

	case cart_type::COMAL_80:
		if (addr == 0xDE00) { current_bank = value & 0x03; update_bank_mapping(); }
		break;

	case cart_type::WARPSPEED:
		if (addr >= 0xDF00 && addr <= 0xDFFF) enabled = false;
		else if (addr >= 0xDE00 && addr <= 0xDEFF) enabled = true;
		break;

	case cart_type::ROSS:
		if (addr >= 0xDF00 && addr <= 0xDFFF) enabled = false;
		else if (addr >= 0xDE00 && addr <= 0xDEFF) { current_bank = 1; update_bank_mapping(); }
		break;

	case cart_type::EASYFLASH:
		if (addr == 0xDE00)
		{
			current_bank = value & 0x3F;
			update_bank_mapping();
		}
		else if (addr == 0xDE02)
		{
			easyflash_control = value;
			if (value & 0x04)
			{
				game = (value & 0x01) ? 0 : 1;
				exrom = (value & 0x02) ? 0 : 1;
				ultimax_mode = (exrom == 0 && game == 1);
			}
			else
			{
				game = 0;
				exrom = 0;
				ultimax_mode = false;
			}
			update_bank_mapping();
		}
		else if (addr >= 0xDF00 && addr <= 0xDFFF && !easyflash_ram.empty())
		{
			easyflash_ram[addr & 0xFF] = value;
		}
		break;

	default:
		break;
	}
}
