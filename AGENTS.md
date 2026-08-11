# lib8bit Repository Guidance

lib8bit is a C++20 Commodore 64 emulator: a platform-independent static library
(`src/`), a Win32 test app (`app/`), and a command-line test suite (`test/`).

Read [docs/design.md](docs/design.md) before changing anything in `src/` — it
covers the execution model, the banking page tables, the disk trap and the
invariants that hold the design together. [docs/todo.md](docs/todo.md) is the
gap list; prefer picking work from there over inventing new work.
See [README.md](README.md) for what the emulator covers at a glance.

## Project Boundaries

- `src/` is platform independent. No `windows.h`, no `wchar_t` in new API, no
  file-system access, no `pf::` types. It must link into Diffractor and other
  hosts unchanged.
- The library takes bytes and returns bytes: parsers in `src/` operate on
  caller-provided buffers. Reading and writing files is the host's job.
- `app/` owns everything Windows: the message loop, GDI rendering, waveOut
  audio, menus, resources, and the host-key to C64-matrix mapping.
- When the app needs a new capability, expose a platform-neutral operation from
  `machine.h` rather than reaching into `machine_state`.

## Coding Guidelines

- Tab-indented, C++20, existing naming (`snake_case` types and functions).
- Keep it compact. This is meant to be a small, fast emulator — add an
  abstraction only when it improves the library API or removes real duplication.
- `src/rom-*.cpp` are ROM images as byte arrays. Never hand-edit them.
- Never edit anything under `intermediate/` or `bin/`.
- Debug uses the static debug runtime (`/MTd`), Release the static runtime
  (`/MT`). Changing this breaks the project-reference link.

## Build And Validation

`dd.ps1` builds and runs in one step and finds MSBuild via `vswhere`, so no
developer prompt is needed. It defaults to Debug x64 (`run` defaults to Release):

```powershell
.\dd.ps1 test               # build + run the test suite
.\dd.ps1 run                # build + launch the Win32 app
.\dd.ps1 test -Release      # Release
.\dd.ps1 test -Win32        # 32-bit
```

Direct MSBuild after any source or project change:

```powershell
msbuild lib8bit.sln /m /p:Configuration=Debug /p:Platform=x64
.\bin\test8bit64d.exe       # returns non-zero on failure
```

Gotcha: at the **solution** level the 32-bit platform is named `x86`
(`/p:Platform=x86`); `Platform=Win32` only works on an individual `.vcxproj`.
For changes to project configuration or to public headers, also build
`Release|x64` and `Debug|x86`.

Outputs go to `bin/` as `<name><64|32><r|d>` (`64`/`32` = x64/Win32,
`d`/`r` = Debug/Release), e.g. `lib8bit64d.lib`, `app8bit64d.exe`,
`test8bit64d.exe`. Intermediates stay under `intermediate/`.

CI ([.github/workflows/ci.yml](.github/workflows/ci.yml)) builds and tests
Debug and Release x64 on every push and PR.

## What The Tests Pin

`test/emulator-tests.cpp` is the contract. Before "fixing" an emulation detail,
check whether a test already encodes the current behaviour — several do
deliberately:

- CPU: every documented opcode's result, flags and **cycle count**.
- VIC-II: **exact frame-buffer pixels** for each graphics mode and for sprite
  collisions. The `$D011` YSCROLL baseline and the sprite-Y offset are pinned to
  the current convention; changing either requires updating the tests in the
  same change, deliberately.
- CIA: timer counting, reload, one-shot, cascade and interrupt latching.
- Disk: the KERNAL LOAD contract (carry, `X`/`Y` end address, RAM contents),
  `LOAD"$",8` and the `FILE NOT FOUND` path.
- Full-machine: KERNAL cold boot to `READY.`, `PRINT 6*7` typed through the
  keyboard matrix, `example.prg`, `1942.d64`, and the `.crt` fixtures.

A green build proves nothing on its own — run `.\dd.ps1 test` and check for
`FAIL` lines. Smoke-test `app/` changes by launching the app; the title bar
reports media, input mode, pause state and whether audio started.

## Known Gaps

The gap list lives in [docs/todo.md](docs/todo.md), ranked by value per unit of
work and marked with what is untested. Keep it current: when you close an item,
delete it; when a review turns one up, add it there rather than in this file.

Note that several tests pin the *current* convention rather than hardware truth
(the `$D011` YSCROLL baseline, the sprite-Y offset, the Timer B cascade period).
Those are listed at the end of that document — changing one means changing its
test in the same commit, deliberately.

