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
#include <locale>

#include "cpu.h"
#include "machine.h"

namespace
{
	// PAL VIC-II raster timing.
	constexpr int32_t CYCLES_PER_RASTER_LINE = 63;
	constexpr int32_t RASTER_LINES_PER_FRAME = 312;
	constexpr int32_t CYCLES_PER_FULL_FRAME = CYCLES_PER_RASTER_LINE * RASTER_LINES_PER_FRAME;
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
	update_memory_map();

	irq_pending = false;
	nmi_pending = false;

	cia1 = cia_chip{};
	cia2 = cia_chip{};
	vic = vic_chip{};
	sid.reset();
	sid.audio_cycle = 0;

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
	const uint8_t bank = RAM[1];
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
	// (0 = asserted); the ROM is visible when the corresponding line is low.
	if (cart.enabled)
	{
		const bool cart_exrom_high = cart.exrom == 1; // line inactive
		const bool cart_game_high = cart.game == 1;

		if (cart.has_roml() && !cart_exrom_high)
			std::memset(read_map + 0x80, 5, 0x20); // $8000-$9FFF ROML

		if (cart.has_romh())
		{
			if (!cart_game_high && !cart_exrom_high)
				std::memset(read_map + 0xA0, 6, 0x20); // 16K: ROMH at $A000-$BFFF
			else if (!cart_game_high && cart_exrom_high)
				std::memset(read_map + 0xE0, 6, 0x20); // Ultimax: ROMH at $E000-$FFFF
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
	const int32_t raster_line = vic.raster_cycle / CYCLES_PER_RASTER_LINE;

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
		if ((vic.irq_status & 0x0F) == 0) vic.irq_status &= 0x7F;
		RAM[reg] = vic.irq_status;
		update_irq_line();
		return;
	case 0xD01A:
		vic.irq_enable = value & 0x0F;
		RAM[reg] = value;
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
	switch (address & 0x0F)
	{
	case 0x00: // Port A - joystick 2 + keyboard rows when columns are driven
	{
		uint8_t result = static_cast<uint8_t>(joystick2 & RAM[0xDC00]);
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
		uint8_t result = joystick1;
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
	default: return RAM[address];
	}
}

void machine_state::write_cia1(const uint16_t address, const uint8_t value)
{
	switch (address & 0x0F)
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
		RAM[address] = value;
		break;
	}
}

// ===========================================================================
// CIA2 (VIC bank select / NMI timer)
// ===========================================================================

uint8_t machine_state::read_cia2(const uint16_t address)
{
	switch (address & 0x0F)
	{
	case 0x00: return RAM[address]; // Port A output latch (VIC bank select)
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
	default: return RAM[address];
	}
}

void machine_state::write_cia2(const uint16_t address, const uint8_t value)
{
	switch (address & 0x0F)
	{
	case 0x00:
		RAM[address] = value; // VIC bank select (bits 0-1)
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
		RAM[address] = value;
		break;
	}
}

uint8_t machine_state::sid_read(const uint16_t reg)
{
	return sid.read(reg & 0x1F);
}

void machine_state::sid_write(const uint16_t reg, const uint8_t value)
{
	if (audio_enabled) sid.write(reg & 0x1F, value, clock_ticks);
	else sid.write_now(reg & 0x1F, value);
}

// ===========================================================================
// Interrupt line aggregation and per-instruction hardware clocking
// ===========================================================================

void machine_state::update_irq_line()
{
	const bool vic_irq = (vic.irq_status & 0x80) != 0;
	const bool cia_irq = (cia1.icr_data & 0x80) != 0;
	irq_pending = vic_irq || cia_irq;
}

void machine_state::update_nmi()
{
	// The CIA2 timer-A underflow drives the NMI line (edge triggered).
	const bool nmi_active = (cia2.icr_data & 0x01) != 0 && cia2.timer_a_int_enabled;

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
	if (c.timer_a_running)
	{
		c.timer_a_counter -= cycles;
		if (c.timer_a_counter <= 0)
		{
			c.timer_a_counter += c.timer_a_latch;
			c.icr_data |= 0x01;
			if (c.cra & 0x08) { c.timer_a_running = false; c.cra &= ~0x01; } // one-shot
			if (c.timer_a_int_enabled)
			{
				c.icr_data |= 0x80;
				if (is_cia2) update_nmi(); else update_irq_line();
			}

			// Timer B counting Timer A underflows (CRB bits 5-6 = 10 or 11).
			if (c.timer_b_running && (c.crb & 0x60) >= 0x40)
			{
				if (--c.timer_b_counter < 0)
				{
					c.timer_b_counter = c.timer_b_latch;
					c.icr_data |= 0x02;
					if (c.crb & 0x08) { c.timer_b_running = false; c.crb &= ~0x01; }
					if (c.timer_b_int_enabled)
					{
						c.icr_data |= 0x80;
						if (is_cia2) update_nmi(); else update_irq_line();
					}
				}
			}
		}
	}

	// Timer B counting the system clock (CRB bits 5-6 = 00).
	if (c.timer_b_running && (c.crb & 0x60) == 0x00)
	{
		c.timer_b_counter -= cycles;
		if (c.timer_b_counter <= 0)
		{
			c.timer_b_counter += c.timer_b_latch;
			c.icr_data |= 0x02;
			if (c.crb & 0x08) { c.timer_b_running = false; c.crb &= ~0x01; }
			if (c.timer_b_int_enabled)
			{
				c.icr_data |= 0x80;
				if (is_cia2) update_nmi(); else update_irq_line();
			}
		}
	}
}

void machine_state::tick_vic(const int32_t cycles)
{
	vic.raster_cycle += cycles;
	if (vic.raster_cycle >= CYCLES_PER_FULL_FRAME) vic.raster_cycle -= CYCLES_PER_FULL_FRAME;

	const int32_t current_line = vic.raster_cycle / CYCLES_PER_RASTER_LINE;
	if (current_line != vic.last_raster_line)
	{
		// Render the line we just finished so a raster-IRQ handler has had the
		// whole line to change VIC registers before we capture them.
		if (render_enabled && vic.last_raster_line >= 0)
			vic_render_scanline(this, vic.last_raster_line);

		if (current_line == vic.raster_compare)
		{
			vic.irq_status |= 0x01;
			if (vic.irq_enable & 0x01) vic.irq_status |= 0x80;
			update_irq_line();
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
	return static_cast<uint8_t>(std::toupper(c));
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
	if ((_state->cpu.status & FLAG_INTERRUPT) == 0)
	{
		cpu_irq(_state);
	}
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
	const int64_t cycles = now - start;
	_state->sid.audio_cycle = now;
	if (cycles <= 0) return 0;

	_state->sid.begin_frame();
	return _state->sid.clock(static_cast<int>(cycles), buffer, start, max_samples);
}

namespace
{
	// PSID/RSID player driver memory layout.
	constexpr uint16_t SID_NMI_HANDLER = 0x0380;
	constexpr uint16_t SID_IRQ_HANDLER = 0x0390; // simple handler (regs saved by kernal)
	constexpr uint16_t SID_DRIVER = 0x0400;      // main driver (screen RAM)
	constexpr uint16_t SID_IRQ_FULL = 0x03B0;    // full handler (saves its own regs)
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

	// 1-based song selection; fall back to the file's default.
	int song_number = song > 0 ? song : (start_song ? start_song : 1);
	if (songs > 0 && song_number > songs) song_number = songs;
	const uint8_t song0 = static_cast<uint8_t>(song_number - 1);

	// Fresh machine, then load the tune image and the player driver.
	_state->reset();
	_state->sid.set_chip_model(model_flag == 2 ? sid_model::mos8580 : sid_model::mos6581);

	std::memcpy(_state->RAM + actual_load, program, program_len);

	const bool use_cia = (speed >> song0) & 1;
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

	set_audio_enabled(true);
	return true;
}
