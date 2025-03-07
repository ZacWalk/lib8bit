# lib8bit Repository Guidance

lib8bit is a C++20 Commodore 64 emulator split into a reusable static library and a Windows test application.

## Project Boundaries

- `src/` owns platform-independent emulator code and builds `lib8bit.lib`.
- Disk image parsers belong in `src/` and operate on caller-provided bytes;
	host file-system access remains in the app or test executable.
- `app/` owns the Win32 test application, platform abstraction, GDI rendering, menus, and resources.
- Keep Windows and `pf::` dependencies out of `src/` so the emulator can be linked into Diffractor and other hosts.
- Add host input, rendering, and file-system adaptation at the app boundary. Expose platform-neutral operations from `machine.h` when the library needs a new capability.

## Coding Guidelines

- Preserve the existing tab-indented C++ style and target C++20.
- Keep the emulator compact; avoid adding abstractions unless they improve the static-library API or remove real duplication.
- Keep Debug projects on the static debug runtime (`/MTd`) and Release projects on the static runtime (`/MT`) to avoid project-reference linker conflicts.
- Do not edit generated files in `intermediate/` or `bin/`.

## Build And Validation

Build the full solution after source or project changes:

```powershell
msbuild lib8bit.sln /m /p:Configuration=Debug /p:Platform=x64
```

All build outputs land in the `bin/` folder using the naming convention
`<name><64|32><r|d>` (`64`/`32` = x64/Win32, `d`/`r` = Debug/Release), e.g.
`lib8bit64d.lib`, `app8bit64d.exe`, `test8bit64d.exe`. Intermediate artifacts
stay under `intermediate/`.

Use the `dd.ps1` helper to build-and-run in one step (defaults to Debug x64):

```powershell
.\dd.ps1 test   # build + run the emulator tests
.\dd.ps1 run    # build + launch the Win32 test app
```

Run the command-line behavioral tests after emulator changes (the `dd.ps1`
helper builds first, or run the binary directly):

```powershell
.\dd.ps1 test
# or, after building manually:
.\bin\test8bit64d.exe
```

For changes to project configuration or public headers, also build `Release|x64` and at least one Win32 configuration.
