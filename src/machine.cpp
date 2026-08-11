// lib8bit by Zac Walker
//
// Core C64 machine implementation: reset, memory banking, the memory bus,
// CIA timers and keyboard, and the top-level exec/step orchestration that
// drives the CPU, VIC-II and SID each frame.
//
// http://forum.arduino.cc/index.php?topic=193216.msg1793065#msg1793065

#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>

#include "cpu.h"
#include "disk.h"
#include "machine.h"

namespace
{
	// PAL VIC-II raster timing.
	constexpr int32_t CYCLES_PER_RASTER_LINE = 63;
	constexpr int32_t RASTER_LINES_PER_FRAME = 312;

	// The .sid front end, defined with the rest of the player at the end of this
	// file: repaints the text screen and services its keys, once per exec().
	void sid_player_step(machine& m);
}

// ===========================================================================
// machine_state: reset, memory banking and the memory bus
// ===========================================================================

void machine_state::reset()
{
	std::memset(RAM, 0, RAM_SIZE);

	// Processor port: 0x2F direction, 0x07 data => BASIC + KERNAL + I/O all
	// visible at power-on, exactly as the C64 comes up before the kernal runs.
	RAM[0x0000] = 0x2F;
	RAM[0x0001] = 0x07;

	std::memset(read_map, 0, sizeof(read_map));
	std::memset(write_map, 0, sizeof(write_map));
	memory_map_key = 0xFFFFFFFF;
	update_memory_map();

	irq_pending = false;
	nmi_pending = false;
	ext_irq = false;

	instruction_count = 0;
	clock_ticks = 0;

	cia1 = cia_chip{};
	cia2 = cia_chip{};
	vic = vic_chip{};
	sid.reset();
	sid.audio_cycle = 0;
	std::memset(sid_regs, 0, sizeof(sid_regs));
	sid_player = sid_player_state{}; // any reset leaves the tune front end

	// Reset the cartridge before reading the reset vector so an Ultimax cart can
	// supply $FFFC/$FFFD.
	cart.reset();

	joystick1 = 0xFF;
	joystick2 = 0xFF;
	for (auto& row : key_matrix) row = 0xFF;

	// The 6502 loads PC from the reset vector and comes up with interrupts
	// masked; the kernal reset routine sets everything else up from there.
	cpu.pc = static_cast<uint16_t>(ram_read(0xFFFC)) | (static_cast<uint16_t>(ram_read(0xFFFD)) << 8);
	cpu.a = 0;
	cpu.x = 0;
	cpu.y = 0;
	cpu.sp = 0xFD;
	cpu.status = FLAG_CONSTANT | FLAG_INTERRUPT;
}

void machine_state::update_memory_map()
{
	// Model the processor port the same way ram_read does: bits configured as
	// inputs (or not connected) float high rather than reading the stale latch.
	const uint8_t ddr = RAM[0];
	const uint8_t bank = static_cast<uint8_t>((RAM[1] & ddr) | (0x17 & ~ddr));

	const uint32_t key = static_cast<uint32_t>(bank & 0x07)
		| (cart.enabled ? 0x08u : 0u)
		| (cart.exrom != 0 ? 0x10u : 0u)
		| (cart.game != 0 ? 0x20u : 0u)
		| (cart.has_roml() ? 0x40u : 0u)
		| (cart.has_romh() ? 0x80u : 0u);
	if (key == memory_map_key) return;
	memory_map_key = key;

	// Bit 0 (LORAM): BASIC ROM at $A000-$BFFF
	// Bit 1 (HIRAM): KERNAL ROM at $E000-$FFFF
	// Bit 2 (CHAREN): 0 = Char ROM at $D000-$DFFF, 1 = I/O
	is_basic_on = (bank & 3) == 3;
	is_kernal_on = (bank & 2) != 0;
	is_io_on = (bank & 4) != 0 && (bank & 3) != 0;
	is_char_on = (bank & 4) == 0 && (bank & 3) != 0;

	std::memset(read_map, 0, sizeof(read_map));
	if (is_basic_on) std::memset(read_map + 0xA0, 1, 0x20);  // $A000-$BFFF
	if (is_kernal_on) std::memset(read_map + 0xE0, 2, 0x20); // $E000-$FFFF

	if (is_io_on)
	{
		std::memset(read_map + 0xD0, 4, 0x10);  // I/O reads
		std::memset(write_map + 0xD0, 1, 0x10); // I/O writes
	}
	else
	{
		std::memset(write_map + 0xD0, 0, 0x10); // RAM writes under $D000-$DFFF
		if (is_char_on) std::memset(read_map + 0xD0, 3, 0x10); // Char ROM reads
	}

	// Cartridge ROML/ROMH overrides. cart.exrom/game are active-low lines
	// (0 = asserted). The PLA also needs the processor port to be asking for ROM:
	// a cartridge that writes $34 to $01 reaches the RAM under its own ROM.
	if (cart.enabled)
	{
		const bool exrom_low = cart.exrom == 0;
		const bool game_low = cart.game == 0;
		const bool loram = (bank & 1) != 0;
		const bool hiram = (bank & 2) != 0;

		if (game_low && !exrom_low)
		{
			// Ultimax: the cartridge owns the map. Only $0000-$0FFF is RAM, I/O is
			// always visible, and there is no BASIC, KERNAL or character ROM.
			is_basic_on = false;
			is_kernal_on = false;
			is_char_on = false;
			is_io_on = true;
			std::memset(read_map + 0x10, 0, 0xF0);
			std::memset(read_map + 0xD0, 4, 0x10);
			std::memset(write_map + 0xD0, 1, 0x10);
			if (cart.has_roml()) std::memset(read_map + 0x80, 5, 0x20); // $8000-$9FFF
			if (cart.has_romh()) std::memset(read_map + 0xE0, 6, 0x20); // $E000-$FFFF
		}
		else if (exrom_low)
		{
			if (cart.has_roml() && loram && hiram)
				std::memset(read_map + 0x80, 5, 0x20); // $8000-$9FFF ROML
			if (cart.has_romh() && game_low && hiram)
			{
				is_basic_on = false;                   // ROMH replaces BASIC
				std::memset(read_map + 0xA0, 6, 0x20); // 16K: ROMH at $A000-$BFFF
			}
		}
	}
}

uint8_t machine_state::ram_read(const uint16_t address)
{
	if (raw_ram) return RAM[address]; // bare 6502 + flat RAM (CPU test vectors)

	const uint8_t type = read_map[address >> 8];

	if (type == 0) // RAM
	{
		if (address == 1)
		{
			// Reading the processor port returns latch bits for outputs and the
			// (pulled-up) pin level 0x17 for inputs.
			const uint8_t ddr = RAM[0];
			return static_cast<uint8_t>((RAM[1] & ddr) | (0x17 & ~ddr));
		}
		return RAM[address];
	}

	if (type == 1) return rom_basic[address & 0x1FFF];
	if (type == 2) return rom_kernal[address & 0x1FFF];
	if (type == 3) return rom_chars[address & 0x0FFF];

	if (type == 4) // I/O
	{
		if (address < 0xD400) return read_vic(address);
		if (address < 0xD800) return sid_read(address & 0x1F);
		if (address < 0xDC00) return RAM[address] & 0x0F; // Colour RAM (4-bit)
		if (address < 0xDD00) return read_cia1(address);
		if (address < 0xDE00) return read_cia2(address);
		// $DE00-$DFFF I/O expansion: some cartridges bank-switch on read.
		if (cart.enabled)
		{
			uint8_t out = 0;
			const bool served = cart.read_io(address, out);
			update_memory_map();
			if (served) return out;
		}
		return RAM[address];
	}

	if (type == 5 || type == 6) // cartridge ROML / ROMH
	{
		uint8_t out = 0;
		if (cart.enabled && cart.read(address, out)) return out;
		if (type == 6 && is_basic_on && address >= 0xA000 && address < 0xC000)
			return rom_basic[address & 0x1FFF];
		if (type == 6 && is_kernal_on && address >= 0xE000)
			return rom_kernal[address & 0x1FFF];
		return RAM[address];
	}

	return RAM[address];
}

