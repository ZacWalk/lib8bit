#pragma once

// lib8bit by Zac Walker
//
// Public interface for the two-pass 6502 assembler and its result type.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// Result of assembling 6502 source text.
struct assembler_result
{
	bool ok = false;
	uint16_t origin = 0;         // load address of the first emitted byte
	std::vector<uint8_t> bytes;  // assembled machine code
	std::string error;           // human-readable message when !ok
	int error_line = 0;          // 1-based source line of the error
};

// A small two-pass 6502 assembler built on the opcode table in opcodes.cpp.
//
// Supported syntax:
//   * Labels:      name:            (define the current address)
//   * Constants:   name = expr      (equate)
//   * Origin:      .org expr   |  * = expr
//   * Data:        .byte/.db  v[,v...]   .word/.dw  v[,v...]   .text/.asc "..."
//   * Comments:    ; to end of line
//   * All documented addressing modes, chosen from the operand syntax.
//   * Numbers:     $hex, %binary, decimal, 'c' (character), and symbols.
//   * Expressions: base [+/- number], with optional < (low byte) / > (high byte).
//                  '*' means the current address.
//
// Zero-page vs absolute is chosen by value: a literal <= $FF uses zero page when
// the instruction has that form; a symbolic operand always assembles as absolute.
assembler_result assemble(std::string_view source);
