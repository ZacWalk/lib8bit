#pragma once

// lib8bit by Zac Walker
//
// Core C64 machine model: memory banking through $01, the 6502 memory bus,
// CIA timers, keyboard matrix and joystick, plus the top-level machine class
// that ties together the CPU, VIC-II, SID, cartridge and disk subsystems.

#include <cstdint>
#include <cstring>
#include <vector>

#include "cartridge.h"
#include "sid.h"
#include "vic.h"

extern uint8_t rom_kernal[8192];
extern uint8_t rom_basic[8192];
extern uint8_t rom_chars[];

struct debug_state;

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

enum class machine_key
{
	back,
	up,
	down,
	left,
	right,
};

struct CPUSTATUS
{
	uint16_t pc;
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

// A MOS 6526 (CIA) timer/interrupt block. Both CIA1 (IRQ source) and CIA2 (NMI
// source) share this state; the only difference is which CPU interrupt line an
// underflow drives, handled by the owner in step_hardware.
struct cia_chip
{
	// Timer A
	uint16_t timer_a_latch = 0xFFFF;
	int32_t timer_a_counter = 0xFFFF; // signed so underflow (<= 0) is detectable
	bool timer_a_running = false;
	bool timer_a_int_enabled = false; // ICR bit 0 (IRQ for CIA1, NMI for CIA2)
	// Timer B
	uint16_t timer_b_latch = 0xFFFF;
	int32_t timer_b_counter = 0xFFFF;
	bool timer_b_running = false;
	bool timer_b_int_enabled = false; // ICR bit 1
	// Control registers (force-load strobe bit 4 is not stored)
	uint8_t cra = 0;
	uint8_t crb = 0;
	// Interrupt state
	uint8_t icr_data = 0; // interrupt flags; bit 7 = "an enabled interrupt fired"
	uint8_t icr_mask = 0; // interrupt enable mask
	bool nmi_line = true; // CIA2 only: NMI line level (true = high/inactive)
};

// VIC-II raster / interrupt state. Full framebuffer rendering lives in vic.cpp;
// this block only tracks the raster position and raster-compare interrupt so the
// CPU sees accurate $D011/$D012/$D019/$D01A behaviour.
struct vic_chip
{
	uint16_t raster_compare = 0; // raster line that raises the raster IRQ
	uint8_t irq_enable = 0;      // $D01A interrupt enable mask
	uint8_t irq_status = 0;      // $D019 interrupt status (bit 7 = active)
	int32_t raster_line = 0;     // current raster line (0 .. RASTER_LINES_PER_FRAME-1)
	int32_t line_cycle = 0;      // cycle position within the current raster line
	int32_t last_raster_line = -1;
};

// The on-screen front end for a playing PSID/RSID tune: the tune's metadata,
// the sub-tune list and the level meters are painted into the emulated text
// screen once per exec(), and the number keys and X are read straight from the
// keyboard matrix. Everything here is host independent — a host only has to
// feed keys and show the frame buffer, exactly as for any other program.
struct sid_player_state
{
	bool active = false;
	std::vector<uint8_t> file; // the tune image, so another sub-tune can be started
	char title[33] = {};
	char author[33] = {};
	char released[33] = {};
	bool chip_8580 = false;    // the SID model the header asks for
	bool timed_by_cia = false; // play routine driven by a CIA timer, not the raster
	int songs = 1;
	int song = 1;             // 1-based, the sub-tune now playing
	uint16_t screen_base = 0x0400;
	int64_t start_ticks = 0;  // clock_ticks when this sub-tune started
	uint32_t clock_hz = 985248;
	uint8_t prev_keys[8] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
	uint8_t peak[3] = {};     // decaying peak-hold marker per voice
};

struct machine_state
{
	static constexpr size_t RAM_SIZE = 1024 * 64;

	//6502 CPU registers
	CPUSTATUS cpu;

	//helper variables
	int64_t instruction_count = 0; //keep track of total instructions executed
	int64_t clock_ticks = 0;
	uint8_t penalty_addr = 0; // set by indexed addressing when a page is crossed
	// When true the machine behaves as a bare 6502 + flat 64K RAM (no banking,
	// no I/O, no processor port at $00/$01). Used for headless CPU test vectors
	// such as Klaus Dormann's 6502 functional test.
	bool raw_ram = false;

	uint8_t RAM[RAM_SIZE];

	// Snapshot of the previous frame's text screen + colour RAM, used to skip
	// expensive WM_PAINT redraws when nothing visible has changed.
	// Layout: [0..999] = screen codes, [1000..1999] = colour RAM nybbles,
	// [2000] = border colour, [2001] = background colour.
	uint8_t video_snapshot[2002];

