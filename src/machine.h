#pragma once

#include <cstring>

extern uint8_t rom_kernal[8192];
extern uint8_t rom_basic[8192];
extern uint8_t rom_chars[];

constexpr uint32_t rgb(const uint32_t r, const uint32_t g, const uint32_t b)
{
	return r << 16 | g << 8 | b;
}

constexpr uint32_t bgr(const uint32_t rgb)
{
	return ((rgb & 0xff0000) >> 16) | (rgb & 0x00ff00) | ((rgb & 0x0000ff) << 16);
}

constexpr uint32_t c64_black = rgb(0, 0, 0);
constexpr uint32_t c64_white = rgb(253, 254, 252);
constexpr uint32_t c64_red = rgb(190, 26, 36);
constexpr uint32_t c64_cyan = rgb(48, 230, 198);
constexpr uint32_t c64_purple = rgb(180, 26, 226);
constexpr uint32_t c64_green = rgb(31, 162, 30);
constexpr uint32_t c64_blue = rgb(33, 27, 174);
constexpr uint32_t c64_yellow = rgb(223, 246, 10);
constexpr uint32_t c64_orange = rgb(184, 65, 4);
constexpr uint32_t c64_brown = rgb(106, 51, 4);
constexpr uint32_t c64_light_red = rgb(254, 74, 87);
constexpr uint32_t c64_dark_grey = rgb(66, 69, 64);
constexpr uint32_t c64_grey = rgb(112, 116, 111);
constexpr uint32_t c64_light_green = rgb(89, 254, 89);
constexpr uint32_t c64_light_blue = rgb(95, 83, 254);
constexpr uint32_t c64_light_grey = rgb(164, 167, 162);

constexpr uint32_t c64_pallette[] =
{
	c64_black,
	c64_white,
	c64_red,
	c64_cyan,
	c64_purple,
	c64_green,
	c64_blue,
	c64_yellow,
	c64_orange,
	c64_brown,
	c64_light_red,
	c64_dark_grey,
	c64_grey,
	c64_light_green,
	c64_light_blue,
	c64_light_grey,
};

constexpr uint8_t FLAG_CARRY = 0x01;
constexpr uint8_t FLAG_ZERO = 0x02;
constexpr uint8_t FLAG_INTERRUPT = 0x04;
constexpr uint8_t FLAG_DECIMAL = 0x08;
constexpr uint8_t FLAG_BREAK = 0x10;
constexpr uint8_t FLAG_CONSTANT = 0x20;
constexpr uint8_t FLAG_OVERFLOW = 0x40;
constexpr uint8_t FLAG_SIGN = 0x80;


// Memory map https://sta.c64.org/cbm64mem.html
// Kernal disasm https://www.pagetable.com/c64ref/c64disasm/

constexpr uint32_t mem_length_of_keyboard_buffer = 0x00C6; // 0: Buffer is empty.1-10: Buffer length.
constexpr uint32_t mem_keyboard_buffer = 0x0277; // Keyboard buffer (10 bytes)
constexpr uint32_t mem_center_color = 0xd021;
constexpr uint32_t mem_border_color = 0xd020;
constexpr uint32_t mem_text_color = 0xd800;
constexpr uint32_t text_video_mem_offset = 1024;

using address_t = unsigned short;

struct CPUSTATUS
{
	address_t pc;
	uint8_t sp;
	uint8_t a;
	uint8_t x;
	uint8_t y;
	uint8_t status;


	void set_carry() { status |= FLAG_CARRY; }
	void clear_carry() { status &= (~FLAG_CARRY); }
	void set_zero() { status |= FLAG_ZERO; }
	void clear_zero() { status &= (~FLAG_ZERO); }
	void set_interrupt() { status |= FLAG_INTERRUPT; }
	void clear_interrupt() { status &= (~FLAG_INTERRUPT); }
	void set_decimal() { status |= FLAG_DECIMAL; }
	void clear_decimal() { status &= (~FLAG_DECIMAL); }
	void set_overflow() { status |= FLAG_OVERFLOW; }
	void clear_overflow() { status &= (~FLAG_OVERFLOW); }
	void set_sign() { status |= FLAG_SIGN; }
	void clear_sign() { status &= (~FLAG_SIGN); }


	void calc_zero(const uint16_t n)
	{
		if ((n) & 0x00FF) clear_zero();
		else set_zero();
	}

	void calc_sign(const uint16_t n)
	{
		if ((n) & 0x0080) set_sign();
		else clear_sign();
	}

	void calc_carry(const uint16_t n)
	{
		if ((n) & 0xFF00) set_carry();
		else clear_carry();
	}

	void calc_overflow(const uint16_t n, const uint16_t m, const uint16_t o)
	{
		if (((n) ^ static_cast<uint16_t>(m)) & ((n) ^ (o)) & 0x0080) set_overflow();
		else clear_overflow();
	}
};

struct machine_state
{
	static constexpr size_t RAM_SIZE = 1024 * 64;

	//6502 CPU registers
	CPUSTATUS cpu;

	//helper variables
	int64_t instruction_count = 0; //keep track of total instructions executed
	int64_t clock_ticks = 0;

	uint8_t RAM[RAM_SIZE];

	// Snapshot of the previous frame's text screen + colour RAM, used to skip
	// expensive WM_PAINT redraws when nothing visible has changed.
	// Layout: [0..999] = screen codes, [1000..1999] = colour RAM nybbles,
	// [2000] = border colour, [2001] = background colour.
	uint8_t video_snapshot[2002];

	machine_state()
	{
		std::memset(RAM, 0, RAM_SIZE);
		std::memset(video_snapshot, 0xFF, sizeof(video_snapshot));
	}

	uint8_t ram_read(uint16_t address)
	{
		if ((address >= 0xA000) && (address < 0xC000)) return rom_basic[address - 0xA000]; //BASIC ROM
		if (address >= 0xE000) return rom_kernal[address - 0xE000]; //KERNAL ROM
		if ((address >= 0xD000) && (address < 0xD800)) return 0; //VIC2 and SID
		return RAM[address];
	}

	void ram_write(const uint16_t address, const uint8_t value)
	{
		RAM[address] = value;
	}

	void reset()
	{
		cpu.pc = static_cast<uint16_t>(ram_read(0xFFFC)) | (static_cast<uint16_t>(ram_read(0xFFFD)) << 8);
		cpu.a = 0;
		cpu.x = 0;
		cpu.y = 0;
		cpu.sp = 0xFD;
		cpu.status |= FLAG_CONSTANT;
	}
};

class machine
{
public:
	machine_state* _state = nullptr;

	machine();
	~machine();

	uint8_t convert_char(wchar_t c);
	void add_char(wchar_t c);
	void exec(int32_t tick_count = 1000000 / 50);
	void irq();

	// Load a Commodore .PRG file into RAM.
	//   bytes[0..1] = little-endian load address
	//   bytes[2..]  = program data
	// If load address is $0801 the BASIC end-of-program pointers are updated
	// and "RUN\n" is queued so the program autostarts. Returns false on bad data.
	bool load_prg(const uint8_t* data, size_t size);

	bool video_is_invalid() const;
};