void machine_state::ram_write(const uint16_t address, const uint8_t value)
{
	if (raw_ram) { RAM[address] = value; return; } // bare 6502 + flat RAM

	if (write_map[address >> 8] == 0) // RAM
	{
		if (address > 1)
		{
			RAM[address] = value;
			return;
		}
		if (address == 0)
		{
			RAM[0] = value;
			update_memory_map();
			return;
		}
		// address == 1: store the full latch and re-evaluate banking.
		RAM[1] = value;
		update_memory_map();
		return;
	}

	// I/O write (write_map == 1). If the CPU banked I/O out mid-access fall back
	// to RAM underneath.
	if (!is_io_on)
	{
		RAM[address] = value;
		return;
	}

	if (address < 0xD400) { write_vic(address, value); return; }
	if (address < 0xD800) { sid_write(address & 0x1F, value); return; }
	if (address < 0xDC00) { RAM[address] = value & 0x0F; return; } // Colour RAM
	if (address < 0xDD00) { write_cia1(address, value); return; }
	if (address < 0xDE00) { write_cia2(address, value); return; }
	// $DE00-$DFFF I/O expansion: cartridge bank switching.
	if (cart.enabled)
	{
		cart.write_io(address, value);
		update_memory_map();
	}
	RAM[address] = value;
}

// ===========================================================================
// VIC-II registers (raster + raster interrupt)
// ===========================================================================

uint8_t machine_state::read_vic(const uint16_t address)
{
	// The VIC-II has 47 registers at $D000-$D02E and mirrors its 64-byte block
	// every 64 bytes through $D3FF, so normalise the address to $D000-$D03F.
	const uint16_t reg = static_cast<uint16_t>(0xD000 | (address & 0x3F));
	const int32_t raster_line = vic.raster_line;

	switch (reg)
	{
	case 0xD012: // raster counter low 8 bits
		return static_cast<uint8_t>(raster_line & 0xFF);
	case 0xD011: // control register 1: bit 7 is raster bit 8
		return static_cast<uint8_t>((RAM[0xD011] & 0x7F) | ((raster_line & 0x100) >> 1));
	case 0xD016: // control register 2: bits 6-7 are unused (read as 1)
		return static_cast<uint8_t>(RAM[0xD016] | 0xC0);
	case 0xD018: // memory pointers: bit 0 is unused (read as 1)
		return static_cast<uint8_t>(RAM[0xD018] | 0x01);
	case 0xD019: // interrupt status: bits 4-6 are unused (read as 1)
		return static_cast<uint8_t>(vic.irq_status | 0x70);
	case 0xD01A: // interrupt enable: bits 4-7 are unused (read as 1)
		return static_cast<uint8_t>(vic.irq_enable | 0xF0);
	case 0xD01E: // sprite-sprite collision (cleared on read)
	{
		const uint8_t v = RAM[0xD01E];
		RAM[0xD01E] = 0;
		return v;
	}
	case 0xD01F: // sprite-background collision (cleared on read)
	{
		const uint8_t v = RAM[0xD01F];
		RAM[0xD01F] = 0;
		return v;
	}
	default:
		// Colour registers $D020-$D02E only use the low nibble; the high nibble
		// reads back as 1. Registers $D02F-$D03F do not exist and read as $FF.
		if (reg >= 0xD020 && reg <= 0xD02E) return static_cast<uint8_t>(RAM[reg] | 0xF0);
		if (reg >= 0xD02F) return 0xFF;
		return RAM[reg];
	}
}

void machine_state::write_vic(const uint16_t address, const uint8_t value)
{
	const uint16_t reg = static_cast<uint16_t>(0xD000 | (address & 0x3F));
	switch (reg)
	{
	case 0xD011:
		RAM[reg] = value;
		vic.raster_compare = static_cast<uint16_t>((RAM[0xD012] & 0xFF) | ((value & 0x80) << 1));
		return;
	case 0xD012:
		RAM[reg] = value;
		vic.raster_compare = static_cast<uint16_t>(((RAM[0xD011] & 0x80) << 1) | value);
		return;
	case 0xD019:
		// Acknowledge interrupts: writing 1s clears the matching status bits.
		vic.irq_status &= ~(value & 0x0F);
		update_vic_irq();
		return;
	case 0xD01A:
		vic.irq_enable = value & 0x0F;
		RAM[reg] = value;
		update_vic_irq();
		return;
	case 0xD01E: // collision registers are read-only
	case 0xD01F:
		return;
	default:
		RAM[reg] = value;
		return;
	}
}

// ===========================================================================
// CIA1 (keyboard / joystick / system IRQ timer)
// ===========================================================================

uint8_t machine_state::read_cia1(const uint16_t address)
{
	// The CIA has 16 registers mirrored every 16 bytes through its $DC00-$DCFF page.
	const uint16_t reg = static_cast<uint16_t>(0xDC00 | (address & 0x0F));

	switch (reg & 0x0F)
	{
	case 0x00: // Port A - joystick 2 + keyboard rows when columns are driven
	{
		// Pins configured as inputs float high; only output bits read back the latch.
		const uint8_t ddra = RAM[0xDC02];
		uint8_t result = static_cast<uint8_t>(((RAM[0xDC00] & ddra) | ~ddra) & joystick2);
		// Reverse scan: a game driving columns low on Port B (DDRB output) and
		// reading Port A sees a row bit pulled low where a key is pressed.
		const uint8_t ddrb = RAM[0xDC03];
		const uint8_t col_out = RAM[0xDC01];
		for (int c = 0; c < 8; ++c)
		{
			if ((ddrb & (1u << c)) && (col_out & (1u << c)) == 0)
				for (int r = 0; r < 8; ++r)
					if ((key_matrix[r] & (1u << c)) == 0) result &= ~(1u << r);
		}
		return result;
	}
	case 0x01: // Port B - joystick 1 + keyboard columns for the selected rows
	{
		const uint8_t ddrb = RAM[0xDC03];
		uint8_t result = static_cast<uint8_t>(((RAM[0xDC01] & ddrb) | ~ddrb) & joystick1);
		// Rows are the Port A outputs (DDRA bit set, latch bit low = row selected).
		const uint8_t ddra = RAM[0xDC02];
		const uint8_t row_out = RAM[0xDC00];
		for (int r = 0; r < 8; ++r)
			if ((ddra & (1u << r)) && (row_out & (1u << r)) == 0)
				result &= key_matrix[r];
		return result;
	}
	case 0x04: return static_cast<uint8_t>(cia1.timer_a_counter & 0xFF);
	case 0x05: return static_cast<uint8_t>((cia1.timer_a_counter >> 8) & 0xFF);
	case 0x06: return static_cast<uint8_t>(cia1.timer_b_counter & 0xFF);
	case 0x07: return static_cast<uint8_t>((cia1.timer_b_counter >> 8) & 0xFF);
	case 0x0D: // ICR - reading returns and clears the flags
	{
		const uint8_t v = cia1.icr_data;
		cia1.icr_data = 0;
		update_irq_line();
		return v;
	}
	case 0x0E: return cia1.cra;
	case 0x0F: return cia1.crb;
	default: return RAM[reg];
	}
}

