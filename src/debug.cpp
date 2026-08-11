// lib8bit by Zac Walker
//
// Machine introspection (see debug.h): a passive read of the emulated chips
// plus a 6502 disassembler driven by the shared opcode table.

#include "debug.h"

#include "machine.h"
#include "opcodes.h"

#include <cstdio>
#include <cstring>

namespace
{
	// The addressing-mode names in opcodes.cpp, in a form that is cheap to switch
	// on. Matching the strings once here keeps the format table below readable.
	enum addr_mode
	{
		AM_UNKNOWN, AM_IMPLIED, AM_ACCUMULATOR, AM_IMMEDIATE,
		AM_ZP, AM_ZP_X, AM_ZP_Y, AM_ABS, AM_ABS_X, AM_ABS_Y,
		AM_IND, AM_IND_X, AM_IND_Y, AM_RELATIVE,
	};

	addr_mode mode_of(const char* name)
	{
		struct entry { const char* text; addr_mode mode; };
		static constexpr entry table[] = {
			{"implied", AM_IMPLIED},
			{"accumulator", AM_ACCUMULATOR},
			{"#immediate", AM_IMMEDIATE},
			{"$zero page", AM_ZP},
			{"$zero page,X", AM_ZP_X},
			{"$zero page,Y", AM_ZP_Y},
			{"$absolute", AM_ABS},
			{"$absolute,X", AM_ABS_X},
			{"$absolute,Y", AM_ABS_Y},
			{"($indirect)", AM_IND},
			{"($indirect,X)", AM_IND_X},
			{"($indirect),Y", AM_IND_Y},
			{"relative", AM_RELATIVE},
		};
		for (const entry& e : table)
			if (std::strcmp(name, e.text) == 0) return e.mode;
		return AM_UNKNOWN;
	}

	const char* mode_name(int mode)
	{
		switch (mode)
		{
		case 0: return "TEXT";
		case 1: return "MC TEXT";
		case 2: return "BITMAP";
		case 3: return "MC BITMAP";
		case 4: return "ECM TEXT";
		default: return "INVALID";
		}
	}
}

int disassemble_6502(const uint16_t address, const uint8_t bytes[3], char* out, const size_t out_size)
{
	const opcode& op = opcodes[bytes[0]];
	// A JAM has no length; report 1 so callers walking a listing keep advancing.
	const int length = op.length > 0 ? op.length : 1;
	const uint8_t lo = bytes[1];
	const uint16_t word = static_cast<uint16_t>(lo | (bytes[2] << 8));

	switch (mode_of(op.addressing_mode))
	{
	case AM_ACCUMULATOR: std::snprintf(out, out_size, "%s A", op.name); break;
	case AM_IMMEDIATE: std::snprintf(out, out_size, "%s #$%02X", op.name, lo); break;
	case AM_ZP: std::snprintf(out, out_size, "%s $%02X", op.name, lo); break;
	case AM_ZP_X: std::snprintf(out, out_size, "%s $%02X,X", op.name, lo); break;
	case AM_ZP_Y: std::snprintf(out, out_size, "%s $%02X,Y", op.name, lo); break;
	case AM_ABS: std::snprintf(out, out_size, "%s $%04X", op.name, word); break;
	case AM_ABS_X: std::snprintf(out, out_size, "%s $%04X,X", op.name, word); break;
	case AM_ABS_Y: std::snprintf(out, out_size, "%s $%04X,Y", op.name, word); break;
	case AM_IND: std::snprintf(out, out_size, "%s ($%04X)", op.name, word); break;
	case AM_IND_X: std::snprintf(out, out_size, "%s ($%02X,X)", op.name, lo); break;
	case AM_IND_Y: std::snprintf(out, out_size, "%s ($%02X),Y", op.name, lo); break;
	case AM_RELATIVE:
	{
		const uint16_t target = static_cast<uint16_t>(address + 2 + static_cast<int8_t>(lo));
		std::snprintf(out, out_size, "%s $%04X", op.name, target);
		break;
	}
	case AM_UNKNOWN: std::snprintf(out, out_size, "?? $%02X", bytes[0]); break;
	default: std::snprintf(out, out_size, "%s", op.name); break;
	}

	return length;
}

uint8_t debug_peek(const machine_state* s, const uint16_t address)
{
	if (s->raw_ram) return s->RAM[address];

	switch (s->read_map[address >> 8])
	{
	case 1: return rom_basic[address & 0x1FFF];
	case 2: return rom_kernal[address & 0x1FFF];
	case 3: return rom_chars[address & 0x0FFF];
	case 5:
	case 6:
	{
		uint8_t out = 0;
		if (s->cart.enabled && s->cart.read(address, out)) return out;
		if (s->read_map[address >> 8] == 6 && s->is_basic_on && address >= 0xA000 && address < 0xC000)
			return rom_basic[address & 0x1FFF];
		if (s->read_map[address >> 8] == 6 && s->is_kernal_on && address >= 0xE000)
			return rom_kernal[address & 0x1FFF];
		return s->RAM[address];
	}
	default: return s->RAM[address]; // RAM, and the RAM hidden under I/O
	}
}

