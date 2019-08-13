# Turbo 8bit

Turbo 8bit is a small Commodore 64 emulator for Windows — the companion
C++ app for [turbo8bit.com](https://turbo8bit.com). It runs the original CBM
BASIC / KERNAL ROMs on an emulated 6510 CPU and renders the 40×25 text
screen into a Win32 window using GDI.

The project is intentionally compact and is intended as a learning
exercise rather than a feature-complete emulator (no SID, no sprites,
no disk drive).

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