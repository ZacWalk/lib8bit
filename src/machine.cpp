// C64 Emulator
// http://forum.arduino.cc/index.php?topic=193216.msg1793065#msg1793065

#include <cstdint>
#include <cstdio>
#include <memory>
#include <locale>

#include "cpu.h"
#include "machine.h"

#include "platform.h"


uint8_t machine::convert_char(const wchar_t c)
{
	if (c == '\"') return 34;
	if (c == pf::platform_key::Back) return 20;
	if (c == pf::platform_key::Up) return 145;
	if (c == pf::platform_key::Down) return 17;
	if (c == pf::platform_key::Left) return 157;
	if (c == pf::platform_key::Right) return 29;
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
		const char run[] = {'R', 'U', 'N', 13};
		for (int i = 0; i < 4; i++)
			_state->RAM[mem_keyboard_buffer + i] = static_cast<uint8_t>(run[i]);
		_state->RAM[mem_length_of_keyboard_buffer] = 4;
	}

	return true;
}
