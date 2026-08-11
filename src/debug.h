#pragma once

// lib8bit by Zac Walker
//
// Machine introspection for monitors and debug overlays: a side-effect-free
// snapshot of the CPU, banking, VIC-II, CIA and SID state plus a disassembly
// window around the program counter.
//
// Nothing here perturbs the machine. Registers that would latch or clear on a
// real read (CIA $DC0D, VIC $D019, ...) are taken from the emulated chip state
// rather than by reading the bus.

#include <cstddef>
#include <cstdint>

struct machine_state;

constexpr int DEBUG_DISASM_LINES = 12;

struct disasm_line
{
	uint16_t address = 0;
	uint8_t length = 1;    // 1-3
	uint8_t bytes[3] = {};
	char text[20] = {};    // e.g. "LDA ($FB),Y"
};

struct debug_state
{
	// --- CPU ---------------------------------------------------------------
	uint16_t pc = 0;
	uint8_t a = 0, x = 0, y = 0, sp = 0, status = 0;
	int64_t cycles = 0, instructions = 0;
	bool irq_pending = false, nmi_pending = false;

	// --- Banking (processor port at $01) ------------------------------------
	uint8_t port = 0;
	bool basic_on = false, kernal_on = false, io_on = false, char_on = false;
	bool cart_inserted = false;

	// --- VIC-II -------------------------------------------------------------
	uint8_t ctrl1 = 0, ctrl2 = 0, mem_ptr = 0;  // $D011, $D016, $D018
	uint8_t border = 0, background = 0;         // $D020, $D021
	uint16_t raster = 0, raster_compare = 0;
	uint8_t vic_irq_enable = 0, vic_irq_status = 0;
	uint8_t sprite_enable = 0, sprite_sprite_hit = 0, sprite_bg_hit = 0;
	uint16_t vic_bank = 0, screen_base = 0, char_base = 0, bitmap_base = 0;
	const char* mode = "";                      // graphics mode name

	// --- CIA1 / CIA2 --------------------------------------------------------
	// Index 0 = CIA1 timer A, 1 = CIA1 B, 2 = CIA2 A, 3 = CIA2 B.
	uint16_t timer[4] = {};
	uint8_t timer_ctrl[4] = {};
	bool timer_running[4] = {};
	uint8_t cia_icr[2] = {}, cia_mask[2] = {};

	// --- SID ----------------------------------------------------------------
	// Shadow of the write-only registers ($D400-$D41E) plus the live envelope
	// level of each voice, which is what actually tells you if a voice sounds.
	uint8_t sid[32] = {};
	uint8_t sid_env[3] = {};

	// --- Disassembly window -------------------------------------------------
	disasm_line disasm[DEBUG_DISASM_LINES];
	int disasm_count = 0;
	int current = 0; // index into disasm[] of the line at pc
};

// Format the instruction whose opcode and operand bytes are in `bytes[3]`, as
// it would read at `address` (branch targets are resolved). Returns the
// instruction length in bytes (1-3).
int disassemble_6502(uint16_t address, const uint8_t bytes[3], char* out, size_t out_size);

// Read a byte through the current banking map without any I/O side effects.
// Addresses that select an I/O register return the RAM hidden underneath.
uint8_t debug_peek(const machine_state* s, uint16_t address);

void debug_capture(const machine_state* s, debug_state& out);