void machine_state::write_cia1(const uint16_t address, const uint8_t value)
{
	const uint16_t reg = static_cast<uint16_t>(0xDC00 | (address & 0x0F));

	switch (reg & 0x0F)
	{
	case 0x04:
		cia1.timer_a_latch = static_cast<uint16_t>((cia1.timer_a_latch & 0xFF00) | value);
		break;
	case 0x05:
		cia1.timer_a_latch = static_cast<uint16_t>((cia1.timer_a_latch & 0x00FF) | (value << 8));
		if (!cia1.timer_a_running) cia1.timer_a_counter = cia1.timer_a_latch;
		break;
	case 0x06:
		cia1.timer_b_latch = static_cast<uint16_t>((cia1.timer_b_latch & 0xFF00) | value);
		break;
	case 0x07:
		cia1.timer_b_latch = static_cast<uint16_t>((cia1.timer_b_latch & 0x00FF) | (value << 8));
		if (!cia1.timer_b_running) cia1.timer_b_counter = cia1.timer_b_latch;
		break;
	case 0x0D:
		if (value & 0x80) cia1.icr_mask |= (value & 0x1F);
		else cia1.icr_mask &= ~(value & 0x1F);
		cia1.timer_a_int_enabled = (cia1.icr_mask & 0x01) != 0;
		cia1.timer_b_int_enabled = (cia1.icr_mask & 0x02) != 0;
		if (cia1.icr_data & cia1.icr_mask & 0x1F) cia1.icr_data |= 0x80;
		else cia1.icr_data &= 0x7F;
		update_irq_line();
		break;
	case 0x0E:
		cia1.cra = value & ~0x10;
		cia1.timer_a_running = (value & 0x01) != 0;
		if (value & 0x10) cia1.timer_a_counter = cia1.timer_a_latch;
		break;
	case 0x0F:
		cia1.crb = value & ~0x10;
		cia1.timer_b_running = (value & 0x01) != 0;
		if (value & 0x10) cia1.timer_b_counter = cia1.timer_b_latch;
		break;
	default:
		RAM[reg] = value;
		break;
	}
}

// ===========================================================================
// CIA2 (VIC bank select / NMI timer)
// ===========================================================================

uint8_t machine_state::read_cia2(const uint16_t address)
{
	const uint16_t reg = static_cast<uint16_t>(0xDD00 | (address & 0x0F));

	switch (reg & 0x0F)
	{
	case 0x00:
		// Port A drives the VIC bank on bits 0-1; lines set to input float high.
		return static_cast<uint8_t>((RAM[reg] & RAM[0xDD02]) | (0xFF & ~RAM[0xDD02]));
	case 0x04: return static_cast<uint8_t>(cia2.timer_a_counter & 0xFF);
	case 0x05: return static_cast<uint8_t>((cia2.timer_a_counter >> 8) & 0xFF);
	case 0x06: return static_cast<uint8_t>(cia2.timer_b_counter & 0xFF);
	case 0x07: return static_cast<uint8_t>((cia2.timer_b_counter >> 8) & 0xFF);
	case 0x0D:
	{
		const uint8_t v = cia2.icr_data;
		cia2.icr_data = 0;
		update_nmi();
		return v;
	}
	case 0x0E: return cia2.cra;
	case 0x0F: return cia2.crb;
	default: return RAM[reg];
	}
}

void machine_state::write_cia2(const uint16_t address, const uint8_t value)
{
	const uint16_t reg = static_cast<uint16_t>(0xDD00 | (address & 0x0F));

	switch (reg & 0x0F)
	{
	case 0x00:
		RAM[reg] = value; // VIC bank select (bits 0-1)
		break;
	case 0x04:
		cia2.timer_a_latch = static_cast<uint16_t>((cia2.timer_a_latch & 0xFF00) | value);
		break;
	case 0x05:
		cia2.timer_a_latch = static_cast<uint16_t>((cia2.timer_a_latch & 0x00FF) | (value << 8));
		if (!cia2.timer_a_running) cia2.timer_a_counter = cia2.timer_a_latch;
		break;
	case 0x06:
		cia2.timer_b_latch = static_cast<uint16_t>((cia2.timer_b_latch & 0xFF00) | value);
		break;
	case 0x07:
		cia2.timer_b_latch = static_cast<uint16_t>((cia2.timer_b_latch & 0x00FF) | (value << 8));
		if (!cia2.timer_b_running) cia2.timer_b_counter = cia2.timer_b_latch;
		break;
	case 0x0D:
		if (value & 0x80) cia2.icr_mask |= (value & 0x1F);
		else cia2.icr_mask &= ~(value & 0x1F);
		cia2.timer_a_int_enabled = (cia2.icr_mask & 0x01) != 0;
		cia2.timer_b_int_enabled = (cia2.icr_mask & 0x02) != 0;
		update_nmi();
		break;
	case 0x0E:
		cia2.cra = value & ~0x10;
		cia2.timer_a_running = (value & 0x01) != 0;
		if (value & 0x10) cia2.timer_a_counter = cia2.timer_a_latch;
		break;
	case 0x0F:
		cia2.crb = value & ~0x10;
		cia2.timer_b_running = (value & 0x01) != 0;
		if (value & 0x10) cia2.timer_b_counter = cia2.timer_b_latch;
		break;
	default:
		RAM[reg] = value;
		break;
	}
}

uint8_t machine_state::sid_read(const uint16_t reg)
{
	return sid.read(reg & 0x1F);
}

void machine_state::sid_write(const uint16_t reg, const uint8_t value)
{
	sid_regs[reg & 0x1F] = value;
	if (audio_enabled) sid.write(reg & 0x1F, value, clock_ticks);
	else sid.write_now(reg & 0x1F, value);
}

// ===========================================================================
// Interrupt line aggregation and per-instruction hardware clocking
// ===========================================================================

void machine_state::update_vic_irq()
{
	// $D019 bit 7 is set while any latched source is also enabled in $D01A.
	if (vic.irq_status & vic.irq_enable & 0x0F) vic.irq_status |= 0x80;
	else vic.irq_status &= 0x7F;
	RAM[0xD019] = vic.irq_status;
	update_irq_line();
}

void machine_state::update_irq_line()
{
	const bool vic_irq = (vic.irq_status & 0x80) != 0;
	const bool cia_irq = (cia1.icr_data & 0x80) != 0;
	irq_pending = vic_irq || cia_irq || ext_irq;
}

void machine_state::update_nmi()
{
	// Any enabled CIA2 source (both timers, serial, flag) drives NMI, edge triggered.
	const bool nmi_active = (cia2.icr_data & cia2.icr_mask & 0x1F) != 0;

	if (nmi_active && cia2.nmi_line)
	{
		cia2.nmi_line = false; // high -> low transition raises the NMI
		nmi_pending = true;
	}
	else if (!nmi_active)
	{
		cia2.nmi_line = true; // release the line
	}
}

