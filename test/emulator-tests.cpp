// lib8bit by Zac Walker
//
// Headless behavioural test suite: 6502 CPU, VIC-II, CIA and assembler tests,
// plus boot/BASIC/SID/disk checks and the command-line test runners.

#include "disk.h"
#include "machine.h"
#include "assembler.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
	constexpr int execution_step = 100'000;
	constexpr int boot_cycle_limit = 5'000'000;
	constexpr int command_cycle_limit = 2'000'000;

	uint8_t screen_code(const char c)
	{
		if (c >= 'A' && c <= 'Z') return static_cast<uint8_t>(c - 'A' + 1);
		return static_cast<uint8_t>(c);
	}

	bool screen_contains(const machine& c64, const std::string_view text)
	{
		const auto* screen = c64._state->RAM + text_video_mem_offset;
		for (size_t offset = 0; offset + text.size() <= 1000; ++offset)
		{
			bool matches = true;
			for (size_t index = 0; index < text.size(); ++index)
			{
				if ((screen[offset + index] & 0x7f) != screen_code(text[index]))
				{
					matches = false;
					break;
				}
			}
			if (matches) return true;
		}
		return false;
	}

	bool run_until_screen_contains(machine& c64, const std::string_view text, const int cycle_limit)
	{
		for (auto cycles = 0; cycles <= cycle_limit; cycles += execution_step)
		{
			if (screen_contains(c64, text)) return true;
			c64.exec(execution_step);
		}
		return false;
	}

	// Drive the machine the way the GUI app's 50 Hz timer does: fire the periodic
	// interrupt and run a frame's worth of cycles, repeatedly. A correct 6502 IRQ
	// vectors through $FFFE ($FF48) so the kernal handler's register save/restore
	// stays balanced; if it does not, the handler's closing RTI returns to a
	// garbage address and the kernal wipes the screen.
	void run_app_style_ticks(machine& c64, const int ticks)
	{
		constexpr int cycles_per_frame = 1'000'000 / 50;
		for (auto tick = 0; tick < ticks; ++tick)
		{
			c64.irq();
			c64.exec(cycles_per_frame);
		}
	}

	void type_line(machine& c64, const std::string_view text)
	{
		for (const auto c : text)
		{
			while (c64._state->RAM[mem_length_of_keyboard_buffer] >= 10)
				c64.exec(20'000);
			c64.add_char(c);
		}
		while (c64._state->RAM[mem_length_of_keyboard_buffer] >= 10)
			c64.exec(20'000);
		c64.add_char(13);
	}

	std::vector<uint8_t> read_file(const std::filesystem::path& path)
	{
		std::ifstream stream(path, std::ios::binary | std::ios::ate);
		if (!stream) return {};
		const auto size = stream.tellg();
		if (size <= 0) return {};
		std::vector<uint8_t> bytes(static_cast<size_t>(size));
		stream.seekg(0);
		stream.read(reinterpret_cast<char*>(bytes.data()), size);
		return bytes;
	}

	bool list_disk(const std::filesystem::path& path, disk_directory* parsed = nullptr)
	{
		const auto bytes = read_file(path);
		disk_directory directory;
		if (!read_disk_directory(bytes.data(), bytes.size(), directory))
		{
			std::cerr << "Unable to read disk directory: " << path.string() << '\n';
			return false;
		}

		std::cout << disk_format_name(directory.format) << " disk: \"" << directory.name << "\"";
		if (!directory.id.empty()) std::cout << " ID " << directory.id;
		std::cout << '\n';
		for (const auto& entry : directory.entries)
		{
			std::cout << "  " << entry.blocks << "  \"" << entry.filename << "\" " << entry.type;
			if (!entry.closed) std::cout << '*';
			if (entry.locked) std::cout << '<';
			std::cout << '\n';
		}

		if (parsed) *parsed = std::move(directory);
		return true;
	}

	bool has_entry_starting_with(const disk_directory& directory, const std::string_view prefix)
	{
		return std::any_of(directory.entries.begin(), directory.entries.end(), [prefix](const auto& entry)
		{
			return entry.filename.starts_with(prefix);
		});
	}

	bool recognizes_empty_disk(const size_t image_size, const size_t header_offset,
		const disk_image_format expected_format)
	{
		std::vector<uint8_t> bytes(image_size);
		bytes[header_offset] = 0;
		disk_directory directory;
		return read_disk_directory(bytes.data(), bytes.size(), directory) && directory.format == expected_format;
	}

	uint16_t read_big_endian_16(const std::vector<uint8_t>& bytes, const size_t offset)
	{
		return static_cast<uint16_t>(bytes[offset] << 8 | bytes[offset + 1]);
	}

	uint32_t read_big_endian_32(const std::vector<uint8_t>& bytes, const size_t offset)
	{
		return static_cast<uint32_t>(bytes[offset]) << 24 |
			static_cast<uint32_t>(bytes[offset + 1]) << 16 |
			static_cast<uint32_t>(bytes[offset + 2]) << 8 |
			bytes[offset + 3];
	}

	struct crt_info
	{
		bool valid = false;
		uint16_t hardware_type = 0;
		size_t chip_count = 0;
		size_t rom_size = 0;
	};

	crt_info inspect_crt(const std::vector<uint8_t>& bytes)
	{
		constexpr std::string_view signature = "C64 CARTRIDGE   ";
		crt_info result;
		if (bytes.size() < 0x40 || !std::equal(signature.begin(), signature.end(), bytes.begin()))
			return result;

		const auto header_size = read_big_endian_32(bytes, 0x10);
		if (header_size < 0x40 || header_size > bytes.size()) return result;
		result.hardware_type = read_big_endian_16(bytes, 0x16);

		auto offset = static_cast<size_t>(header_size);
		while (offset < bytes.size())
		{
			if (bytes.size() - offset < 16 ||
				!std::equal("CHIP", "CHIP" + 4, bytes.begin() + offset))
				return result;

			const auto packet_size = read_big_endian_32(bytes, offset + 4);
			const auto rom_size = read_big_endian_16(bytes, offset + 14);
			if (packet_size < 16 || rom_size == 0 || rom_size > 0x4000 ||
				packet_size < 16u + rom_size || packet_size > bytes.size() - offset)
				return result;

			++result.chip_count;
			result.rom_size += rom_size;
			offset += packet_size;
		}

		result.valid = result.chip_count != 0;
		return result;
	}

	struct test_runner
	{
		int failures = 0;

		void check(const bool result, const std::string_view name)
		{
			std::cout << (result ? "PASS " : "FAIL ") << name << '\n';
			if (!result) ++failures;
		}
	};

	// ======================================================================
	// Headless 6502 CPU test harness
	// ======================================================================
	// Runs small hand-assembled programs on a bare 6502 + flat 64K RAM (no
	// banking, no I/O), the way Klaus Dormann's functional test expects. Each
	// program ends at a JMP-to-self trap so we can detect completion.
	struct cpu_test
	{
		machine m;

		cpu_test() { m._state->raw_ram = true; }

		void poke(uint16_t addr, std::initializer_list<uint8_t> bytes)
		{
			for (const uint8_t b : bytes) m._state->RAM[addr++] = b;
		}

		void poke(uint16_t addr, uint8_t value) { m._state->RAM[addr] = value; }

		// Load code at $0200, append a JMP-to-self trap, run until it traps.
		void run_prog(std::vector<uint8_t> code)
		{
			constexpr uint16_t base = 0x0200;
			const auto trap = static_cast<uint16_t>(base + code.size());
			code.push_back(0x4C);
			code.push_back(static_cast<uint8_t>(trap & 0xFF));
			code.push_back(static_cast<uint8_t>((trap >> 8) & 0xFF));
			uint16_t p = base;
			for (const uint8_t b : code) m._state->RAM[p++] = b;

			m._state->cpu.a = m._state->cpu.x = m._state->cpu.y = 0;
			m._state->cpu.sp = 0xFD;
			m._state->cpu.status = FLAG_CONSTANT;
			m._state->cpu.pc = base;
			for (int i = 0; i < 100000; ++i)
			{
				const uint16_t before = m._state->cpu.pc;
				m.exec(1);
				if (m._state->cpu.pc == before) break;
			}
		}

		// Cycles consumed by one instruction assembled at $0200.
		int64_t instr_cycles(std::initializer_list<uint8_t> code, uint8_t init_status = FLAG_CONSTANT)
		{
			uint16_t p = 0x0200;
			for (const uint8_t b : code) m._state->RAM[p++] = b;
			m._state->cpu.pc = 0x0200;
			m._state->cpu.status = init_status;
			const int64_t before = m._state->clock_ticks;
			m.exec(1);
			return m._state->clock_ticks - before;
		}

		uint8_t a() const { return m._state->cpu.a; }
		uint8_t x() const { return m._state->cpu.x; }
		uint8_t y() const { return m._state->cpu.y; }
		uint8_t mem(uint16_t addr) const { return m._state->RAM[addr]; }
		bool flag(uint8_t f) const { return (m._state->cpu.status & f) != 0; }
	};

	void run_cpu_tests(test_runner& tests)
	{
		// --- Loads / flags ---
		{ cpu_test t; t.run_prog({0xA9, 0x00}); tests.check(t.a() == 0x00 && t.flag(FLAG_ZERO) && !t.flag(FLAG_SIGN), "LDA #0 sets Z"); }
		{ cpu_test t; t.run_prog({0xA9, 0x80}); tests.check(t.a() == 0x80 && t.flag(FLAG_SIGN) && !t.flag(FLAG_ZERO), "LDA #$80 sets N"); }
		{ cpu_test t; t.run_prog({0xA9, 0x42, 0xAA}); tests.check(t.x() == 0x42, "TAX transfers A to X"); }

		// --- ADC / SBC (binary + decimal) ---
		{ cpu_test t; t.run_prog({0x18, 0xA9, 0x50, 0x69, 0x50}); tests.check(t.a() == 0xA0 && t.flag(FLAG_OVERFLOW) && t.flag(FLAG_SIGN) && !t.flag(FLAG_CARRY), "ADC signed overflow"); }
		{ cpu_test t; t.run_prog({0x18, 0xA9, 0xFF, 0x69, 0x01}); tests.check(t.a() == 0x00 && t.flag(FLAG_CARRY) && t.flag(FLAG_ZERO), "ADC carry out"); }
		{ cpu_test t; t.run_prog({0xF8, 0x18, 0xA9, 0x09, 0x69, 0x01}); tests.check(t.a() == 0x10, "ADC decimal 09+01=10"); }
		{ cpu_test t; t.run_prog({0xF8, 0x18, 0xA9, 0x99, 0x69, 0x01}); tests.check(t.a() == 0x00 && t.flag(FLAG_CARRY), "ADC decimal 99+01 carries"); }
		{ cpu_test t; t.run_prog({0x38, 0xA9, 0x50, 0xE9, 0x10}); tests.check(t.a() == 0x40 && t.flag(FLAG_CARRY), "SBC borrowless"); }
		{ cpu_test t; t.run_prog({0x18, 0xA9, 0x50, 0xE9, 0x10}); tests.check(t.a() == 0x3F, "SBC with borrow"); }
		{ cpu_test t; t.run_prog({0xF8, 0x38, 0xA9, 0x50, 0xE9, 0x25}); tests.check(t.a() == 0x25 && t.flag(FLAG_CARRY), "SBC decimal 50-25=25"); }

		// --- Decimal mode (BCD): value + carry are deterministic on the NMOS 6502
		//     (cases drawn from Bruce Clark's decimal-mode reference). ---
		{ cpu_test t; t.run_prog({0xF8, 0x18, 0xA9, 0x24, 0x69, 0x56}); tests.check(t.a() == 0x80 && !t.flag(FLAG_CARRY), "ADC decimal 24+56=80"); }
		{ cpu_test t; t.run_prog({0xF8, 0x18, 0xA9, 0x93, 0x69, 0x82}); tests.check(t.a() == 0x75 && t.flag(FLAG_CARRY), "ADC decimal 93+82=175 carries"); }
		{ cpu_test t; t.run_prog({0xF8, 0x18, 0xA9, 0x50, 0x69, 0x50}); tests.check(t.a() == 0x00 && t.flag(FLAG_CARRY), "ADC decimal 50+50=100 carries"); }
		{ cpu_test t; t.run_prog({0xF8, 0x38, 0xA9, 0x00, 0xE9, 0x01}); tests.check(t.a() == 0x99 && !t.flag(FLAG_CARRY), "SBC decimal 00-01 borrows to 99"); }
		{ cpu_test t; t.run_prog({0xF8, 0x38, 0xA9, 0x46, 0xE9, 0x12}); tests.check(t.a() == 0x34 && t.flag(FLAG_CARRY), "SBC decimal 46-12=34"); }
		{ cpu_test t; t.run_prog({0xF8, 0x18, 0xA9, 0x32, 0xE9, 0x02}); tests.check(t.a() == 0x29, "SBC decimal 32-02 with borrow-in = 29"); }

		// --- Logic / shifts / rotates ---
		{ cpu_test t; t.run_prog({0xA9, 0xCC, 0x29, 0x0F}); tests.check(t.a() == 0x0C, "AND immediate"); }
		{ cpu_test t; t.run_prog({0xA9, 0xC0, 0x09, 0x0F}); tests.check(t.a() == 0xCF, "ORA immediate"); }
		{ cpu_test t; t.run_prog({0xA9, 0xFF, 0x49, 0x0F}); tests.check(t.a() == 0xF0, "EOR immediate"); }
		{ cpu_test t; t.run_prog({0xA9, 0x81, 0x0A}); tests.check(t.a() == 0x02 && t.flag(FLAG_CARRY), "ASL A"); }
		{ cpu_test t; t.run_prog({0xA9, 0x03, 0x4A}); tests.check(t.a() == 0x01 && t.flag(FLAG_CARRY), "LSR A"); }
		{ cpu_test t; t.run_prog({0x38, 0xA9, 0x80, 0x2A}); tests.check(t.a() == 0x01 && t.flag(FLAG_CARRY), "ROL A through carry"); }
		{ cpu_test t; t.run_prog({0x38, 0xA9, 0x01, 0x6A}); tests.check(t.a() == 0x80 && t.flag(FLAG_CARRY) && t.flag(FLAG_SIGN), "ROR A through carry"); }

		// --- Inc / dec ---
		{ cpu_test t; t.run_prog({0xA2, 0xFF, 0xE8}); tests.check(t.x() == 0x00 && t.flag(FLAG_ZERO), "INX wraps to 0"); }
		{ cpu_test t; t.run_prog({0xA2, 0x00, 0xCA}); tests.check(t.x() == 0xFF && t.flag(FLAG_SIGN), "DEX wraps to $FF"); }
		{ cpu_test t; t.run_prog({0xA9, 0x7F, 0x8D, 0x00, 0x03, 0xEE, 0x00, 0x03}); tests.check(t.mem(0x0300) == 0x80, "INC memory"); }
		{ cpu_test t; t.run_prog({0xA9, 0x01, 0x8D, 0x00, 0x03, 0xCE, 0x00, 0x03}); tests.check(t.mem(0x0300) == 0x00, "DEC memory"); }

		// --- Addressing modes ---
		{ cpu_test t; t.run_prog({0xA2, 0x05, 0xA9, 0xAA, 0x95, 0x80}); tests.check(t.mem(0x85) == 0xAA, "STA zero-page,X"); }
		{ cpu_test t; t.poke(0x0313, 0x77); t.run_prog({0xA2, 0x03, 0xBD, 0x10, 0x03}); tests.check(t.a() == 0x77, "LDA absolute,X"); }
		{ cpu_test t; t.poke(0x0010, {0x00, 0x04}); t.poke(0x0402, 0x99); t.run_prog({0xA0, 0x02, 0xB1, 0x10}); tests.check(t.a() == 0x99, "LDA (indirect),Y"); }

		// --- Compares / bit ---
		{ cpu_test t; t.run_prog({0xA9, 0x50, 0xC9, 0x50}); tests.check(t.flag(FLAG_ZERO) && t.flag(FLAG_CARRY), "CMP equal"); }
		{ cpu_test t; t.run_prog({0xA9, 0x50, 0xC9, 0x40}); tests.check(t.flag(FLAG_CARRY) && !t.flag(FLAG_ZERO), "CMP greater"); }
		{ cpu_test t; t.run_prog({0xA9, 0x40, 0xC9, 0x50}); tests.check(!t.flag(FLAG_CARRY) && t.flag(FLAG_SIGN), "CMP less"); }
		{ cpu_test t; t.poke(0x0300, 0xC0); t.run_prog({0xA9, 0x00, 0x2C, 0x00, 0x03}); tests.check(t.flag(FLAG_SIGN) && t.flag(FLAG_OVERFLOW) && t.flag(FLAG_ZERO), "BIT sets N/V from memory"); }

		// --- Stack / subroutines / branches ---
		{ cpu_test t; t.run_prog({0xA9, 0x33, 0x48, 0xA9, 0x00, 0x68}); tests.check(t.a() == 0x33, "PHA/PLA round-trip"); }
		{ cpu_test t; t.poke(0x0220, {0xA9, 0x5A, 0x60}); t.run_prog({0x20, 0x20, 0x02, 0x8D, 0x00, 0x03}); tests.check(t.mem(0x0300) == 0x5A && t.a() == 0x5A, "JSR/RTS"); }
		{ cpu_test t; t.run_prog({0xA9, 0x00, 0xF0, 0x02, 0xA9, 0xFF}); tests.check(t.a() == 0x00, "BEQ taken skips code"); }
		{ cpu_test t; t.run_prog({0xA9, 0x00, 0xD0, 0x02, 0xA9, 0xFF}); tests.check(t.a() == 0xFF, "BNE not taken falls through"); }

		// --- Cycle counts (base timings + the 6502 page-cross penalty) ---
		{ cpu_test t; tests.check(t.instr_cycles({0xEA}) == 2, "cycles: NOP = 2"); }
		{ cpu_test t; tests.check(t.instr_cycles({0xA9, 0x00}) == 2, "cycles: LDA # = 2"); }
		{ cpu_test t; tests.check(t.instr_cycles({0xA5, 0x00}) == 3, "cycles: LDA zp = 3"); }
		{ cpu_test t; tests.check(t.instr_cycles({0xAD, 0x00, 0x02}) == 4, "cycles: LDA abs = 4"); }
		{ cpu_test t; t.m._state->cpu.x = 1; tests.check(t.instr_cycles({0xBD, 0x00, 0x02}) == 4, "cycles: LDA abs,X no page cross = 4"); }
		{ cpu_test t; t.m._state->cpu.x = 1; tests.check(t.instr_cycles({0xBD, 0xFF, 0x02}) == 5, "cycles: LDA abs,X page cross = 5"); }
		{ cpu_test t; t.m._state->cpu.y = 1; tests.check(t.instr_cycles({0xB9, 0xFF, 0x02}) == 5, "cycles: LDA abs,Y page cross = 5"); }
		{ cpu_test t; tests.check(t.instr_cycles({0x4C, 0x00, 0x02}) == 3, "cycles: JMP abs = 3"); }
		{ cpu_test t; tests.check(t.instr_cycles({0x20, 0x00, 0x02}) == 6, "cycles: JSR = 6"); }
		{ cpu_test t; tests.check(t.instr_cycles({0xF0, 0x02}, FLAG_CONSTANT) == 2, "cycles: branch not taken = 2"); }
		{ cpu_test t; tests.check(t.instr_cycles({0xF0, 0x02}, FLAG_CONSTANT | FLAG_ZERO) == 3, "cycles: branch taken same page = 3"); }
		{ cpu_test t; tests.check(t.instr_cycles({0xF0, 0xFD}, FLAG_CONSTANT | FLAG_ZERO) == 4, "cycles: branch taken page cross = 4"); }
	}

	// Validate the built-in 6502 assembler (src/assembler.cpp) by checking exact
	// encodings, addressing-mode selection, data directives, error reporting, and
	// by round-tripping an assembled program through the CPU.
	void run_assembler_tests(test_runner& tests)
	{
		{
			const auto r = assemble("LDA #$01\nSTA $0400");
			const std::vector<uint8_t> expect{0xA9, 0x01, 0x8D, 0x00, 0x04};
			tests.check(r.ok && r.bytes == expect, "ASM: immediate + absolute encode");
		}
		{
			const auto r = assemble(".org $0200\nLDA $10");
			const std::vector<uint8_t> expect{0xA5, 0x10};
			tests.check(r.ok && r.origin == 0x0200 && r.bytes == expect, "ASM: zero-page selected by value");
		}
		{
			const auto r = assemble("*=$0200\nloop: INX\n CPX #$05\n BNE loop\n STX $0400");
			const std::vector<uint8_t> expect{0xE8, 0xE0, 0x05, 0xD0, 0xFB, 0x8E, 0x00, 0x04};
			tests.check(r.ok && r.bytes == expect, "ASM: labels and relative branch");
		}
		{
			const auto r = assemble(".byte $01,$02\n.word $0403");
			const std::vector<uint8_t> expect{0x01, 0x02, 0x03, 0x04};
			tests.check(r.ok && r.bytes == expect, "ASM: .byte and .word emit little-endian data");
		}
		{
			const auto r = assemble("LDA #$01\nFOO $10");
			tests.check(!r.ok && r.error_line == 2, "ASM: unknown mnemonic reports the error line");
		}
		{
			// Assemble a small counting loop and run it on the CPU (relative branches
			// are position-independent, so loading at $0200 matches the *=$0200 origin).
			const auto r = assemble("*=$0200\n LDX #$00\nloop: INX\n CPX #$05\n BNE loop\n STX $0300");
			cpu_test t; t.run_prog(r.bytes);
			tests.check(r.ok && t.x() == 0x05 && t.mem(0x0300) == 0x05, "ASM: assembled loop runs on the CPU");
		}
	}

	// Direct VIC-II graphics validation: set up registers + video memory, render
	// a single raster line with the real renderer, and assert exact framebuffer
	// pixels / collision / raster behaviour.
	void run_vic_tests(test_runner& tests)
	{
		constexpr int W = VIC_FB_WIDTH;
		constexpr int BX = 32; // border width (left)  = (384-320)/2
		constexpr int BY = 36; // border height (top)  = (272-200)/2

		auto setup_text = [](machine& m)
		{
			auto* r = m._state->RAM;
			r[0xD011] = 0x18; // DEN + RSEL, YSCROLL 0
			r[0xD016] = 0x08; // CSEL, no MCM
			r[0xD018] = 0x18; // screen $0400, char/bitmap base $2000 (in RAM, so we control glyphs)
			r[0xD020] = 14;   // border light blue
			r[0xD021] = 6;    // background blue
			r[0xDD00] = 0x03; // VIC bank 0
		};

		// --- Raster counter + raster interrupt (driven directly, no CPU) ---
		{
			machine m;
			m._state->vic = vic_chip{};
			m._state->vic.raster_compare = 100;
			m._state->vic.irq_enable = 0x01;
			m._state->irq_pending = false;
			for (int i = 0; i < 400; ++i) m._state->tick_vic(63); // one raster line per call
			tests.check((m._state->vic.irq_status & 0x01) != 0, "VIC: raster compare sets the IRQ flag");
			tests.check((m._state->vic.irq_status & 0x80) != 0, "VIC: enabled raster IRQ asserts (bit 7)");
			tests.check(m._state->irq_pending, "VIC: raster IRQ raises the CPU IRQ line");
			m._state->write_vic(0xD019, 0x01);
			tests.check((m._state->vic.irq_status & 0x81) == 0, "VIC: writing $D019 acknowledges the raster IRQ");
		}

		// --- Standard character mode ---
		{
			machine m;
			setup_text(m);
			m._state->RAM[0x0400] = 1;                // char code 1 at screen (0,0)
			m._state->RAM[0x2000 + 1 * 8 + 0] = 0x80; // glyph row 0: leftmost pixel set
			m._state->RAM[0xD800] = 1;                // colour RAM = white
			vic_render_scanline(m._state, 51);        // raster 51 -> top pixel row of char row 0
			const auto* fb = m.framebuffer();
			tests.check(fb[BY * W + BX + 0] == vic_palette[1], "VIC: character foreground pixel");
			tests.check(fb[BY * W + BX + 1] == vic_palette[6], "VIC: character background pixel");
			tests.check(fb[BY * W + 0] == vic_palette[14], "VIC: border pixel outside the screen");
		}

		// --- Multicolor character mode ---
		{
			machine m;
			setup_text(m);
			m._state->RAM[0xD016] |= 0x10;            // MCM
			m._state->RAM[0xD022] = 8;                // background 1 orange
			m._state->RAM[0xD023] = 5;                // background 2 green
			m._state->RAM[0x0400] = 1;
			m._state->RAM[0xD800] = 0x08 | 3;         // multicolor flag + foreground colour 3 (cyan)
			m._state->RAM[0x2000 + 1 * 8 + 0] = 0x1B; // bit-pairs 00 01 10 11
			vic_render_scanline(m._state, 51);
			const auto* fb = m.framebuffer();
			tests.check(fb[BY * W + BX + 0] == vic_palette[6], "VIC: multicolor pair 00 = background 0");
			tests.check(fb[BY * W + BX + 2] == vic_palette[8], "VIC: multicolor pair 01 = background 1");
			tests.check(fb[BY * W + BX + 4] == vic_palette[5], "VIC: multicolor pair 10 = background 2");
			tests.check(fb[BY * W + BX + 6] == vic_palette[3], "VIC: multicolor pair 11 = foreground");
		}

		// --- Standard bitmap mode ---
		{
			machine m;
			setup_text(m);
			m._state->RAM[0xD011] |= 0x20;   // BMM
			m._state->RAM[0x0400] = 0x1C;    // cell colours: fg=1 white, bg=12 grey
			m._state->RAM[0x2000 + 0] = 0x80; // bitmap row 0: leftmost bit set
			vic_render_scanline(m._state, 51);
			const auto* fb = m.framebuffer();
			tests.check(fb[BY * W + BX + 0] == vic_palette[1], "VIC: bitmap foreground pixel");
			tests.check(fb[BY * W + BX + 1] == vic_palette[12], "VIC: bitmap background pixel");
		}

		// --- Sprite rendering ---
		{
			machine m;
			setup_text(m);
			m._state->RAM[0xD015] = 0x01; // sprite 0 enabled
			m._state->RAM[0xD000] = 100;  // X
			m._state->RAM[0xD001] = 60;   // Y
			m._state->RAM[0x07F8] = 0x20; // sprite 0 pointer -> data at $0800
			m._state->RAM[0x0800] = 0x80; // top-left pixel of the sprite
			m._state->RAM[0xD027] = 2;    // sprite 0 colour red
			vic_render_scanline(m._state, 61); // sprite's top row
			const auto* fb = m.framebuffer();
			const int y = 60 - 50 + BY;   // sprite_y
			const int sx = 100 - 24 + BX; // sprite_x
			tests.check(fb[y * W + sx] == vic_palette[2], "VIC: sprite pixel rendered at expected position");
		}

		// --- Sprite-to-sprite collision ---
		{
			machine m;
			setup_text(m);
			m._state->RAM[0xD015] = 0x03; // sprites 0 and 1
			m._state->RAM[0xD000] = 100; m._state->RAM[0xD001] = 60;
			m._state->RAM[0xD002] = 100; m._state->RAM[0xD003] = 60; // same position
			m._state->RAM[0x07F8] = 0x20; m._state->RAM[0x07F9] = 0x20;
			m._state->RAM[0x0800] = 0x80;
			m._state->RAM[0xD01E] = 0;
			vic_render_scanline(m._state, 61);
			tests.check((m._state->RAM[0xD01E] & 0x03) == 0x03, "VIC: sprite-sprite collision flags both sprites");
		}

		// --- Sprite-to-background collision (foreground vs blank) ---
		{
			machine m;
			setup_text(m);
			for (int i = 0; i < 8; ++i) m._state->RAM[0x2000 + 1 * 8 + i] = 0xFF; // solid glyph
			for (int c = 0; c < 40; ++c) m._state->RAM[0x0400 + 40 + c] = 1;      // fill char row 1
			m._state->RAM[0xD015] = 0x01;
			m._state->RAM[0xD000] = 100; m._state->RAM[0xD001] = 60;
			m._state->RAM[0x07F8] = 0x20; m._state->RAM[0x0800] = 0x80;
			m._state->RAM[0xD01F] = 0;
			vic_render_scanline(m._state, 61);
			tests.check((m._state->RAM[0xD01F] & 0x01) != 0, "VIC: sprite collides with foreground graphics");
		}
		{
			machine m;
			setup_text(m); // blank screen (char 0, empty glyph) = background only
			m._state->RAM[0xD015] = 0x01;
			m._state->RAM[0xD000] = 100; m._state->RAM[0xD001] = 60;
			m._state->RAM[0x07F8] = 0x20; m._state->RAM[0x0800] = 0x80;
			m._state->RAM[0xD01F] = 0;
			vic_render_scanline(m._state, 61);
			tests.check((m._state->RAM[0xD01F] & 0x01) == 0, "VIC: no sprite-background collision over blank screen");
		}

		// --- Register read-back semantics (inspired by VICE's VIC-II register
		//     tests): unused bits read as 1, mirroring, raster/collision reads ---
		{
			machine m;
			m._state->vic = vic_chip{};
			// Unused bits are forced to 1 on read regardless of what was written.
			m._state->write_vic(0xD016, 0x00);
			tests.check((m._state->read_vic(0xD016) & 0xC0) == 0xC0, "VIC: $D016 unused bits 6-7 read as 1");
			m._state->write_vic(0xD018, 0x00);
			tests.check((m._state->read_vic(0xD018) & 0x01) == 0x01, "VIC: $D018 unused bit 0 reads as 1");
			m._state->write_vic(0xD01A, 0x00);
			tests.check((m._state->read_vic(0xD01A) & 0xF0) == 0xF0, "VIC: $D01A unused bits 4-7 read as 1");
			m._state->write_vic(0xD020, 0x00);
			tests.check((m._state->read_vic(0xD020) & 0xF0) == 0xF0, "VIC: $D020 colour high nibble reads as 1");
			tests.check(m._state->read_vic(0xD02F) == 0xFF, "VIC: non-existent $D02F reads as $FF");

			// The chip mirrors its 64-byte register block through $D3FF.
			m._state->write_vic(0xD021, 0x0A);
			tests.check((m._state->read_vic(0xD061) & 0x0F) == 0x0A, "VIC: register block mirrors at $D040+ ($D061 == $D021)");
			m._state->write_vic(0xD861, 0x0C); // mirror write lands in $D021
			tests.check((m._state->read_vic(0xD021) & 0x0F) == 0x0C, "VIC: mirror writes reach the real register");

			// Collision registers are read-only and clear themselves on read.
			m._state->RAM[0xD01E] = 0x05;
			tests.check(m._state->read_vic(0xD01E) == 0x05, "VIC: $D01E returns the collision mask");
			tests.check(m._state->read_vic(0xD01E) == 0x00, "VIC: $D01E clears itself after a read");
			m._state->RAM[0xD01F] = 0x03;
			m._state->write_vic(0xD01F, 0xFF); // write must be ignored
			tests.check(m._state->read_vic(0xD01F) == 0x03, "VIC: $D01F is read-only");
		}

		// --- Raster counter read-back ($D011 bit 7 = MSB, $D012 = low byte) ---
		{
			machine m;
			m._state->vic = vic_chip{};
			m._state->write_vic(0xD011, 0x00);
			for (int i = 0; i < 260; ++i) m._state->tick_vic(63); // advance to raster line 260
			tests.check(m._state->read_vic(0xD012) == (260 & 0xFF), "VIC: $D012 returns the raster line low byte");
			tests.check((m._state->read_vic(0xD011) & 0x80) == 0x80, "VIC: $D011 bit 7 returns the raster line MSB");
		}

		// --- $D019 IRQ latch: bit 7 summarises enabled+pending, write acks ---
		{
			machine m;
			m._state->vic = vic_chip{};
			m._state->vic.raster_compare = 50;
			m._state->write_vic(0xD01A, 0x01); // enable raster IRQ
			for (int i = 0; i < 60; ++i) m._state->tick_vic(63);
			const uint8_t status = m._state->read_vic(0xD019);
			tests.check((status & 0x01) == 0x01, "VIC: $D019 bit 0 latches the raster IRQ");
			tests.check((status & 0x80) == 0x80, "VIC: $D019 bit 7 summarises an enabled IRQ");
			tests.check((status & 0x70) == 0x70, "VIC: $D019 unused bits 4-6 read as 1");
			m._state->write_vic(0xD019, 0x01); // acknowledge
			tests.check((m._state->read_vic(0xD019) & 0x81) == 0x00, "VIC: writing $D019 acknowledges the latch and clears bit 7");
		}
	}

	// CIA timer validation (inspired by VICE's CIA timer test programs): counting,
	// latch reload on underflow, ICR flags, one-shot vs continuous, force-load,
	// interrupt masking and the Timer-B-counts-Timer-A cascade. Driven directly
	// through tick_cia so the results are deterministic and CPU-independent.
	void run_cia_tests(test_runner& tests)
	{
		// Start Timer A from a known latch in continuous mode.
		auto arm_timer_a = [](machine& m, uint16_t latch, uint8_t cra)
		{
			m._state->cia1 = cia_chip{};
			m._state->irq_pending = false;
			m._state->write_cia1(0xDC04, static_cast<uint8_t>(latch & 0xFF));
			m._state->write_cia1(0xDC05, static_cast<uint8_t>(latch >> 8)); // loads counter (timer stopped)
			m._state->write_cia1(0xDC0E, cra);
		};

		// --- Counting down ---
		{
			machine m; arm_timer_a(m, 100, 0x01);
			m._state->tick_cia(m._state->cia1, 40, false);
			tests.check(m._state->read_cia1(0xDC04) == 60, "CIA: Timer A counts down with the system clock");
		}

		// --- Underflow reloads from the latch and sets the ICR flag ---
		{
			machine m; arm_timer_a(m, 10, 0x01);
			m._state->tick_cia(m._state->cia1, 11, false); // 10 - 11 underflows, +latch = 9
			tests.check(m._state->read_cia1(0xDC04) == 9, "CIA: Timer A reloads from the latch after underflow");
			tests.check((m._state->cia1.icr_data & 0x01) != 0, "CIA: Timer A underflow sets ICR bit 0");
		}

		// --- Reading the ICR returns the flags and clears them ---
		{
			machine m; arm_timer_a(m, 10, 0x01);
			m._state->tick_cia(m._state->cia1, 11, false);
			const uint8_t first = m._state->read_cia1(0xDC0D);
			const uint8_t second = m._state->read_cia1(0xDC0D);
			tests.check((first & 0x01) != 0, "CIA: reading $DC0D reports the Timer A flag");
			tests.check(second == 0x00, "CIA: reading $DC0D clears the pending flags");
		}

		// --- One-shot mode stops after a single underflow ---
		{
			machine m; arm_timer_a(m, 5, 0x09); // bit3 = one-shot, bit0 = start
			m._state->tick_cia(m._state->cia1, 5, false);
			tests.check(!m._state->cia1.timer_a_running, "CIA: one-shot Timer A stops after underflow");
			tests.check((m._state->cia1.cra & 0x01) == 0, "CIA: one-shot clears the start bit in $DC0E");
		}

		// --- Continuous mode keeps running across an underflow ---
		{
			machine m; arm_timer_a(m, 5, 0x01);
			m._state->tick_cia(m._state->cia1, 6, false);
			tests.check(m._state->cia1.timer_a_running, "CIA: continuous Timer A keeps running after underflow");
		}

		// --- Force-load (bit 4) reloads the counter from the latch immediately ---
		{
			machine m; arm_timer_a(m, 100, 0x01);
			m._state->tick_cia(m._state->cia1, 40, false);          // counter now 60
			m._state->write_cia1(0xDC0E, 0x11);                     // start + force-load
			tests.check(m._state->read_cia1(0xDC04) == 100, "CIA: writing $DC0E bit 4 force-loads the latch");
		}

		// --- Interrupt masking: enabled underflow raises the IRQ line ---
		{
			machine m; arm_timer_a(m, 5, 0x01);
			m._state->write_cia1(0xDC0D, 0x81); // set-mask + Timer A enable
			m._state->tick_cia(m._state->cia1, 6, false);
			tests.check((m._state->cia1.icr_data & 0x80) != 0, "CIA: enabled Timer A underflow sets ICR bit 7");
			tests.check(m._state->irq_pending, "CIA: enabled Timer A underflow raises the CPU IRQ line");
		}

		// --- Interrupt masking: a disabled timer flags but does not interrupt ---
		{
			machine m; arm_timer_a(m, 5, 0x01); // interrupts left disabled
			m._state->tick_cia(m._state->cia1, 6, false);
			tests.check((m._state->cia1.icr_data & 0x01) != 0, "CIA: disabled Timer A still latches its flag");
			tests.check((m._state->cia1.icr_data & 0x80) == 0 && !m._state->irq_pending, "CIA: disabled Timer A does not interrupt");
		}

		// --- Timer B cascade: CRB bits 5-6 = 01 counts Timer A underflows ---
		{
			machine m;
			m._state->cia1 = cia_chip{};
			m._state->irq_pending = false;
			m._state->write_cia1(0xDC06, 2); m._state->write_cia1(0xDC07, 0); // Timer B latch = 2
			m._state->write_cia1(0xDC04, 2); m._state->write_cia1(0xDC05, 0); // Timer A latch = 2
			m._state->write_cia1(0xDC0F, 0x41); // Timer B: count Timer A underflows + start
			m._state->write_cia1(0xDC0E, 0x01); // Timer A: start
			for (int i = 0; i < 3; ++i) m._state->tick_cia(m._state->cia1, 2, false); // 3 Timer A underflows
			tests.check((m._state->cia1.icr_data & 0x02) != 0, "CIA: Timer B underflows after counting Timer A underflows");
		}
	}

	// Screen code -> ASCII for dumping the text screen of a .prg test.
	char screen_char(const uint8_t code)
	{
		const uint8_t c = code & 0x7F;
		if (c >= 0x01 && c <= 0x1A) return static_cast<char>('A' + c - 1);
		if (c == 0x00) return '@';
		if (c >= 0x30 && c <= 0x3F) return static_cast<char>(c); // digits + punctuation
		if (c == 0x20) return ' ';
		return (c >= 0x20 && c <= 0x5F) ? static_cast<char>(c) : '.';
	}

	// Run a raw binary as a bare 6502 program (e.g. Klaus Dormann's 6502
	// functional test). Detects the success/failure infinite-loop trap.
	int run_bin(const std::filesystem::path& path, const uint16_t load, const uint16_t start,
		const bool has_expected, const uint16_t expected)
	{
		const auto bytes = read_file(path);
		if (bytes.empty())
		{
			std::cerr << "Unable to read binary: " << path.string() << '\n';
			return 2;
		}

		machine m;
		m._state->raw_ram = true;
		std::fill(std::begin(m._state->RAM), std::end(m._state->RAM), static_cast<uint8_t>(0));
		const size_t room = machine_state::RAM_SIZE - load;
		const size_t count = std::min(bytes.size(), room);
		std::copy_n(bytes.begin(), count, m._state->RAM + load);

		m._state->cpu.pc = start;
		m._state->cpu.sp = 0xFD;
		m._state->cpu.a = m._state->cpu.x = m._state->cpu.y = 0;
		m._state->cpu.status = FLAG_CONSTANT | FLAG_INTERRUPT;

		constexpr long long instruction_cap = 200'000'000;
		for (long long i = 0; i < instruction_cap; ++i)
		{
			const uint16_t before = m._state->cpu.pc;
			m.exec(1);
			if (m._state->cpu.pc == before)
			{
				std::cout << "Trapped at $" << std::hex << before << std::dec
					<< " after " << m._state->instruction_count << " instructions.\n";
				if (has_expected)
				{
					const bool ok = before == expected;
					std::cout << (ok ? "PASS" : "FAIL") << " (expected $"
						<< std::hex << expected << std::dec << ")\n";
					return ok ? 0 : 1;
				}
				return 0;
			}
		}
		std::cout << "No trap after " << instruction_cap << " instructions (PC=$"
			<< std::hex << m._state->cpu.pc << std::dec << ")\n";
		return 1;
	}

	// Load and run a .prg, then print the text screen (for eyeballing the result
	// of Lorenz / VICE single-file test programs).
	int run_prg_dump(const std::filesystem::path& path)
	{
		const auto bytes = read_file(path);
		if (bytes.size() < 3)
		{
			std::cerr << "Unable to read PRG: " << path.string() << '\n';
			return 2;
		}

		machine m;
		for (int c = 0; c < 60 && !screen_contains(m, "READY."); ++c) m.exec(execution_step);
		m.exec(3'000'000);
		if (!m.load_prg(bytes.data(), bytes.size()))
		{
			std::cerr << "Not a valid PRG.\n";
			return 2;
		}
		for (int i = 0; i < 400; ++i) m.exec(50'000); // ~20M cycles

		const auto* screen = m._state->RAM + text_video_mem_offset;
		std::cout << "+" << std::string(40, '-') << "+\n";
		for (int row = 0; row < 25; ++row)
		{
			std::cout << '|';
			for (int col = 0; col < 40; ++col) std::cout << screen_char(screen[row * 40 + col]);
			std::cout << "|\n";
		}
		std::cout << "+" << std::string(40, '-') << "+\n";
		return 0;
	}

	uint16_t parse_hex(const char* text)
	{
		return static_cast<uint16_t>(std::stoul(text, nullptr, 16));
	}

	// Assemble a 6502 source file and print the resulting machine code as a hex
	// dump (or the error with its line number). Drives src/assembler.cpp.
	int run_asm(const std::filesystem::path& path)
	{
		const auto bytes = read_file(path);
		if (bytes.empty())
		{
			std::cerr << "Unable to read source file: " << path.string() << '\n';
			return 1;
		}
		const std::string source(bytes.begin(), bytes.end());
		const auto result = assemble(source);
		if (!result.ok)
		{
			std::cerr << path.filename().string() << ':' << result.error_line
				<< ": error: " << result.error << '\n';
			return 1;
		}

		std::cout << "Assembled " << result.bytes.size() << " byte(s) at $"
			<< std::hex << std::uppercase << std::setw(4) << std::setfill('0') << result.origin << ":\n";
		for (size_t i = 0; i < result.bytes.size(); i += 16)
		{
			std::cout << '$' << std::setw(4) << std::setfill('0')
				<< (result.origin + static_cast<unsigned>(i)) << "  ";
			for (size_t j = 0; j < 16 && i + j < result.bytes.size(); ++j)
				std::cout << std::setw(2) << std::setfill('0')
					<< static_cast<unsigned>(result.bytes[i + j]) << ' ';
			std::cout << '\n';
		}
		std::cout << std::dec << std::nouppercase;
		return 0;
	}
}

int main(const int argc, char** argv)
{
	if (argc == 3 && std::string_view(argv[1]) == "--list-disk")
		return list_disk(argv[2]) ? 0 : 1;

	// Run a raw binary as a bare 6502 (e.g. Klaus Dormann's 6502 functional
	// test): --run-bin <file> <loadHex> <startHex> [expectedTrapHex]
	if (argc >= 5 && std::string_view(argv[1]) == "--run-bin")
	{
		const auto load = parse_hex(argv[3]);
		const auto start = parse_hex(argv[4]);
		const bool has_expected = argc >= 6;
		const auto expected = has_expected ? parse_hex(argv[5]) : uint16_t{0};
		return run_bin(argv[2], load, start, has_expected, expected);
	}

	// Load a .prg and print the resulting text screen (Lorenz / VICE tests):
	// --run-prg <file.prg>
	if (argc == 3 && std::string_view(argv[1]) == "--run-prg")
		return run_prg_dump(argv[2]);

	// Assemble a 6502 source file and print the machine code: --asm <file.asm>
	if (argc == 3 && std::string_view(argv[1]) == "--asm")
		return run_asm(argv[2]);

	if (argc != 1)
	{
		std::cerr << "Usage: lib8bit-tests\n"
			<< "       lib8bit-tests --list-disk <image.d64|.d71|.d81>\n"
			<< "       lib8bit-tests --run-bin <file> <loadHex> <startHex> [expectedTrapHex]\n"
			<< "       lib8bit-tests --run-prg <file.prg>\n"
			<< "       lib8bit-tests --asm <file.asm>\n";
		return 2;
	}

	test_runner tests;
	const auto fixture_folder = std::filesystem::path(__FILE__).parent_path();

	// Headless 6502 CPU validation (opcodes, flags, addressing, cycle timing).
	run_cpu_tests(tests);

	// VIC-II graphics chip validation (modes, sprites, collision, raster IRQ).
	run_vic_tests(tests);

	// CIA timer validation (counting, reload, ICR, one-shot, cascade, IRQ).
	run_cia_tests(tests);

	// Built-in 6502 assembler validation (encoding, directives, round-trip).
	run_assembler_tests(tests);

	machine booted;
	tests.check(run_until_screen_contains(booted, "READY.", boot_cycle_limit), "boots to READY prompt");

	// The full cold-start screen must show the BASIC banner and free-memory line,
	// not just the READY prompt.
	tests.check(screen_contains(booted, "COMMODORE 64 BASIC V2"), "shows BASIC startup banner");
	tests.check(screen_contains(booted, "38911 BASIC BYTES FREE"), "shows free memory message");

	// Regression: driving the periodic IRQ the way the GUI app does must not derail
	// the kernal (a mis-vectored IRQ used to unbalance the stack and wipe the
	// screen down to a bare READY prompt).
	run_app_style_ticks(booted, 200);
	tests.check(screen_contains(booted, "COMMODORE 64 BASIC V2") && screen_contains(booted, "READY."),
		"keeps full screen after periodic IRQs");

	type_line(booted, "PRINT 6*7");
	tests.check(run_until_screen_contains(booted, "42", command_cycle_limit), "executes interactive BASIC");

	// Function keys inject their PETSCII codes into the keyboard buffer exactly
	// like normal keys; a running program reads them with GET. (C64 BASIC does
	// nothing visible with them at the direct-mode READY prompt.)
	{
		machine fk;
		tests.check(run_until_screen_contains(fk, "READY.", boot_cycle_limit), "boots for function-key test");
		type_line(fk, "10 GET A$:IF A$=\"\" THEN 10");
		type_line(fk, "20 PRINT ASC(A$)");
		type_line(fk, "RUN");
		fk.exec(1'000'000); // process RUN and settle into the GET loop
		fk.add_char(133);   // F1 -> PETSCII 133
		tests.check(run_until_screen_contains(fk, "133", command_cycle_limit),
			"function key F1 reaches a running program");
	}

	// Keyboard matrix: holding a key in the CIA1 matrix must be picked up by the
	// kernal's IRQ-driven keyboard scan and typed. 'Q' is at row 7, column 6 and
	// does not appear in the boot banner.
	{
		machine kb;
		tests.check(run_until_screen_contains(kb, "READY.", boot_cycle_limit), "boots for keyboard-matrix test");
		kb.set_key(7, 6, true); // press Q
		for (int i = 0; i < 8; ++i)
			kb.exec(20'000);    // hold across several keyboard scans
		kb.set_key(7, 6, false); // release
		tests.check(run_until_screen_contains(kb, "Q", command_cycle_limit),
			"keyboard matrix types through the kernal scan");
	}

	// VIC-II frame buffer rendering: boot with rendering on and confirm the
	// renderer produces a real picture (border around a background screen with
	// text pixels), not a blank buffer.
	{
		machine video;
		video.set_render_enabled(true);
		const bool booted_video = run_until_screen_contains(video, "READY.", boot_cycle_limit);
		video.exec(60'000); // let a couple of frames of scanlines render
		tests.check(booted_video, "boots with video rendering enabled");

		const auto* fb = video.framebuffer();
		const auto width = machine::framebuffer_width();
		const auto height = machine::framebuffer_height();
		const auto border = vic_palette[video._state->RAM[0xD020] & 0x0F];
		const auto background = vic_palette[video._state->RAM[0xD021] & 0x0F];

		constexpr int border_x = (384 - 320) / 2;
		constexpr int border_y = (272 - 200) / 2;

		bool has_background = false;
		bool has_text = false;
		for (int y = border_y; y < border_y + 200; ++y)
		{
			for (int x = border_x; x < border_x + 320; ++x)
			{
				const auto pixel = fb[y * width + x];
				if (pixel == background) has_background = true;
				else has_text = true;
			}
		}

		tests.check(fb[0] == border, "renders the border colour");
		tests.check(has_background, "renders the screen background");
		tests.check(has_text, "renders text pixels over the background");
		(void)height;
	}

	// Sprite collision registers ($D01E sprite-sprite, $D01F sprite-background)
	// must clear when read; otherwise a stale collision persists every frame and
	// games (e.g. Pitfall) think the player keeps colliding.
	{
		machine collide;
		collide._state->RAM[0xD01E] = 0x03;
		collide._state->RAM[0xD01F] = 0x81;
		const auto ss_first = collide._state->ram_read(0xD01E);
		const auto ss_again = collide._state->ram_read(0xD01E);
		const auto sb_first = collide._state->ram_read(0xD01F);
		const auto sb_again = collide._state->ram_read(0xD01F);
		tests.check(ss_first == 0x03 && ss_again == 0x00, "sprite-sprite collision clears on read");
		tests.check(sb_first == 0x81 && sb_again == 0x00, "sprite-background collision clears on read");
	}

	machine prg_machine;
	tests.check(run_until_screen_contains(prg_machine, "READY.", boot_cycle_limit), "boots before loading PRG");
	const auto prg = read_file(fixture_folder / "example.prg");
	const auto loaded = prg_machine.load_prg(prg.data(), prg.size());
	tests.check(loaded, "loads example.prg");
	tests.check(run_until_screen_contains(prg_machine, "HELLO WORLD", command_cycle_limit), "runs example.prg");

	for (const auto name : {"gng_bl.crt", "pitfall.crt", "river_raid.crt"})
	{
		const auto crt = read_file(fixture_folder / name);
		const auto info = inspect_crt(crt);
		tests.check(info.valid, std::string("parses CRT fixture: ") + name);
		if (info.valid)
			std::cout << "     type=" << info.hardware_type << ", chips=" << info.chip_count
				<< ", ROM bytes=" << info.rom_size << '\n';
	}

	// Load the normal (type 0) cartridges into a real machine and confirm the
	// cartridge ROM actually appears in the CPU address space via banking.
	for (const auto name : {"pitfall.crt", "river_raid.crt"})
	{
		const auto crt = read_file(fixture_folder / name);
		machine cart_machine;
		const bool loaded = cart_machine.load_crt(crt.data(), crt.size());
		tests.check(loaded, std::string("loads cartridge into machine: ") + name);
		if (loaded)
		{
			const auto header_len = read_big_endian_32(crt, 0x10);
			const auto load_addr = read_big_endian_16(crt, header_len + 12);
			const auto* rom = crt.data() + header_len + 16;
			bool maps = true;
			for (int i = 0; i < 16; ++i)
			{
				if (cart_machine._state->ram_read(static_cast<uint16_t>(load_addr + i)) != rom[i])
				{
					maps = false;
					break;
				}
			}
			tests.check(maps, std::string("maps cartridge ROM into memory: ") + name);
		}
	}

	// SID audio: boot, program voice 1 with a gated sawtooth at full volume and
	// confirm the SID pipeline renders non-silent 16-bit PCM.
	{
		machine audio;
		run_until_screen_contains(audio, "READY.", boot_cycle_limit);
		audio.set_audio_enabled(true); // sync the audio clock to the post-boot cycle

		auto& sid = audio._state->sid;
		sid.write_now(0x18, 0x0F); // master volume max, no filter
		sid.write_now(0x00, 0x00); // voice 1 frequency low
		sid.write_now(0x01, 0x20); // voice 1 frequency high
		sid.write_now(0x05, 0x00); // attack=0, decay=0
		sid.write_now(0x06, 0xF0); // sustain=15, release=0
		sid.write_now(0x04, 0x21); // sawtooth waveform + gate on

		audio.exec(20'000); // ~one frame of cycles

		int16_t buffer[2048];
		const int samples = audio.generate_audio(buffer, 2048);
		bool non_silent = false;
		for (int i = 0; i < samples; ++i)
			if (buffer[i] != 0) { non_silent = true; break; }

		tests.check(samples > 0, "SID renders audio samples");
		tests.check(non_silent, "SID output is non-silent");
	}

	// SID tune playback: load a real .sid file, run the installed player driver
	// for a few frames and confirm the play routine drives the SID to audible
	// output.
	{
		const auto tune_bytes = read_file(fixture_folder / "cybernoid.sid");
		machine tune;
		const bool loaded = tune.load_sid(tune_bytes.data(), tune_bytes.size());
		tests.check(loaded, "loads cybernoid.sid tune");

		bool non_silent = false;
		int16_t buffer[2048];
		for (int frame = 0; frame < 100 && !non_silent; ++frame)
		{
			tune.exec(20'000);
			const int samples = tune.generate_audio(buffer, 2048);
			for (int i = 0; i < samples; ++i)
				if (buffer[i] != 0) { non_silent = true; break; }
		}
		tests.check(non_silent, "SID tune drives audible output");
	}

	disk_directory disk;
	const auto listed = list_disk(fixture_folder / "1942.d64", &disk);
	tests.check(listed, "lists 1942.d64 directory");
	tests.check(disk.format == disk_image_format::d64, "detects D64 format");
	tests.check(disk.name == "1986 ELITE", "reads D64 disk name");
	tests.check(has_entry_starting_with(disk, "1942"), "finds 1942 program on disk");
	tests.check(recognizes_empty_disk(349696, 91392, disk_image_format::d71), "detects D71 format");
	tests.check(recognizes_empty_disk(819200, 399360, disk_image_format::d81), "detects D81 format");

	// Extract the first program off the disk (as LOAD"*",8 would) and by name.
	{
		const auto d64 = read_file(fixture_folder / "1942.d64");
		std::vector<uint8_t> first;
		const bool got_first = read_disk_file(d64.data(), d64.size(), "*", first);
		tests.check(got_first && first.size() > 2, "extracts first program from 1942.d64");

		std::vector<uint8_t> by_name;
		read_disk_file(d64.data(), d64.size(), "1942*", by_name);
		tests.check(!by_name.empty() && by_name == first, "extracts disk file by name pattern");
	}

	if (tests.failures != 0)
		std::cout << tests.failures << " test(s) failed\n";
	return tests.failures == 0 ? 0 : 1;
}
