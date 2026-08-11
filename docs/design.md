# lib8bit design

How the emulator is put together and why. For what is *not* built yet, see
[todo.md](todo.md). For build and usage, see the [README](../README.md).

## Shape

Two pieces, with a hard boundary between them:

| | |
|---|---|
| `src/` | The emulator. Platform independent: no Windows headers, no file system, no threads. Builds `lib8bit.lib`. |
| `app/` | A Win32 host. Owns the window, GDI, waveOut, menus and the settings file. |
| `test/` | A console harness that drives the library headlessly. |

The library takes bytes and returns bytes. The host reads files, the library
parses buffers; the library fills a frame buffer and a PCM buffer, the host
presents them. Nothing in `src/` knows what a file or a window is.

`machine` is the embeddable façade; `machine_state` is the whole machine as
plain data. `machine` owns exactly one `machine_state` and is non-copyable.

## Execution model

Everything is driven from `cpu_exec(machine_state*, cycles)`. One pass of its
loop is one instruction, and the chips are advanced by exactly the cycles that
instruction took:

```
while (budget remains)
    service NMI / IRQ           (7 cycles, at the instruction boundary)
    KERNAL LOAD trap            (see "Disk" below)
    fetch opcode, execute
    clock_ticks += ticktable[opcode]  (+1 if a page cross earns a penalty)
    step_hardware(cycles just spent) -> tick CIA1, CIA2, VIC
```

There is no scheduler and no event queue. The cost is that a chip can only
observe time at instruction granularity; the benefit is that the whole machine
is one function call with no allocation, which is what keeps it embeddable.

`clock_ticks` is the single clock. Audio, raster position and CIA timers are all
expressed against it, so they cannot drift apart.

A PAL frame is 63 cycles × 312 raster lines = 19656 cycles. Hosts call
`exec(19656)` once per frame.

## Memory bus and banking

`ram_read` / `ram_write` are the only way into memory, and both are a table
lookup on the high byte of the address:

- `read_map[256]` — `0` RAM, `1` BASIC, `2` KERNAL, `3` character ROM, `4` I/O,
  `5` cartridge ROML, `6` cartridge ROMH.
- `write_map[256]` — `0` RAM, `1` I/O. Writes always reach the RAM under a ROM,
  which is why only the `$D0-$DF` pages are ever `1`.

`update_memory_map()` rebuilds both tables from the processor port at `$00`/`$01`
(LORAM/HIRAM/CHAREN, honouring the data direction register) plus the cartridge's
`/EXROM` and `/GAME` lines. It is called on every write to `$00`/`$01` and on
every `$DE00-$DFFF` access, so it early-outs on a cached `memory_map_key`.

Consequences worth knowing:

- Cartridge ROM is only visible while the processor port is *asking* for ROM, so
  a cartridge can write `$34` to `$01` and reach the RAM underneath itself.
- Ultimax (`/GAME` asserted, `/EXROM` not) is handled as its own case: the
  cartridge owns the map, there is no BASIC, KERNAL or character ROM, and I/O is
  visible regardless of CHAREN.
- The 64 KB `RAM[]` array doubles as the storage for the I/O registers and colour
  RAM. That is a deliberate simplification with a real cost — see
  [todo.md](todo.md).

## CPU

`src/cpu.cpp` is a switch-per-opcode interpreter over `machine_state::cpu`.

- All documented opcodes, all addressing modes, correct zero-page and pointer
  wrapping, and the indirect-`JMP` page-boundary bug.
- Cycle counts come from `ticktable[256]`; the page-crossing penalty is applied
  only to the seventeen read instructions that actually earn it, gated by
  `penalty_op()`.
- Read-modify-write instructions perform the dummy write. C64 code depends on
  it: that is what makes `INC $D019` and `DEC $DC0D` acknowledge an interrupt.
- Instruction lengths come from `opcodes[256].length` in `opcodes.cpp`, which is
  the single source of truth shared with the disassembler and the assembler.
  `0` marks a JAM.
- NMI is edge-triggered and always taken; IRQ is level-triggered and gated on
  the interrupt-disable flag. `machine::irq()` gives the host a third IRQ source
  alongside the CIA and VIC, cleared once taken.

## VIC-II

`vic_render_scanline(s, line)` renders one raster line into
`machine_state::framebuffer`, a 384×272 array of packed `0x00RRGGBB`. Rendering
is opt-in (`set_render_enabled`) so headless use stays cheap.

`tick_vic` advances the raster position and renders the line *just finished*, so
a raster-interrupt handler has had the whole line to change registers before
they are sampled. All five graphics modes and all eight sprites (expansion,
priority, collisions) are covered; sprite-background collision uses a per-scanline
foreground mask.

Register storage lives in `RAM[$D000-$D3FF]`; the raster counter, `$D019` latch
and `$D01A` mask live in `machine_state::vic` so `read_vic`/`write_vic` can
model the read-to-clear and write-1-to-acknowledge semantics.

## SID

`src/sid.cpp` is a cycle-driven 6581/8580 model: three waveform generators with
combined-waveform tables, ADSR envelopes, a two-integrator filter and a FIR
resampler down to 44100 Hz.

Register writes are timestamped with `clock_ticks` while audio is enabled, and
`generate_audio()` replays them at cycle accuracy over the span since the last
call. That is what lets a host run a whole frame and *then* ask for its audio
without losing sub-frame register changes. With audio disabled, writes apply
immediately and no samples are produced.

### The `.sid` player