void machine_state::tick_cia(cia_chip& c, const int32_t cycles, const bool is_cia2)
{
	const auto raise = [&](const uint8_t flag, const bool enabled)
	{
		c.icr_data |= flag;
		if (enabled)
		{
			c.icr_data |= 0x80;
			if (is_cia2) update_nmi(); else update_irq_line();
		}
	};

	int32_t timer_a_underflows = 0;

	if (c.timer_a_running)
	{
		c.timer_a_counter -= cycles;
		// A single call can span several periods when the latch is smaller than the
		// instruction's cycle count, so keep reloading until the counter is positive.
		while (c.timer_a_counter <= 0)
		{
			++timer_a_underflows;
			if (c.cra & 0x08) // one-shot
			{
				c.timer_a_counter = c.timer_a_latch;
				c.timer_a_running = false;
				c.cra &= ~0x01;
				break;
			}
			c.timer_a_counter += c.timer_a_latch ? c.timer_a_latch : 0x10000;
		}

		if (timer_a_underflows > 0)
		{
			raise(0x01, c.timer_a_int_enabled);

			// Timer B counting Timer A underflows (CRB bits 5-6 = 10 or 11).
			if (c.timer_b_running && (c.crb & 0x60) >= 0x40)
			{
				c.timer_b_counter -= timer_a_underflows;
				bool underflowed = false;
				while (c.timer_b_counter < 0)
				{
					underflowed = true;
					if (c.crb & 0x08)
					{
						c.timer_b_counter = c.timer_b_latch;
						c.timer_b_running = false;
						c.crb &= ~0x01;
						break;
					}
					c.timer_b_counter += c.timer_b_latch ? c.timer_b_latch : 0x10000;
				}
				if (underflowed) raise(0x02, c.timer_b_int_enabled);
			}
		}
	}

	// Timer B counting the system clock (CRB bits 5-6 = 00).
	if (c.timer_b_running && (c.crb & 0x60) == 0x00)
	{
		c.timer_b_counter -= cycles;
		bool underflowed = false;
		while (c.timer_b_counter <= 0)
		{
			underflowed = true;
			if (c.crb & 0x08)
			{
				c.timer_b_counter = c.timer_b_latch;
				c.timer_b_running = false;
				c.crb &= ~0x01;
				break;
			}
			c.timer_b_counter += c.timer_b_latch ? c.timer_b_latch : 0x10000;
		}
		if (underflowed) raise(0x02, c.timer_b_int_enabled);
	}
}

void machine_state::tick_vic(const int32_t cycles)
{
	vic.line_cycle += cycles;
	while (vic.line_cycle >= CYCLES_PER_RASTER_LINE)
	{
		vic.line_cycle -= CYCLES_PER_RASTER_LINE;
		if (++vic.raster_line >= RASTER_LINES_PER_FRAME) vic.raster_line = 0;
	}

	const int32_t current_line = vic.raster_line;
	if (current_line != vic.last_raster_line)
	{
		// Render the line we just finished so a raster-IRQ handler has had the
		// whole line to change VIC registers before we capture them.
		if (render_enabled && vic.last_raster_line >= 0)
			vic_render_scanline(this, vic.last_raster_line);

		if (current_line == vic.raster_compare)
		{
			vic.irq_status |= 0x01;
			update_vic_irq();
		}
		vic.last_raster_line = current_line;
	}
}

void machine_state::step_hardware(const int32_t cycles)
{
	tick_cia(cia1, cycles, false);
	tick_cia(cia2, cycles, true);
	tick_vic(cycles);
}


uint8_t machine::convert_char(const wchar_t c)
{
	if (c == '\"') return 34;
	// Only ASCII letters fold to upper case; PETSCII control codes (cursor keys,
	// function keys) must pass through untouched.
	const auto v = static_cast<uint32_t>(c);
	if (v >= 'a' && v <= 'z') return static_cast<uint8_t>(v - 'a' + 'A');
	return static_cast<uint8_t>(v);
}

void machine::add_char(const wchar_t c)
{
	const auto len = _state->RAM[mem_length_of_keyboard_buffer];

	if (len < 10)
	{
		_state->RAM[mem_keyboard_buffer + len] = convert_char(c);
		_state->RAM[mem_length_of_keyboard_buffer] = static_cast<uint8_t>(len + 1);
	}
}

void machine::add_key(const machine_key key)
{
	switch (key)
	{
	case machine_key::back: add_char(20);
		break;
	case machine_key::up: add_char(145);
		break;
	case machine_key::down: add_char(17);
		break;
	case machine_key::left: add_char(157);
		break;
	case machine_key::right: add_char(29);
		break;
	}
}

machine::machine() : _state(new machine_state())
{
	_state->reset();
}

machine::~machine()
{
	delete _state;
}

bool machine::video_is_invalid() const
{
	auto* snap = _state->video_snapshot;
	const auto* screen = _state->RAM + text_video_mem_offset;
	const auto* colour = _state->RAM + mem_text_color;
	const auto border = _state->RAM[mem_border_color];
	const auto background = _state->RAM[mem_center_color];

	bool changed = false;
	if (std::memcmp(snap, screen, 1000) != 0)
	{
		std::memcpy(snap, screen, 1000);
		changed = true;
	}
	if (std::memcmp(snap + 1000, colour, 1000) != 0)
	{
		std::memcpy(snap + 1000, colour, 1000);
		changed = true;
	}
	if (snap[2000] != border)
	{
		snap[2000] = border;
		changed = true;
	}
	if (snap[2001] != background)
	{
		snap[2001] = background;
		changed = true;
	}

	return changed;
}

void machine::exec(const int32_t tick_count)
{
	if (_state->sid_player.active) sid_player_step(*this);
	cpu_exec(_state, tick_count);
}

// On PAL systems, the CIA1 timer will count 16, 421 CPU cycles before doing another interrupt.
// On NTSC systems, the CIA1 timer will count 17, 045 CPU cycles before doing an interrupt.
// This is because the PAL CPU clock runs at 0.985248 MHz and the NTSC CPU at 1.02273 MHz.
//
// PAL: 60 * 16, 421 / 985, 248 ≈ 1
// NTSC : 60 * 17, 045 / 1, 022, 730 ≈ 1

void machine::irq()
{
	_state->ext_irq = true;
	_state->update_irq_line();
}

bool machine::load_prg(const uint8_t* data, const size_t size)
{
	if (!data || size < 3) return false;

	const uint16_t load_addr = static_cast<uint16_t>(data[0]) |
		(static_cast<uint16_t>(data[1]) << 8);
	const size_t payload = size - 2;
	if (static_cast<size_t>(load_addr) + payload > machine_state::RAM_SIZE)
		return false;

	std::memcpy(_state->RAM + load_addr, data + 2, payload);
	// A program loading over $00/$01 changes the processor port latch/DDR.
	_state->update_memory_map();

	// If this is a BASIC program (load address $0801), fix up the
	// BASIC end-of-program / variable / array / string pointers and
	// inject "RUN\n" so it autostarts.
	if (load_addr == 0x0801)
	{
		const uint16_t end = static_cast<uint16_t>(load_addr + payload);
		_state->RAM[0x2D] = end & 0xFF; // start of variables (=end of BASIC)
		_state->RAM[0x2E] = (end >> 8) & 0xFF;
		_state->RAM[0x2F] = end & 0xFF; // start of arrays
		_state->RAM[0x30] = (end >> 8) & 0xFF;
		_state->RAM[0x31] = end & 0xFF; // end of arrays
		_state->RAM[0x32] = (end >> 8) & 0xFF;

		// Clear the keyboard buffer and inject RUN<RETURN>.
		constexpr char run[] = {'R', 'U', 'N', 13};
		for (int i = 0; i < 4; i++)
			_state->RAM[mem_keyboard_buffer + i] = static_cast<uint8_t>(run[i]);
		_state->RAM[mem_length_of_keyboard_buffer] = 4;
	}

	return true;
}

void machine::set_render_enabled(const bool enabled)
{
	_state->render_enabled = enabled;
}

