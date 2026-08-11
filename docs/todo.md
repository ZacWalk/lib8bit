# lib8bit gaps

What is not implemented, and what is implemented approximately. Ordered by value
per unit of work within each section. For how the emulator is built, see
[design.md](design.md).

Most of this came out of a structured review of the CPU, machine/VIC/cartridge,
disk and host code. Items marked *untested* have no assertion pinning them either
way.

## Start here

**Undocumented CPU opcodes are not executed.** `SLO/RLA/SRE/RRA/LAX/SAX/DCP/ISC/
ANC/ALR/ARR/SBX` are consumed with the correct length and cycle count, but do
nothing. The instruction stream stays aligned; the results are wrong.

This is the largest correctness gap for real software — games and SID players use
these routinely — and it is the cheapest to close: the length and cycle tables
are already right, and Klaus Dormann's functional test is already reachable via
`--run-bin`. It just is not wired into the automated suite, so nothing in CI
validates the opcode set exhaustively.

## CPU

| | |
|---|---|
| No dummy read on indexed addressing | `abs,X` / `abs,Y` / `(zp),Y` compute the effective address directly. The RMW dummy *write* is emulated, which is the case that matters on a C64, so this only shows on I/O side effects such as `LDA $DC00,X`. *Untested.* |
| Interrupts sampled at the instruction boundary | Hardware samples during the penultimate cycle, so an interrupt asserted by an instruction's last cycle is deferred one further instruction. Related: no `CLI`/`SEI`/`PLP` one-instruction delay. Matters only for cycle-exact raster code. *Untested.* |
| `PLP` keeps the pushed B bit, `RTI` clears it | Inconsistent but unobservable — bits 4/5 do not physically exist. |

## Memory and I/O

**RAM under the I/O area is the same storage the VIC and colour RAM use.** When
I/O is banked out, a write to `$D000-$DFFF` lands in the same bytes that back the
VIC registers and colour RAM. On hardware those 4 KB are completely independent.
Any program using that RAM — common for sprite and bitmap data — corrupts the
display.

Closing this means splitting `vic_regs[64]` and `color_ram[1024]` out of `RAM[]`
and updating `vic.cpp`, `machine.cpp`, `debug.cpp` and several pinned tests. It
is the most invasive item on this list. *Untested.*

Smaller: colour RAM's high nybble reads back as `0` rather than open bus, which
some copy protection checks.

## VIC-II

| | |
|---|---|
| Bad-line cycle stealing | Not modelled at all. |
| CSEL (`$D016` bit 3) | 38-column mode ignored; only XSCROLL is read. RSEL *is* handled. |
| Sprites are not clipped by the border | They draw over it; hardware gives the border unit priority. |
| Invalid graphics modes | Render as background; hardware outputs black. |
| Character ROM shadow at `$1000-$1FFF` | Applied to character fetches only, not bitmap or sprite fetches. |
| `tick_vic` drops raster compares | If ever called with ≥126 cycles it advances several lines but checks the compare once. Unreachable through `cpu_exec` (max ~7 cycles per step), so latent rather than live. |

Two approximations in what *is* implemented:

- **Collision interrupts re-latch.** Hardware latches `$D019` bit 1 only on the
  first collision after `$D01E` is read; this re-raises when a new sprite joins
  an existing collision. Setting an already-set status bit is idempotent, so it
  is only observable if a program acknowledges `$D019` without reading `$D01E`.
- **DEN is sampled per scanline**, not once at raster `$30`. This makes the
  common case right — games blanking the screen with `STA $D011` while loading
  now actually blank — at the cost of exactness for mid-frame DEN effects.

## CIA

- **TOD clocks** are not implemented.
- **Timer B cascade is off by one** relative to the φ2 paths (`< 0` versus
  `<= 0`). The two conventions cannot both be right. The existing test pins the
  current asymmetry, so changing it is a deliberate decision.
- **Writing the CIA1 ICR mask clears the IR flag (bit 7).** On a 6526, bit 7 is
  cleared only by *reading* the ICR. CIA2 does not perform the equivalent
  recompute at all, so the two chips behave differently when a mask bit is set
  over an already-latched flag. *Untested for CIA2 — there are no NMI tests.*

## Cartridge

- **EasyFlash and other `$DE00-$DFFF` bank-switching handlers** are absent.
- `EPYX_FASTLOAD`, `ZAXXON` and `FINAL_CARTRIDGE_I` are declared in `cart_type`
  but have no handler, and silently degrade to plain 8K/16K.
- The CHIP packet's type field (ROM / RAM / Flash) is ignored, so RAM banks are
  treated as ROM.
- `cartridge::ultimax_mode` is written with an inverted test. Currently harmless
  because the field is dead state, never read.
- The bank switching that *does* exist — Ocean, Magic Desk, FunPlay, Dinamic, GS,
  Super Games, Comal, FC3 — is entirely untested.

## Disk

- **No IEC serial bus.** A program that revectors `$0330` to its own fastloader
  is deliberately not intercepted, and gets no disk. This is the honest failure
  mode; see [design.md](design.md#disk-device-8).
- **No SAVE.** Would need image write-back and a policy for a modified image.
- **The load is instantaneous** — the trap charges no cycles. That is effectively
  what a fastload cartridge does, but a loader with a timing watchdog could
  notice.
- **Splat (unclosed) files are loadable.** The `closed` flag is decoded for the
  directory but never checked when extracting; a real drive refuses them.
- `LOAD"*"` picks the first PRG where real DOS picks the first directory entry.
  Arguably better behaviour, and documented in `disk.h`.
- No G64 or T64 images.

## Host application

Crash and data-loss paths are closed. Remaining:

| | |
|---|---|
| No `WM_GETMINMAXINFO` | Shrinking the window below 384×272 leaves stale pixels instead of scaling down. The blit guards are correct, so it looks broken rather than being unsafe. |
| `waveOutWrite` failure becomes a hot spin | A persistently failing device (removal, driver reset) leaves the `TIME_CRITICAL` audio thread busy-looping. No failure counter, no fallback to the no-audio path. |
| `audio_engine::start` leaks prepared headers | If `waveOutPrepareHeader` fails partway, earlier headers are not unprepared before `waveOutClose`. |
| `MAX_PATH` buffer in the Open dialog | Paths over 259 characters fail silently. Drag-and-drop sizes correctly and handles them. |
| `load_file` holds the machine lock across 3M cycles | Every load stalls the emulation thread and drains the audio queue, producing a brief audible glitch. |
| One frame of stale debug data | The panel renders whatever snapshot was last published until the next frame. Self-corrects in ~20 ms. |
| `WM_DROPFILES` does not foreground the window | A drop onto a background window loads the file but leaves focus elsewhere. |

## API

`machine.h` is not yet a properly encapsulated embeddable API: `_state` is
public and the header sits in the global namespace.

## Conventions pinned by tests

Not bugs, but not hardware-exact either. Each is asserted in
`test/emulator-tests.cpp`, so changing one means updating its test in the same
commit, deliberately:

- The `$D011` YSCROLL baseline of 3.
- The sprite-Y offset — a sprite's first line lands one raster line low.
- The Timer B cascade period described above.