	// --- Memory banking (driven by the processor port at $0001) -------------
	// Per-page (256-byte) source tables. read_map: 0=RAM, 1=BASIC, 2=KERNAL,
	// 3=CHAR ROM, 4=I/O, 5=cart ROML, 6=cart ROMH. write_map: 0=RAM, 1=I/O.
	uint8_t read_map[256] = {};
	uint8_t write_map[256] = {};
	bool is_basic_on = true;
	bool is_kernal_on = true;
	bool is_io_on = true;
	bool is_char_on = false;
	// Configuration the page tables were last built for; update_memory_map() is on
	// hot paths (every $00/$01 and $DE00-$DFFF access) so it early-outs on a match.
	uint32_t memory_map_key = 0xFFFFFFFF;

	// --- CPU interrupt lines ------------------------------------------------
	bool irq_pending = false; // level-triggered: asserted while any IRQ source active
	bool nmi_pending = false; // edge-triggered: set on the high->low NMI transition
	bool ext_irq = false;     // host-raised IRQ (machine::irq), cleared when taken

	// --- Chips --------------------------------------------------------------
	cia_chip cia1; // keyboard / joystick / system 60 Hz IRQ timer
	cia_chip cia2; // VIC bank select / NMI timer
	vic_chip vic;  // raster + raster interrupt
	sid_chip sid;  // MOS6581/8580 sound chip
	cartridge cart; // optional .CRT cartridge (ROML/ROMH + bank switching)
	// The SID registers are write-only, so keep the last value written to each
	// for monitors to display.
	uint8_t sid_regs[32] = {};

	// --- Disk drive (device 8) ----------------------------------------------
	// A mounted .d64/.d71/.d81 image, empty when the drive is empty. It survives
	// reset, exactly as a disk left in a real drive does.
	std::vector<uint8_t> disk;

	// --- SID tune front end -------------------------------------------------
	// Only meaningful while a .sid tune is playing; reset() clears it.
	sid_player_state sid_player;

	// --- Input --------------------------------------------------------------
	uint8_t joystick1 = 0xFF; // CIA1 Port B ($DC01), bits active low
	uint8_t joystick2 = 0xFF; // CIA1 Port A ($DC00), bits active low
	// C64 keyboard matrix: 8 rows (PA0-PA7) x 8 columns (PB0-PB7). A pressed key
	// clears its column bit (active low); 0xFF rows = nothing pressed. The kernal
	// (and games) scan it through CIA1 Port A/Port B.
	uint8_t key_matrix[8] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

	// --- Video output -------------------------------------------------------
	// Packed-RGB frame buffer filled by the VIC-II renderer, one raster line at
	// a time as the CPU runs (only when render_enabled). The app blits this.
	uint32_t framebuffer[VIC_FB_WIDTH * VIC_FB_HEIGHT] = {};
	int8_t sprite_collision[VIC_FB_WIDTH] = {}; // scratch buffer for sprite collisions
	uint8_t fg_mask[VIC_FB_WIDTH] = {};         // per-scanline foreground flags (sprite-bg collision)
	bool render_enabled = false;

	// --- Audio output -------------------------------------------------------
	// When enabled, SID register writes are timestamped so generate_audio() can
	// replay them at cycle accuracy; when disabled they apply immediately and no
	// audio is produced (keeping exec() cheap for headless/text use).
	bool audio_enabled = false;

	machine_state()
	{
		std::memset(RAM, 0, RAM_SIZE);
		std::memset(video_snapshot, 0xFF, sizeof(video_snapshot));
		reset();
	}

	// Memory bus (implements C64 banking + I/O dispatch); defined in machine.cpp.
	uint8_t ram_read(uint16_t address);
	void ram_write(uint16_t address, uint8_t value);

	void reset();
	void update_memory_map();

	// I/O register handlers.
	uint8_t read_vic(uint16_t address);
	void write_vic(uint16_t address, uint8_t value);
	uint8_t read_cia1(uint16_t address);
	void write_cia1(uint16_t address, uint8_t value);
	uint8_t read_cia2(uint16_t address);
	void write_cia2(uint16_t address, uint8_t value);
	uint8_t sid_read(uint16_t reg);
	void sid_write(uint16_t reg, uint8_t value);

	// Interrupt line aggregation.
	void update_vic_irq();
	void update_irq_line();
	void update_nmi();

	// Per-instruction hardware clocking (advances CIA timers + VIC raster).
	void tick_cia(cia_chip& c, int32_t cycles, bool is_cia2);
	void tick_vic(int32_t cycles);
	void step_hardware(int32_t cycles);

