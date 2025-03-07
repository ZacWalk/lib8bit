# lib8bit

lib8bit is a small Commodore 64 emulator packaged as a C++ static library.
It is intended for embedding in applications such as Diffractor to add retro
computer support. A separate Windows test app exercises the emulator, loads
`.prg` files, and renders the 40x25 text screen with GDI.

Repository: <https://github.com/ZacWalk/lib8bit>

The emulator is intentionally compact rather than feature-complete. It does
not currently emulate SID audio, sprites, or an IEC/1541 disk drive. The
library can inspect Commodore disk images without drive emulation.

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

The suite currently verifies that the emulator:

- Boots the BASIC and KERNAL ROMs to the `READY.` prompt.
- Renders the full cold-start screen (the `**** COMMODORE 64 BASIC V2 ****`
  banner and `38911 BASIC BYTES FREE` line), and keeps it intact while the
  periodic 50 Hz IRQ runs.
- Accepts keyboard input and evaluates `PRINT 6*7` as `42`.
- Loads `test/example.prg`, runs it, and displays `HELLO WORLD`.
- Lists `test/1942.d64` and verifies its disk name and `1942` PRG entry.
- Parses every CRT fixture as a valid CCS64/VICE container, including all
  `CHIP` packet boundaries and ROM sizes.

### Disk Images

`src/disk.h` exposes platform-independent disk-directory parsing for:

- D64: 35-, 40-, and 42-track images, with or without error-byte tables.
- D71: double-sided 1571 images, with or without error-byte tables.
- D81: 1581 images, with or without error-byte tables.

List any supported image from the command line:

```powershell
.\bin\test8bit64d.exe --list-disk .\test\1942.d64
```

The listing includes the PETSCII disk name and ID, file names, file types,
block counts, open-file markers, and locked-file markers. G64/G71 nibble images,
T64 tape images, and actual DOS/IEC drive-command execution are not yet
implemented.

CRT execution is not implemented yet. The fixtures establish the next targets:

| Fixture | Hardware type | Current target |
|---------|---------------|----------------|
| `pitfall.crt` | Normal 8 KiB | ROML mapping at `$8000` |
| `river_raid.crt` | Normal 16 KiB | ROML/ROMH mapping at `$8000`/`$A000` |
| `gng_bl.crt` | EasyFlash, 64 banks | `$DE00` bank selection and `$DE02` control |

## JavaScript Parity

The reference implementation is <https://github.com/ZacWalk/turbo8bit>.
Native parity should be built in testable stages:

1. Normal 8 KiB and 16 KiB CRT loading, EXROM/GAME lines, and cartridge reset.
2. EasyFlash and other bank-switching handlers in `$DE00-$DFFF`.
3. Complete C64 memory banking through processor ports `$0000` and `$0001`.
4. VIC-II character, bitmap, multicolor, sprite, raster, and scrolling behavior.
5. CIA timers, interrupts, keyboard matrix, and joystick input.
6. IEC serial bus and 1541/1571/1581 drive behavior, building on disk parsing.
7. SID 6581/8580 voices, envelopes, filters, and PSID/RSID playback.

## Features

- 6510 / 6502 CPU core
  - All documented opcodes
  - All addressing modes including the indirect-`JMP` page-wrap bug
  - BCD (decimal) mode for `ADC` / `SBC`
  - `BRK`, `RTI`, `IRQ` and `NMI` with correct stack frames and flag handling
- C64 memory map with BASIC, KERNAL and character ROMs at their normal locations
- VIC-II text mode rendering (40×25, 16-colour palette, border + background colour)
- Keyboard input routed into the kernal keyboard buffer at `$0277`
- ~50 Hz IRQ driven from a Win32 timer