`load_sid()` parses the PSID/RSID header, copies the tune image into RAM and
hand-assembles a driver that calls the tune's `init` and then its `play` from an
interrupt — a CIA timer or a raster IRQ, whichever the header's speed word asks
for. The driver, both IRQ handlers and the NMI handler all live in `$0334-$03FF`
so the text screen at `$0400` stays free.

That screen is the player's front end. `sid_player_state` (in `machine.h`) keeps
the tune image, its metadata and the selected sub-tune, and `exec()` repaints the
40×25 display each call: name, author, release, chip, timing, elapsed time, the
sub-tune row, per-voice level meters taken from `sid_chip::envelope_level()`, and
the master volume. It writes screen codes and colour nybbles straight into the
memory the VIC reads, and re-asserts `$D011`/`$D016`/`$D018`/`$D020`/`$D021` and
the CIA2 bank bits so a tune that programs the VIC cannot blank the display. If
the tune image occupies `$0400-$07FF` the screen moves to the first free 1 KB
page of VIC bank 0 instead.

Input comes from the emulated keyboard matrix, edge-detected against the previous
frame: `1`-`9` restart the tune on another sub-tune and `X` resets the machine.
Nothing here is host specific — a host only feeds keys and shows the frame
buffer, exactly as for any other program.

## CIA

One `cia_chip` struct used twice. CIA1 drives IRQ, CIA2 drives NMI; the only
difference is which line an underflow pulls, decided by the caller. Covers timer
latch and reload, one-shot, the force-load strobe, Timer B counting Timer A
underflows, and ICR latch/mask/read-to-clear.

CIA1 also carries the keyboard matrix (8 rows × 8 columns, active low, scanned
in both directions) and the two joystick ports. CIA2 port A selects the VIC bank,
through its data direction register so undriven lines float high.

## Cartridge

`cartridge` parses a `.crt` image into `chip_bank`s and exposes the currently
banked ROML/ROMH. Bank switching is per hardware type, driven from the
`$DE00-$DFFF` I/O window; `machine_state` re-derives the page tables afterwards.
The `/EXROM` and `/GAME` lines it reports are what select 8K, 16K or Ultimax.

## Disk (device 8)

There is no IEC bus and no 1541. Instead the KERNAL's own LOAD routine is
serviced by the host-side model:

- `machine::insert_disk()` copies a `.d64`/`.d71`/`.d81` image into
  `machine_state::disk`, where it survives reset.
- `cpu_exec` compares the PC against **`$F4A5`** before each fetch. That is the
  default target of the LOAD vector at `$0330`, reached by `JMP ($0330)` from
  `$F49E`, which means `$C3`/`$C4` are already populated and the top of the stack
  is the *caller's* return address — so a simulated `RTS` is correct.
- `machine_state::kernal_load()` honours the documented ABI: secondary address 0
  is a relocating load, success returns carry clear with the end address in
  `X`/`Y` and `$AE`/`$AF`, failure returns carry set with `A` = 4.
- It **declines** anything that is not ours (a verify, a device other than 8, an
  empty filename) so the ROM's own error handling still runs.
- `LOAD"$",8` synthesises a listable BASIC directory at `$0801`.

Trapping at the vector's *target* rather than at `$FFD5` is the point: a program
that installs its own fastloader over `$0330` is deliberately not intercepted,
because pretending to serve it would be a lie about what is emulated.

`src/disk.cpp` identifies the format from the file size alone and rejects
anything not exactly sized. Every offset is then derived from a validated
geometry, so a malformed image cannot read outside the caller's buffer.

## Introspection

`debug_capture()` fills a `debug_state` with the CPU, banking, VIC, CIA and SID
state plus a disassembly window around the PC. It is strictly side-effect free:
latching registers are read from the emulated chip state, never through the bus,
so a debug overlay can never acknowledge an interrupt out from under a program.

`src/assembler.cpp` is a two-pass 6502 assembler over the same opcode table,
used by tests and available to hosts.

## Host application

`app/app.cpp` runs three threads:

- **Emulation** — one PAL frame per iteration, publishing into a triple-buffered
  frame and pushing the frame's samples onto a bounded queue.
- **Audio** — drains that queue into waveOut buffers. The sound card consumes at
  a fixed rate, so the queue's back-pressure paces emulation at exactly real
  time. No timer is involved. The audio thread never blocks on the queue.
- **UI** — owns the window; takes the newest frame under a lock held just long
  enough to swap two indices, so a slow repaint cannot stall emulation.

All access to the machine is under one mutex. Errors are reported *after* it is
released, because `MessageBoxW` re-enters `WM_KILLFOCUS`, which wants the same
non-recursive lock.

The client area is composed into a single 32-bit DIB and presented with one
`BitBlt`: nearest-neighbour integer scaling for the picture, and a debug panel
drawn with the C64's own character ROM. No GDI fonts or stretch blits.

Window geometry and the debug panel's width and visibility persist to
`%APPDATA%\lib8bit\lib8bit.ini`.

## Rules that keep this working

1. `src/` stays platform independent. No `windows.h`, no file access, no `pf::`
   types, no `wchar_t` in new API.
2. `ram_read`/`ram_write` are the only path to memory. Anything that bypasses
   them bypasses banking and I/O.
3. Chips are advanced only from `step_hardware`, and only by cycles actually
   spent. Never advance a chip out of band.
4. One source of truth per fact: `clock_ticks` for time, `opcodes[].length` for
   instruction length, the page tables for banking.
5. `test/emulator-tests.cpp` is the contract. Several tests pin the *current*
   convention rather than hardware truth (the `$D011` YSCROLL baseline, the
   sprite-Y offset, the Timer B cascade period). Changing one of those means
   changing its test in the same commit, deliberately.
