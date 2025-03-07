#pragma once

// lib8bit by Zac Walker
//
// 6502 opcode table: one entry per byte value giving the instruction length,
// mnemonic and addressing-mode name. Used by the assembler (opcodes.cpp).

struct opcode
{
	const char* byte;            // hex string (informational)
	int length;                  // instruction length in bytes (1-3)
	const char* name;            // mnemonic, e.g. "LDA" ("UNDEFINED" for illegals)
	const char* addressing_mode; // e.g. "$absolute,X", "#immediate", "relative"
};

extern const opcode opcodes[256];