void debug_capture(const machine_state* s, debug_state& out)
{
	out.pc = s->cpu.pc;
	out.a = s->cpu.a;
	out.x = s->cpu.x;
	out.y = s->cpu.y;
	out.sp = s->cpu.sp;
	out.status = s->cpu.status;
	out.cycles = s->clock_ticks;
	out.instructions = s->instruction_count;
	out.irq_pending = s->irq_pending;
	out.nmi_pending = s->nmi_pending;

	const uint8_t ddr = s->RAM[0];
	out.port = static_cast<uint8_t>((s->RAM[1] & ddr) | (0x17 & ~ddr));
	out.basic_on = s->is_basic_on;
	out.kernal_on = s->is_kernal_on;
	out.io_on = s->is_io_on;
	out.char_on = s->is_char_on;
	out.cart_inserted = s->cart.enabled;

	// VIC-II. The renderer reads its registers straight out of RAM, so this is
	// the same view the picture is drawn from.
	out.ctrl1 = s->RAM[VIC_CTRL1];
	out.ctrl2 = s->RAM[VIC_CTRL2];
	out.mem_ptr = s->RAM[VIC_MEMORY];
	out.border = s->RAM[mem_border_color] & 0x0F;
	out.background = s->RAM[mem_center_color] & 0x0F;
	out.raster = static_cast<uint16_t>(s->vic.raster_line);
	out.raster_compare = s->vic.raster_compare;
	out.vic_irq_enable = s->vic.irq_enable;
	out.vic_irq_status = s->vic.irq_status;
	out.sprite_enable = s->RAM[0xD015];
	out.sprite_sprite_hit = s->RAM[0xD01E];
	out.sprite_bg_hit = s->RAM[0xD01F];

	const int bank_num = (~s->RAM[0xDD00]) & 0x03;
	out.vic_bank = static_cast<uint16_t>(bank_num * 0x4000);
	out.screen_base = static_cast<uint16_t>(out.vic_bank + ((out.mem_ptr >> 4) & 0x0F) * 0x0400);
	out.char_base = static_cast<uint16_t>(out.vic_bank + ((out.mem_ptr >> 1) & 0x07) * 0x0800);
	out.bitmap_base = static_cast<uint16_t>(out.vic_bank + ((out.mem_ptr & 0x08) ? 0x2000 : 0));

	{
		const bool ecm = (out.ctrl1 & 0x40) != 0;
		const bool bmm = (out.ctrl1 & 0x20) != 0;
		const bool mcm = (out.ctrl2 & 0x10) != 0;
		int mode = 0;
		if (ecm && (bmm || mcm)) mode = 5;
		else if (bmm && mcm) mode = 3;
		else if (bmm) mode = 2;
		else if (ecm) mode = 4;
		else if (mcm) mode = 1;
		out.mode = mode_name(mode);
	}

	const cia_chip* const cias[2] = {&s->cia1, &s->cia2};
	for (int i = 0; i < 2; i++)
	{
		const cia_chip& c = *cias[i];
		out.timer[i * 2 + 0] = static_cast<uint16_t>(c.timer_a_counter);
		out.timer[i * 2 + 1] = static_cast<uint16_t>(c.timer_b_counter);
		out.timer_ctrl[i * 2 + 0] = c.cra;
		out.timer_ctrl[i * 2 + 1] = c.crb;
		out.timer_running[i * 2 + 0] = c.timer_a_running;
		out.timer_running[i * 2 + 1] = c.timer_b_running;
		out.cia_icr[i] = c.icr_data;
		out.cia_mask[i] = c.icr_mask;
	}

	std::memcpy(out.sid, s->sid_regs, sizeof(out.sid));
	for (int i = 0; i < 3; i++) out.sid_env[i] = s->sid.envelope_level(i);

	// Disassembly window. Walking backwards through 6502 code is ambiguous, so
	// find the furthest start within a few bytes that decodes exactly onto the
	// PC; if none does, start at the PC.
	constexpr int kBefore = 3;
	int start = s->cpu.pc;
	for (int back = 3 * kBefore; back >= 1; --back)
	{
		const int candidate = s->cpu.pc - back;
		if (candidate < 0) continue;
		int address = candidate, count = 0;
		while (address < s->cpu.pc)
		{
			const int length = opcodes[debug_peek(s, static_cast<uint16_t>(address))].length;
			address += length > 0 ? length : 1;
			++count;
		}
		if (address == s->cpu.pc && count <= kBefore) { start = candidate; break; }
	}

	out.disasm_count = 0;
	out.current = 0;
	uint16_t address = static_cast<uint16_t>(start);
	for (int i = 0; i < DEBUG_DISASM_LINES; i++)
	{
		disasm_line& line = out.disasm[i];
		line.address = address;
		for (int b = 0; b < 3; b++)
			line.bytes[b] = debug_peek(s, static_cast<uint16_t>(address + b));
		line.length = static_cast<uint8_t>(
			disassemble_6502(address, line.bytes, line.text, sizeof(line.text)));
		if (address == s->cpu.pc) out.current = i;
		address = static_cast<uint16_t>(address + line.length);
		out.disasm_count = i + 1;
	}
}

void machine::capture_debug(debug_state& out) const
{
	debug_capture(_state, out);
}