	// Service the KERNAL LOAD routine from the mounted disk image and return
	// through a simulated RTS. Returns false when the request is not ours (no
	// disk, not device 8, a verify, or no filename), leaving the ROM to run.
	bool kernal_load();
};

class machine
{
public:
	machine_state* _state = nullptr;

	machine();
	~machine();

	// machine owns _state; copying would double-free it.
	machine(const machine&) = delete;
	machine& operator=(const machine&) = delete;

	uint8_t convert_char(wchar_t c);
	void add_char(wchar_t c);
	void add_key(machine_key key);
	void exec(int32_t tick_count = 1000000 / 50);

	// Raise an externally driven IRQ. Serviced at the next instruction boundary,
	// so its 7-cycle entry is charged like any other interrupt.
	void irq();

	// Load a Commodore .PRG file into RAM.
	//   bytes[0..1] = little-endian load address
	//   bytes[2..]  = program data
	// If load address is $0801 the BASIC end-of-program pointers are updated
	// and "RUN\n" is queued so the program autostarts. Returns false on bad data.
	bool load_prg(const uint8_t* data, size_t size);

	bool video_is_invalid() const;

	// Fill `out` with a side-effect-free snapshot of the CPU, banking, VIC-II,
	// CIA and SID state plus a disassembly window around the PC (see debug.h).
	void capture_debug(debug_state& out) const;

	// --- VIC-II frame buffer -------------------------------------------------
	// Enable per-scanline rendering into the frame buffer during exec(). Hosts
	// that want a full graphical display (all VIC modes + sprites) turn this on;
	// leaving it off keeps exec() cheap for headless/text use.
	void set_render_enabled(bool enabled);
	const uint32_t* framebuffer() const; // VIC_FB_WIDTH x VIC_FB_HEIGHT, packed 0x00RRGGBB
	static constexpr int framebuffer_width() { return VIC_FB_WIDTH; }
	static constexpr int framebuffer_height() { return VIC_FB_HEIGHT; }

	// --- Input ---------------------------------------------------------------
	// Set a joystick port (1 or 2). Directions/fire are pressed booleans.
	void set_joystick(int port, bool up, bool down, bool left, bool right, bool fire);
	void press_stop(bool pressed); // RUN/STOP key state (for BREAK detection)

	// Set/clear a key in the C64 keyboard matrix (row 0-7 = PA line, col 0-7 =
	// PB line). Held keys are seen by the kernal keyboard scan and by games that
	// read the CIA1 matrix directly.
	void set_key(int row, int col, bool pressed);
	void clear_keys(); // release every key (e.g. on window focus loss)

	// --- Cartridges ----------------------------------------------------------
	// Load a .CRT cartridge image and cold-reset the machine so it boots from
	// the cartridge (normal carts autostart via the kernal; Ultimax carts take
	// over the reset vector). Returns false on a malformed image.
	bool load_crt(const uint8_t* data, size_t size);
	void eject_crt();

	// --- Disk drive ----------------------------------------------------------
	// Insert a .d64/.d71/.d81 image into device 8. The image stays mounted, so
	// the KERNAL LOAD routine is serviced from it: a program can LOAD further
	// files while it runs, and LOAD"$",8 lists the directory. The bytes are
	// copied, so the caller's buffer need not outlive the call. Returns false if
	// the image is not a recognised disk format.
	bool insert_disk(const uint8_t* data, size_t size);
	void eject_disk();
	bool has_disk() const;

	// Load and start a PSID/RSID tune. Parses the header, installs a small
	// player driver in RAM, calls the tune's init routine and arms the CIA/VIC
	// interrupt so the play routine runs each frame. Enables audio. Pass a
	// 1-based song number or 0 to use the file's default. Returns false on a
	// malformed image.
	//
	// A text front end showing the tune's metadata, its sub-tunes and live voice
	// levels is painted into the emulated screen by exec(), which also reads the
	// keyboard matrix: keys 1-9 pick a sub-tune and X stops playback.
	bool load_sid(const uint8_t* data, size_t size, int song = 0);
	bool sid_player_active() const;
	int sid_song_count() const;
	int sid_song() const; // 1-based, 0 when no tune is playing
	// Start another sub-tune of the loaded tune (1-based). False if none is loaded
	// or the number is out of range.
	bool sid_select_song(int song);
	// Stop playback and return the machine to a bare reset.
	void stop_sid();

	// --- Audio ---------------------------------------------------------------
	// Enable SID audio generation (off by default). Selects the chip model too.
	void set_audio_enabled(bool enabled);
	void set_sid_model(sid_model model);
	// Produce up to max_samples of mono 16-bit PCM for the cycles executed since
	// the previous call, replaying SID register writes at cycle accuracy. Call
	// once per frame after exec(). Returns the number of samples written.
	int generate_audio(int16_t* buffer, int max_samples);
};
