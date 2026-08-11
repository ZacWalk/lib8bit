# lib8bit

[![CI](https://github.com/ZacWalk/lib8bit/actions/workflows/ci.yml/badge.svg)](https://github.com/ZacWalk/lib8bit/actions/workflows/ci.yml)

A compact Commodore 64 emulator packaged as a C++20 static library, plus a small
Win32 app that drives it.

The library is platform independent and does no file I/O — the host supplies
bytes, the library returns a frame buffer and audio samples. It is intended for
embedding in applications such as Diffractor to add retro computer support.

Repository: <https://github.com/ZacWalk/lib8bit>

## What is emulated

| Chip | Coverage |
|------|----------|
| 6510 CPU | All documented opcodes and addressing modes (including the indirect-`JMP` page-wrap bug), BCD arithmetic, `BRK`/`RTI`/`IRQ`/`NMI`, cycle-accurate timing with page-crossing penalties |
| Memory | Full banking through the processor port at `$00`/`$01`, BASIC/KERNAL/character ROMs, the I/O area |
| VIC-II | 384×272 raster renderer: text, multicolor text, bitmap and multicolor bitmap modes, 8 sprites with expansion, priority and collisions, raster interrupts, 16-colour VICE palette |
| SID | 6581/8580, three voices with ADSR envelopes, multi-mode filter, `.sid` (PSID/RSID) playback through the installed player driver, with an on-screen player: tune details, live voice meters, and sub-tune selection on the number keys |
| CIA ×2 | Timers, interrupts, the full keyboard matrix, two joysticks |
| Cartridge | `.crt` loading with ROML/ROMH banking |
| Disk | `.d64`/`.d71`/`.d81` mounted as device 8: the KERNAL LOAD routine is serviced from the image, so a program can keep loading files while it runs, and `LOAD"$",8` lists the directory |

Also included: `.prg` loading and a two-pass 6502 assembler.

How it all fits together is in [docs/design.md](docs/design.md). What is missing
or approximate is in [docs/todo.md](docs/todo.md) — headline gaps are the
undocumented CPU opcodes, the IEC serial bus (so a program supplying its own
fastloader will not load), SAVE to disk, VIC bad-line cycle stealing and CIA TOD
clocks.

## Layout

| Path | Contents |
|------|----------|
| `src/` | The emulator. Platform independent — no Windows headers, no file system. Builds `lib8bit.lib`. |
| `app/` | Win32 test app: GDI video, waveOut audio, menus, drag-and-drop. |
| `test/` | Command-line behavioural test suite and its fixtures. |
| `docs/` | [design.md](docs/design.md) (architecture) and [todo.md](docs/todo.md) (gaps). |

## Build and run

```powershell
.\dd.ps1 test   # build + run the test suite
.\dd.ps1 run    # build + launch the Win32 app
```

Or with MSBuild directly:

```powershell
msbuild lib8bit.sln /m /p:Configuration=Release /p:Platform=x64
```

Binaries land in `bin/` as `<name><64|32><r|d>` — architecture then
configuration, e.g. `lib8bit64d.lib`, `app8bit64r.exe`, `test8bit64d.exe`.
Intermediate artifacts go under `intermediate/`.

## The app

Open a `.prg`, `.crt`, `.sid`, `.d64`, `.d71` or `.d81` through **File > Open**,
by dragging it onto the window, or by passing it on the command line. A disk
image stays in the drive after its first program starts, so games that load more
data as they run work. The title bar always states the current machine state:
media, input mode, pause, and whether audio started. Window position and the
debug panel's width and visibility are remembered in
`%APPDATA%\lib8bit\lib8bit.ini`.

| | |
|---|---|
| Ctrl+O / Ctrl+R / Ctrl+E | Open, Reset (keeps the cartridge), Eject Media |
| F9 | Pause |
| Input menu | Arrow keys act as cursor keys, Joystick 1, or Joystick 2 |
| Help > Keyboard | The emulated C64 key mapping |

A `.sid` tune comes up in a player screen showing its name, author, release,
chip and timing, a running clock, live per-voice level meters and the list of
sub-tunes. Keys **1**-**9** pick a sub-tune and **X** stops playback and returns
the machine to BASIC.

## Embedding

Add a project reference to `src/lib8bit.vcxproj`, include `src/machine.h`, and
link the matching `lib8bit<arch><cfg>.lib`. The consuming project must use the
same architecture, configuration and C runtime library.

`machine` owns a `machine_state` and is non-copyable. Feed it bytes
(`load_prg`, `load_crt`, `load_sid`, `insert_disk`), step it with `exec(cycles)`,
then read `framebuffer()` and `generate_audio()`. `app/app.cpp` is a complete
worked host, and [docs/design.md](docs/design.md) explains the execution model.

## Tests

`test8bit64d.exe` runs ~170 assertions and returns non-zero on failure: every
documented CPU opcode with flags and cycle counts, pixel-exact VIC-II output for
each graphics mode and for sprite collisions, CIA timer and interrupt behaviour,
the assembler, a full KERNAL cold boot to the `READY.` prompt, keyboard input
evaluating `PRINT 6*7` as `42`, SID audio including a real `.sid` tune, disk
loading through the KERNAL (a machine-code routine calling `SETNAM`/`SETLFS`/
`LOAD`, `LOAD"$",8` and the `FILE NOT FOUND` path), and the PRG/CRT/D64 fixtures
in `test/` (resolved from the source path baked in at compile time, so nothing
needs copying next to the executable).

It doubles as a small toolbox:

```powershell
.\bin\test8bit64d.exe --list-disk .\test\1942.d64            # list a disk image
.\bin\test8bit64d.exe --run-disk .\test\1942.d64 "1942*"     # mount, LOAD, RUN
.\bin\test8bit64d.exe --run-prg .\test\example.prg           # boot, load, run a PRG
.\bin\test8bit64d.exe --run-bin <file> <loadHex> <startHex>  # run a raw 6502 binary
.\bin\test8bit64d.exe --asm <file.asm>                       # assemble 6502 source
```

## Reference implementation

The JavaScript original is <https://github.com/ZacWalk/turbo8bit>. The native
port has reached parity on the CPU, memory banking, VIC-II, SID and normal
8/16 KiB cartridges.

Contributing, or working on this with an AI agent? See [AGENTS.md](AGENTS.md).