const uint32_t* machine::framebuffer() const
{
	return _state->framebuffer;
}

void machine::set_joystick(const int port, const bool up, const bool down,
                           const bool left, const bool right, const bool fire)
{
	uint8_t value = 0xFF; // bits active low
	if (up) value &= ~0x01;
	if (down) value &= ~0x02;
	if (left) value &= ~0x04;
	if (right) value &= ~0x08;
	if (fire) value &= ~0x10;

	if (port == 1) _state->joystick1 = value;
	else if (port == 2) _state->joystick2 = value;
}

void machine::press_stop(const bool pressed)
{
	// RUN/STOP is at keyboard-matrix row 7, column 7.
	set_key(7, 7, pressed);
}

void machine::set_key(const int row, const int col, const bool pressed)
{
	if (row < 0 || row > 7 || col < 0 || col > 7) return;
	if (pressed) _state->key_matrix[row] &= static_cast<uint8_t>(~(1u << col));
	else _state->key_matrix[row] |= static_cast<uint8_t>(1u << col);
}

void machine::clear_keys()
{
	for (auto& row : _state->key_matrix) row = 0xFF;
}

bool machine::load_crt(const uint8_t* data, const size_t size)
{
	if (!_state->cart.load(data, size)) return false;
	_state->reset(); // cold-start so the machine boots from the cartridge
	return true;
}

void machine::eject_crt()
{
	_state->cart.eject();
	_state->reset();
}

// ===========================================================================
// Disk drive (device 8)
//
// Rather than emulate the IEC bus and a 1541, the KERNAL LOAD routine is
// serviced directly from the mounted image. The trap sits at $F4A5, the
// default target of the LOAD vector at $0330, so a program that installs its
// own fastloader over that vector is left alone.
// ===========================================================================

namespace
{
	// Zero page, as SETNAM/SETLFS and the LOAD routine leave it.
	constexpr uint16_t ZP_STATUS = 0x90;    // ST, the serial status byte
	constexpr uint16_t ZP_VERIFY = 0x93;    // 0 = load, non-zero = verify
	constexpr uint16_t ZP_NAME_LEN = 0xB7;
	constexpr uint16_t ZP_SECONDARY = 0xB9;
	constexpr uint16_t ZP_DEVICE = 0xBA;
	constexpr uint16_t ZP_NAME_PTR = 0xBB;  // and $BC
	constexpr uint16_t ZP_END_ADDR = 0xAE;  // and $AF
	constexpr uint16_t ZP_LOAD_ADDR = 0xC3; // and $C4, the X/Y passed to LOAD

	constexpr uint8_t KERNAL_FILE_NOT_FOUND = 4;

	std::string pad_to(std::string text, const size_t width)
	{
		if (text.size() > width) text.resize(width);
		text.append(width - text.size(), ' ');
		return text;
	}

	// Render a directory as a BASIC program at $0801, which is what a real drive
	// sends for LOAD"$",8 and what makes the following LIST readable.
	std::vector<uint8_t> directory_listing(const disk_directory& dir)
	{
		std::vector<uint8_t> prg{0x01, 0x08};
		uint16_t address = 0x0801;

		const auto add_line = [&](const uint16_t number, const std::string& text)
		{
			address = static_cast<uint16_t>(address + 4 + text.size() + 1);
			prg.push_back(address & 0xFF);
			prg.push_back(static_cast<uint8_t>(address >> 8));
			prg.push_back(number & 0xFF);
			prg.push_back(static_cast<uint8_t>(number >> 8));
			for (const auto c : text) prg.push_back(static_cast<uint8_t>(c));
			prg.push_back(0);
		};

		add_line(0, "\x12\"" + pad_to(dir.name, 16) + "\" " + pad_to(dir.id, 2) + " 2A");

		for (const auto& entry : dir.entries)
		{
			const size_t digits = entry.blocks >= 100 ? 3 : entry.blocks >= 10 ? 2 : 1;
			std::string text(4 - digits, ' ');
			text += '"' + pad_to(entry.filename, 16) + '"';
			text += entry.closed ? ' ' : '*';
			text += entry.type;
			if (entry.locked) text += '<';
			add_line(entry.blocks, text);
		}

		add_line(dir.free_blocks, "BLOCKS FREE.");
		prg.push_back(0);
		prg.push_back(0);
		return prg;
	}

	// "@0:NAME,P" and friends: strip the drive prefix and the type/mode suffix.
	std::string load_file_name(const machine_state* s)
	{
		uint8_t raw[256];
		const uint8_t length = s->RAM[ZP_NAME_LEN];
		const auto pointer = static_cast<uint16_t>(s->RAM[ZP_NAME_PTR] | s->RAM[ZP_NAME_PTR + 1] << 8);
		for (int i = 0; i < length; ++i) raw[i] = s->RAM[static_cast<uint16_t>(pointer + i)];

		auto name = petscii_to_ascii(raw, length);
		const auto colon = name.find(':');
		if (colon != std::string::npos) name.erase(0, colon + 1);
		const auto comma = name.find(',');
		if (comma != std::string::npos) name.erase(comma);
		return name;
	}
}

bool machine_state::kernal_load()
{
	// Decline anything the ROM should still handle itself.
	if (cpu.a != 0) return false;            // VERIFY
	if (RAM[ZP_DEVICE] != 8) return false;   // tape, or a drive we are not modelling
	if (RAM[ZP_NAME_LEN] == 0) return false; // MISSING FILE NAME

	RAM[ZP_VERIFY] = 0;
	RAM[ZP_STATUS] = 0;

	const auto name = load_file_name(this);
	std::vector<uint8_t> prg;
	bool found = false;

	if (name.starts_with("$"))
	{
		if (disk_directory dir; read_disk_directory(disk.data(), disk.size(), dir))
		{
			prg = directory_listing(dir);
			found = true;
		}
	}
	else
	{
		found = read_disk_file(disk.data(), disk.size(), name, prg);
	}

	// Return through the RTS that the caller of LOAD is waiting on either way.
	const auto stack = [&](const uint8_t offset)
	{
		return RAM[0x0100 + ((cpu.sp + offset) & 0xFF)];
	};
	const auto ret = static_cast<uint16_t>(stack(1) | stack(2) << 8);
	cpu.sp = static_cast<uint8_t>(cpu.sp + 2);
	cpu.pc = static_cast<uint16_t>(ret + 1);

	if (!found || prg.size() < 3)
	{
		cpu.a = KERNAL_FILE_NOT_FOUND;
		cpu.set_carry();
		return true;
	}

	// A secondary address of 0 is a relocating load: BASIC's own start address
	// wins over the one stored in the file.
	const auto file_addr = static_cast<uint16_t>(prg[0] | prg[1] << 8);
	const auto load_addr = RAM[ZP_SECONDARY] != 0
		                       ? file_addr
		                       : static_cast<uint16_t>(RAM[ZP_LOAD_ADDR] | RAM[ZP_LOAD_ADDR + 1] << 8);

	// Write straight to RAM: a load can never be diverted into the I/O area.
	const size_t payload = prg.size() - 2;
	for (size_t i = 0; i < payload; ++i)
		RAM[static_cast<uint16_t>(load_addr + i)] = prg[i + 2];
	update_memory_map(); // the program may have loaded over $00/$01

	const auto end = static_cast<uint16_t>(load_addr + payload);
	RAM[ZP_END_ADDR] = end & 0xFF;
	RAM[ZP_END_ADDR + 1] = static_cast<uint8_t>(end >> 8);
	RAM[ZP_STATUS] = 0x40; // EOI, which is what a completed transfer leaves
	cpu.a = 0;
	cpu.x = end & 0xFF;
	cpu.y = static_cast<uint8_t>(end >> 8);
	cpu.clear_carry();
	return true;
}

