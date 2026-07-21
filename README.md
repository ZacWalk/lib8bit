# lib8bit

[![CI](https://github.com/ZacWalk/lib8bit/actions/workflows/ci.yml/badge.svg)](https://github.com/ZacWalk/lib8bit/actions/workflows/ci.yml)

lib8bit is a compact Commodore 64 emulator packaged as a C++ static library.
It is intended for embedding in applications such as Diffractor to add retro
computer support. A separate Windows test app exercises the emulator: it renders
the full VIC-II picture with GDI, plays SID audio through waveOut, and loads
`.prg`, `.crt`, and `.d64`/`.d71`/`.d81` files via a file dialog or drag-and-drop.

Repository: <https://github.com/ZacWalk/lib8bit>

The emulator covers the 6510 CPU, full memory banking, the VIC-II (all graphics
modes plus sprites and raster interrupts), the SID sound chip, both CIA timer
chips with the keyboard matrix and joysticks, and cartridge bank switching. It
can also inspect Commodore disk images without full IEC/1541 drive emulation.

## Projects

- `src/lib8bit.vcxproj` builds the platform-independent emulator as
  `lib8bit.lib`.
- `app/lib8bit-test.vcxproj` builds the Win32 test app and links `lib8bit` via
  a Visual Studio project reference.
- `test/lib8bit-tests.vcxproj` builds a command-line behavioral test suite.
- `lib8bit.sln` contains all three projects for Win32 and x64 Debug/Release builds.

## Build

Open `lib8bit.sln` in Visual Studio 2026 and build the desired configuration,
or run:

```powershell
msbuild lib8bit.sln /m /p:Configuration=Release /p:Platform=x64
```

All build outputs are written to the `bin/` folder. Binaries follow the naming
convention `<name><64|32><r|d>`, where the digits are the architecture
(`64` = x64, `32` = Win32) and the letter is the configuration (`d` = Debug,
`r` = Release):

| Project | Debug x64 | Release x64 | Debug Win32 | Release Win32 |
|---------|-----------|-------------|-------------|---------------|
| Static library | `lib8bit64d.lib` | `lib8bit64r.lib` | `lib8bit32d.lib` | `lib8bit32r.lib` |
| Test app | `app8bit64d.exe` | `app8bit64r.exe` | `app8bit32d.exe` | `app8bit32r.exe` |
| Tests | `test8bit64d.exe` | `test8bit64r.exe` | `test8bit32d.exe` | `test8bit32r.exe` |

Intermediate build artifacts (`.obj`, `.tlog`, etc.) go under `intermediate/`.

To embed the emulator, add a reference to `src/lib8bit.vcxproj` or include
`src/machine.h` and link the matching `lib8bit<arch><cfg>.lib`. The consuming
project must use the same architecture, configuration, and C runtime library.

### dd.ps1 helper

`dd.ps1` builds and then runs a target in one step (defaults to Debug x64):

```powershell
.\dd.ps1 test           # build + run the emulator tests
.\dd.ps1 run            # build + launch the Win32 test app
.\dd.ps1 run -Release   # Release build
.\dd.ps1 test -Win32    # 32-bit build
```

It locates MSBuild through `vswhere`, so no Visual Studio developer prompt is
required.

## Tests

Build and run the command-line emulator tests with the helper:

```powershell
.\dd.ps1 test
```

Or build and run manually:

```powershell
msbuild lib8bit.sln /t:lib8bit-tests /m /p:Configuration=Debug /p:Platform=x64
.\bin\test8bit64d.exe
```

Test fixtures (`example.prg`, `1942.d64`, the `.crt` files) live in `test/` and
are resolved from the source path baked in at compile time, so they do not need
to be copied next to the executable.

The suite runs headless unit tests followed by full-machine integration checks.
It verifies that:

- The 6502 CPU core executes every documented opcode with correct flags,
  addressing modes, BCD arithmetic, and cycle counts.
- The VIC-II renders each graphics mode, sprites, sprite collisions, and raster
  interrupts to the exact expected frame-buffer pixels.
- The CIA timers count, reload, cascade, and raise interrupts correctly.
- The built-in 6502 assembler encodes instructions and directives, and
  round-trips assembled code back through the CPU.
- The machine boots the BASIC and KERNAL ROMs to the `READY.` prompt, renders the
  full cold-start screen (the `**** COMMODORE 64 BASIC V2 ****` banner and
  `38911 BASIC BYTES FREE` line), and keeps it intact while the periodic IRQ runs.
- Keyboard input via the CIA1 matrix reaches the kernal and evaluates
  `PRINT 6*7` as `42`.
- The SID renders non-silent 16-bit PCM for a gated waveform, and plays a real
  `.sid` tune through its installed player driver.
- `test/example.prg` loads, runs, and displays `HELLO WORLD`.
- `test/1942.d64` lists with its disk name and `1942` PRG entry, and individual
  programs extract from the image.
- Every CRT fixture parses as a valid CCS64/VICE container, and the normal
  cartridges map their ROM into the CPU address space.

### Disk Images

`src/disk.h` exposes platform-independent disk-directory parsing for:

- D64: 35-, 40-, and 42-track images, with or without error-byte tables.
- D71: double-sided 1571 images, with or without error-byte tables.
- D81: 1581 images, with or without error-byte tables.

Besides the automated suite, the test executable exposes a few command-line
tools:

```powershell
.\bin\test8bit64d.exe --list-disk .\test\1942.d64          # list a disk image
.\bin\test8bit64d.exe --run-prg .\test\example.prg          # boot, load, run a PRG
.\bin\test8bit64d.exe --run-bin <file> <loadHex> <startHex> # run a raw 6502 binary
.\bin\test8bit64d.exe --asm <file.asm>                      # assemble 6502 source
```

The listing includes the PETSCII disk name and ID, file names, file types,
block counts, open-file markers, and locked-file markers. G64/G71 nibble images,
T64 tape images, and actual DOS/IEC drive-command execution are not yet
implemented.

Normal (type 0) 8 KiB and 16 KiB cartridges load and map their ROM into the CPU
address space. EasyFlash and other bank-switching handlers are the next target:

| Fixture | Hardware type | Status |
|---------|---------------|--------|
| `pitfall.crt` | Normal 8 KiB | ROML mapped at `$8000` |
| `river_raid.crt` | Normal 16 KiB | ROML/ROMH mapped at `$8000`/`$A000` |
| `gng_bl.crt` | EasyFlash, 64 banks | `$DE00` bank selection and `$DE02` control (pending) |

## JavaScript Parity

The reference implementation is <https://github.com/ZacWalk/turbo8bit>. Most of
the emulator now has native parity:

- Complete C64 memory banking through processor ports `$0000` and `$0001`.
- VIC-II character, bitmap, multicolor, sprite, raster, and scrolling behavior.
- CIA timers, interrupts, keyboard matrix, and joystick input.
- SID 6581/8580 voices, envelopes, filters, and `.sid` tune playback.
- Normal 8 KiB and 16 KiB CRT loading with ROML/ROMH banking.

Remaining work:

- EasyFlash and other bank-switching handlers in `$DE00-$DFFF`.
- IEC serial bus and 1541/1571/1581 drive behavior, building on disk parsing.

## Features

- 6510 / 6502 CPU core
  - All documented opcodes
  - All addressing modes including the indirect-`JMP` page-wrap bug
  - BCD (decimal) mode for `ADC` / `SBC`
  - `BRK`, `RTI`, `IRQ` and `NMI` with correct stack frames and flag handling
  - Cycle-accurate timing, including page-crossing penalties
- Complete C64 memory banking through the processor port at `$0000`/`$0001`,
  with BASIC, KERNAL, and character ROMs and the I/O area
- VIC-II raster renderer (384×272 frame buffer)
  - Standard and multicolor text, standard and multicolor bitmap modes
  - Eight hardware sprites with multicolor, priority, and collision registers
  - Raster interrupts and per-scanline register changes for raster splits
  - Full 16-colour VICE palette
- SID 6581 / 8580 sound chip
  - Three voices with selectable waveforms and ADSR envelopes
  - Multi-mode analog filter model resampled to 16-bit PCM
  - `.sid` (PSID/RSID) tune playback through the installed player driver
- Two CIA chips with timers, interrupts, the full keyboard matrix, and two joysticks
- Cartridge (`.crt`) loading with ROML/ROMH banking
- `.prg` loading and `.d64`/`.d71`/`.d81` disk-image directory parsing
- Built-in two-pass 6502 assembler
- Standalone multithreaded Win32 test app with double-buffered GDI video,
  gapless waveOut audio, drag-and-drop, and a file dialog