bool machine::insert_disk(const uint8_t* data, const size_t size)
{
	if (disk_directory probe; !data || !read_disk_directory(data, size, probe)) return false;
	_state->disk.assign(data, data + size);
	return true;
}

void machine::eject_disk()
{
	_state->disk.clear();
}

bool machine::has_disk() const
{
	return !_state->disk.empty();
}

void machine::set_audio_enabled(const bool enabled)
{
	_state->audio_enabled = enabled;
	// Sync the SID's audio clock to the current CPU cycle so the first
	// generate_audio() call only renders one frame's worth of samples.
	_state->sid.audio_cycle = _state->clock_ticks;
}

void machine::set_sid_model(const sid_model model)
{
	_state->sid.set_chip_model(model);
}

int machine::generate_audio(int16_t* buffer, const int max_samples)
{
	if (!_state->audio_enabled || !buffer || max_samples <= 0) return 0;

	const int64_t now = _state->clock_ticks;
	const int64_t start = _state->sid.audio_cycle;
	int64_t cycles = now - start;
	_state->sid.audio_cycle = now;
	if (cycles <= 0) return 0;
	// Guard against a huge span (e.g. after a reset) narrowing badly and stalling.
	constexpr int64_t max_cycles = 1'000'000;
	if (cycles > max_cycles) cycles = max_cycles;

	_state->sid.begin_frame();
	return _state->sid.clock(static_cast<int>(cycles), buffer, start, max_samples);
}

namespace
{
	// PSID/RSID player driver memory layout. Everything lives in the cassette
	// buffer and the free bytes around it so that the text screen at $0400 stays
	// available for the player's own display.
	constexpr uint16_t SID_NMI_HANDLER = 0x0334;
	constexpr uint16_t SID_IRQ_HANDLER = 0x0340; // simple handler (regs saved by kernal)
	constexpr uint16_t SID_IRQ_FULL = 0x0360;    // full handler (saves its own regs)
	constexpr uint16_t SID_DRIVER = 0x0390;      // main driver
	constexpr uint32_t SID_CLOCK_PAL = 985248;
	constexpr uint32_t SID_CLOCK_NTSC = 1022727;

	uint16_t sid_be16(const uint8_t* p) { return static_cast<uint16_t>((p[0] << 8) | p[1]); }
	uint32_t sid_be32(const uint8_t* p)
	{
		return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
			(static_cast<uint32_t>(p[2]) << 8) | p[3];
	}

	// Select the $0001 memory configuration a routine at `addr` needs so that the
	// right ROMs/I/O are banked in when it runs.
	uint8_t sid_io_map(uint16_t addr, bool is_rsid)
	{
		if (is_rsid || addr == 0) return 0x37;
		if (addr < 0xA000) return 0x37;
		if (addr < 0xD000) return 0x36;
		if (addr >= 0xE000) return 0x35;
		return 0x34;
	}

	// --- The player's text front end ----------------------------------------

	// Copy one of the header's 32-byte fields into a NUL-terminated string,
	// mapping anything unprintable to a space and trimming the padding.
	void sid_header_text(char (&out)[33], const uint8_t* src)
	{
		int len = 0;
		for (int i = 0; i < 32; ++i)
		{
			const uint8_t c = src[i];
			if (c == 0) break;
			out[len++] = (c >= 0x20 && c < 0x7F) ? static_cast<char>(c) : ' ';
		}
		while (len > 0 && out[len - 1] == ' ') --len;
		out[len] = '\0';
	}

	// A 1 KB page in VIC bank 0 for the player's text screen that the tune image
	// does not sit in. $0400 is the usual answer; a tune loading there pushes the
	// screen out of its way.
	uint16_t sid_player_screen_base(uint16_t load, size_t len)
	{
		constexpr uint16_t candidates[] = {0x0400, 0x0800, 0x0C00, 0x3C00, 0x3800};
		const uint32_t end = load + static_cast<uint32_t>(len);
		for (const uint16_t base : candidates)
			if (end <= base || load >= base + 0x0400u) return base;
		return 0x0400;
	}

	// Text mode, screen at the player's page, characters from the ROM at $1000,
	// black border and background. Re-asserted every frame so the display
	// survives a tune that programs VIC registers of its own.
	void sid_player_setup_video(machine_state& s)
	{
		s.RAM[0xD011] = 0x1B;
		s.RAM[0xD016] = 0x08;
		s.RAM[0xD018] = static_cast<uint8_t>(((s.sid_player.screen_base >> 10) << 4) | 0x04);
		s.RAM[0xD020] = 0;
		s.RAM[0xD021] = 0;
		s.RAM[0xDD00] = 0x03; // VIC bank 0
		s.RAM[0xDD02] = 0x3F;
	}

	uint8_t sid_screen_code(char c)
	{
		const auto u = static_cast<unsigned char>(c);
		if (u >= 'a' && u <= 'z') return static_cast<uint8_t>(u - 'a' + 1);
		if (u >= 'A' && u <= 'Z') return static_cast<uint8_t>(u - 'A' + 1);
		if (u >= 0x20 && u <= 0x3F) return u; // space through '?' share their codes
		if (u == '[') return 27;
		if (u == ']') return 29;
		return 32;
	}

	// Writes screen codes and colour nybbles straight into the memory the VIC
	// reads, clipping at the edges of the 40x25 display.
	struct sid_screen
	{
		uint8_t* ram;
		uint16_t base;

		void put(int x, int y, uint8_t code, uint8_t colour) const
		{
			if (x < 0 || x >= 40 || y < 0 || y >= 25) return;
			const int i = y * 40 + x;
			ram[base + i] = code;
			ram[mem_text_color + i] = colour;
		}

		void text(int x, int y, const char* s, uint8_t colour) const
		{
			for (int i = 0; s[i]; ++i) put(x + i, y, sid_screen_code(s[i]), colour);
		}
	};

	constexpr uint8_t SID_UI_BLOCK = 0xA0; // reversed space: a solid block
	constexpr uint8_t SID_UI_TROUGH = 0x40; // horizontal line: the empty meter

	void sid_player_paint(machine_state& s)
	{
		sid_player_state& p = s.sid_player;
		sid_player_setup_video(s);

		const sid_screen w{s.RAM, p.screen_base};
		for (int i = 0; i < 1000; ++i)
		{
			s.RAM[p.screen_base + i] = 32;
			s.RAM[mem_text_color + i] = 14;
		}

		w.text(7, 1, "*** LIB8BIT SID PLAYER ***", 7);

		w.text(2, 3, "NAME", 12);
		w.text(11, 3, p.title, 1);
		w.text(2, 4, "AUTHOR", 12);
		w.text(11, 4, p.author, 1);
		w.text(2, 5, "RELEASED", 12);
		w.text(11, 5, p.released, 1);

		char field[16];
		const int64_t elapsed = (s.clock_ticks - p.start_ticks) / p.clock_hz;
		w.text(2, 7, "CHIP", 12);
		w.text(11, 7, p.chip_8580 ? "8580" : "6581", 3);
		w.text(20, 7, "TIMING", 12);
		w.text(29, 7, p.timed_by_cia ? "CIA" : "VBLANK", 3);
		w.text(2, 8, "TIME", 12);
		std::snprintf(field, sizeof(field), "%02d:%02d",
			static_cast<int>((elapsed / 60) % 100), static_cast<int>(elapsed % 60));
		w.text(11, 8, field, 3);

		// Sub-tunes. Nine fit the number keys, which is more than almost every
		// tune uses; any beyond that are shown as a count.
		w.text(2, 10, "TRACK", 12);
		const int shown = p.songs < 9 ? p.songs : 9;
		for (int i = 0; i < shown; ++i)
		{
			const bool selected = i + 1 == p.song;
			const uint8_t reverse = selected ? 0x80 : 0;
			const uint8_t colour = selected ? 1 : 12;
			const int x = 10 + i * 3;
			w.put(x, 10, static_cast<uint8_t>(32 | reverse), colour);
			w.put(x + 1, 10, static_cast<uint8_t>((0x31 + i) | reverse), colour);
			w.put(x + 2, 10, static_cast<uint8_t>(32 | reverse), colour);
		}
		if (p.songs > shown)
		{
			std::snprintf(field, sizeof(field), "+%d", p.songs - shown);
			w.text(10 + shown * 3, 10, field, 11);
		}

		// Live voice meters. The registers are write-only, so the envelope level
		// comes from the chip itself; it is what makes a voice audible.
		for (int v = 0; v < 3; ++v)
		{
			const int y = 12 + v;
			const uint8_t env = s.sid.envelope_level(v);
			const uint8_t ctrl = s.sid_regs[v * 7 + 4];
			p.peak[v] = env > p.peak[v] ? env : static_cast<uint8_t>(p.peak[v] > 6 ? p.peak[v] - 6 : 0);

			const int filled = env * 16 / 256;
			const int peak = p.peak[v] * 16 / 256;
			std::snprintf(field, sizeof(field), "V%d", v + 1);
			w.text(2, y, field, 12);
			w.put(5, y, sid_screen_code('['), 11);
			for (int i = 0; i < 16; ++i)
			{
				const uint8_t colour = i < 9 ? 5 : i < 13 ? 7 : 2;
				if (i < filled) w.put(6 + i, y, SID_UI_BLOCK, colour);
				else if (i == peak) w.put(6 + i, y, SID_UI_BLOCK, 1);
				else w.put(6 + i, y, SID_UI_TROUGH, 11);
			}
			w.put(22, y, sid_screen_code(']'), 11);

			const char* wave = (ctrl & 0x80) ? "NOISE" : (ctrl & 0x40) ? "PULSE"
				: (ctrl & 0x20) ? "SAW" : (ctrl & 0x10) ? "TRI" : "OFF";
			w.text(24, y, wave, (ctrl & 0x01) ? 7 : 11);
			std::snprintf(field, sizeof(field), "%04X",
				static_cast<unsigned>(s.sid_regs[v * 7] | (s.sid_regs[v * 7 + 1] << 8)));
			w.text(31, y, field, 11);
		}

		w.text(2, 16, "VOLUME", 12);
		const int volume = s.sid_regs[0x18] & 0x0F;
		for (int i = 0; i < 15; ++i)
			w.put(11 + i, 16, i < volume ? SID_UI_BLOCK : SID_UI_TROUGH, i < volume ? 3 : 11);

		w.text(2, 22, "1-9", 7);
		w.text(6, 22, "SELECT TRACK", 12);
		w.text(2, 23, "X", 7);
		w.text(6, 23, "STOP AND RETURN TO BASIC", 12);
	}

	// C64 keyboard matrix positions (row = PA line, col = PB line) of "1".."9"
	// and "X", the player's only controls.
	constexpr struct { uint8_t row, col; } SID_DIGIT_KEYS[9] = {
		{7, 0}, {7, 3}, {1, 0}, {1, 3}, {2, 0}, {2, 3}, {3, 0}, {3, 3}, {4, 0},
	};
	constexpr uint8_t SID_KEY_X_ROW = 2;
	constexpr uint8_t SID_KEY_X_COL = 7;

	bool sid_key_pressed_now(const machine_state& s, uint8_t row, uint8_t col)
	{
		const uint8_t mask = static_cast<uint8_t>(1 << col); // matrix bits are active low
		return (s.key_matrix[row] & mask) == 0 && (s.sid_player.prev_keys[row] & mask) != 0;
	}

	void sid_player_step(machine& m)
	{
		machine_state& s = *m._state;
		sid_player_state& p = s.sid_player;

		int select = 0;
		for (int i = 0; i < 9 && i < p.songs; ++i)
			if (sid_key_pressed_now(s, SID_DIGIT_KEYS[i].row, SID_DIGIT_KEYS[i].col)) select = i + 1;
		const bool stop = sid_key_pressed_now(s, SID_KEY_X_ROW, SID_KEY_X_COL);
		std::memcpy(p.prev_keys, s.key_matrix, sizeof(p.prev_keys));

		if (stop) m.stop_sid();
		else if (select && select != p.song) m.sid_select_song(select);
		else sid_player_paint(s);
	}
}

bool machine::load_sid(const uint8_t* data, const size_t size, const int song)
{
	if (!data || size < 0x7C) return false;

	const uint32_t magic = sid_be32(data);
	constexpr uint32_t PSID = 0x50534944; // "PSID"
	constexpr uint32_t RSID = 0x52534944; // "RSID"
	if (magic != PSID && magic != RSID) return false;

	const bool is_rsid = magic == RSID;
	const uint16_t version = sid_be16(data + 4);
	const uint16_t data_offset = sid_be16(data + 6);
	if (data_offset < 0x76 || data_offset >= size) return false;

	const uint16_t load_address = sid_be16(data + 8);
	uint16_t init_address = sid_be16(data + 10);
	const uint16_t play_address = sid_be16(data + 12);
	const uint16_t songs = sid_be16(data + 14);
	const uint16_t start_song = sid_be16(data + 16);
	const uint32_t speed = sid_be32(data + 18);
	const uint16_t flags = version >= 2 ? sid_be16(data + 118) : 0;

	const uint8_t* program = data + data_offset;
	size_t program_len = size - data_offset;
	uint16_t actual_load = load_address;
	if (load_address == 0)
	{
		if (program_len < 2) return false;
		actual_load = static_cast<uint16_t>(program[0] | (program[1] << 8));
		program += 2;
		program_len -= 2;
	}
	if (init_address == 0) init_address = actual_load;
	if (static_cast<size_t>(actual_load) + program_len > machine_state::RAM_SIZE) return false;

	const int clock_flag = (flags >> 2) & 0x03;
	const bool is_ntsc = clock_flag == 2;
	const int model_flag = (flags >> 4) & 0x03;

	// 1-based song selection; fall back to the file's default. The header fields
	// are untrusted, so clamp into a range the driver's LDA #song can encode.
	const int max_song = songs > 0 ? (songs < 256 ? songs : 256) : 1;
	int song_number = song > 0 ? song : (start_song ? start_song : 1);
	if (song_number > max_song) song_number = max_song;
	if (song_number < 1) song_number = 1;
	const uint8_t song0 = static_cast<uint8_t>(song_number - 1);

	// Fresh machine, then load the tune image and the player driver.
	_state->reset();
	_state->sid.set_chip_model(model_flag == 2 ? sid_model::mos8580 : sid_model::mos6581);

	std::memcpy(_state->RAM + actual_load, program, program_len);

	const bool use_cia = ((speed >> (song0 < 32 ? song0 : 31)) & 1) != 0;
	const uint16_t timer = is_ntsc ? 0x4295 : 0x4025;
	const uint8_t init_io = sid_io_map(init_address, is_rsid);
	const uint8_t play_io = sid_io_map(play_address, is_rsid);

	auto emit = [&](uint16_t addr, std::initializer_list<int> bytes)
	{
		uint16_t p = addr;
		for (const int b : bytes) _state->RAM[p++] = static_cast<uint8_t>(b);
	};

	// NMI handler: RTI.
	emit(SID_NMI_HANDLER, {0x40});

	// Simple IRQ handler (entered via the kernal, which already saved A/X/Y).
	{
		uint16_t p = SID_IRQ_HANDLER;
		auto put = [&](std::initializer_list<int> b) { for (int v : b) _state->RAM[p++] = static_cast<uint8_t>(v); };
		if (play_address != 0)
		{
			put({0xA5, 0x01, 0x48});                 // LDA $01; PHA
			put({0xA9, play_io, 0x85, 0x01});        // LDA #play_io; STA $01
			put({0xA9, 0x00});                       // LDA #$00
			put({0x20, play_address & 0xFF, (play_address >> 8) & 0xFF}); // JSR play
			put({0x68, 0x85, 0x01});                 // PLA; STA $01
		}
		put({0xA9, 0xFF, 0x8D, 0x19, 0xD0});         // LDA #$FF; STA $D019
		put({0xAD, 0x0D, 0xDC});                     // LDA $DC0D
		put({0x68, 0xA8, 0x68, 0xAA, 0x68, 0x40});   // PLA;TAY;PLA;TAX;PLA;RTI
	}

	// Full IRQ handler (saves its own registers; used via the hardware vector).
	{
		uint16_t p = SID_IRQ_FULL;
		auto put = [&](std::initializer_list<int> b) { for (int v : b) _state->RAM[p++] = static_cast<uint8_t>(v); };
		put({0x48, 0x8A, 0x48, 0x98, 0x48});         // PHA;TXA;PHA;TYA;PHA
		if (play_address != 0)
		{
			put({0xA5, 0x01, 0x48});
			put({0xA9, play_io, 0x85, 0x01});
			put({0xA9, 0x00});
			put({0x20, play_address & 0xFF, (play_address >> 8) & 0xFF});
			put({0x68, 0x85, 0x01});
		}
		put({0xA9, 0xFF, 0x8D, 0x19, 0xD0});
		put({0xAD, 0x0D, 0xDC});
		put({0x68, 0xA8, 0x68, 0xAA, 0x68, 0x40});
	}

	// Main driver at $0400.
	{
		uint16_t p = SID_DRIVER;
		auto put = [&](std::initializer_list<int> b) { for (int v : b) _state->RAM[p++] = static_cast<uint8_t>(v); };
		put({0x78});                                 // SEI
		put({0xA9, 0x00, 0x8D, 0x1A, 0xD0});         // LDA #0; STA $D01A
		put({0xAD, 0x19, 0xD0, 0x8D, 0x19, 0xD0});   // LDA $D019; STA $D019
		put({0xA9, 0x7F, 0x8D, 0x0D, 0xDC, 0x8D, 0x0D, 0xDD}); // LDA #$7F; STA $DC0D; STA $DD0D
		put({0xAD, 0x0D, 0xDC, 0xAD, 0x0D, 0xDD});   // LDA $DC0D; LDA $DD0D
		put({0xA9, 0x0F, 0x8D, 0x18, 0xD4});         // LDA #$0F; STA $D418 (max volume)
		put({0xA9, timer & 0xFF, 0x8D, 0x04, 0xDC}); // set CIA1 timer A latch
		put({0xA9, (timer >> 8) & 0xFF, 0x8D, 0x05, 0xDC});
		put({0xA9, init_io, 0x85, 0x01});            // LDA #init_io; STA $01
		put({0xA9, song0, 0x20, init_address & 0xFF, (init_address >> 8) & 0xFF}); // LDA #song; JSR init
		put({0xA9, 0x37, 0x85, 0x01});               // LDA #$37; STA $01
		if (play_address != 0)
		{
			if (use_cia)
			{
				put({0xA9, 0x81, 0x8D, 0x0D, 0xDC}); // enable CIA1 timer A IRQ
				put({0xA9, 0x01, 0x8D, 0x0E, 0xDC}); // start CIA1 timer A
			}
			else
			{
				put({0xA9, 0x1B, 0x8D, 0x11, 0xD0}); // VIC raster IRQ setup
				put({0xA9, 0x00, 0x8D, 0x12, 0xD0});
				put({0xA9, 0x01, 0x8D, 0x1A, 0xD0});
			}
		}
		else
		{
			put({0xA9, 0x01, 0x8D, 0x0E, 0xDC});     // just start CIA1 timer A
		}
		put({0x58});                                 // CLI
		const uint16_t idle = p;
		put({0x4C, idle & 0xFF, (idle >> 8) & 0xFF}); // JMP * (idle loop)
	}

	// Hardware vectors ($FFFA-$FFFF) and software vectors ($0314-$0319).
	emit(0xFFFA, {SID_NMI_HANDLER & 0xFF, (SID_NMI_HANDLER >> 8) & 0xFF,
		SID_DRIVER & 0xFF, (SID_DRIVER >> 8) & 0xFF,
		SID_IRQ_FULL & 0xFF, (SID_IRQ_FULL >> 8) & 0xFF});
	emit(0x0314, {SID_IRQ_HANDLER & 0xFF, (SID_IRQ_HANDLER >> 8) & 0xFF,
		SID_IRQ_HANDLER & 0xFF, (SID_IRQ_HANDLER >> 8) & 0xFF,
		SID_NMI_HANDLER & 0xFF, (SID_NMI_HANDLER >> 8) & 0xFF});

	// Memory configuration + CPU state to enter the driver.
	_state->RAM[0x0001] = 0x35;
	_state->update_memory_map();
	_state->cpu.pc = SID_DRIVER;
	_state->cpu.a = song0;
	_state->cpu.x = 0;
	_state->cpu.y = 0;
	_state->cpu.sp = 0xFF;
	_state->cpu.status = FLAG_CONSTANT | FLAG_INTERRUPT;

	// Front end: remember the tune so another sub-tune can be started, copy the
	// header's metadata and put the screen somewhere the tune does not occupy.
	sid_player_state& p = _state->sid_player;
	p.active = true;
	p.file.assign(data, data + size);
	sid_header_text(p.title, data + 0x16);
	sid_header_text(p.author, data + 0x36);
	sid_header_text(p.released, data + 0x56);
	p.chip_8580 = model_flag == 2;
	p.timed_by_cia = use_cia;
	p.songs = max_song;
	p.song = song_number;
	p.start_ticks = _state->clock_ticks;
	p.clock_hz = is_ntsc ? SID_CLOCK_NTSC : SID_CLOCK_PAL;
	p.screen_base = sid_player_screen_base(actual_load, program_len);
	sid_player_setup_video(*_state);
	sid_player_paint(*_state);

	set_audio_enabled(true);
	return true;
}

bool machine::sid_player_active() const { return _state->sid_player.active; }
int machine::sid_song_count() const { return _state->sid_player.active ? _state->sid_player.songs : 0; }
int machine::sid_song() const { return _state->sid_player.active ? _state->sid_player.song : 0; }

bool machine::sid_select_song(const int song)
{
	const sid_player_state& p = _state->sid_player;
	if (!p.active || song < 1 || song > p.songs) return false;
	// load_sid resets the machine, which clears the player state, so work from a
	// copy of the tune rather than from the member it is about to destroy.
	const std::vector<uint8_t> file = p.file;
	return load_sid(file.data(), file.size(), song);
}

void machine::stop_sid()
{
	_state->reset(); // also clears the player state
	set_audio_enabled(true);
}
