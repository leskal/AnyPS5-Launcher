# AnyPS5 Launcher

A desktop frontend for [AnyPS5](https://github.com/boykopovar/AnyPS5): a game list that finds your PS5 games, converts them with the AnyPS5 relinker and launches them, with saves, trophies and settings kept per profile.

**Status:** early, but the main loop works:

- **Game list:** game folders and `.zar` archives, with icon, name, serial, version, format, ELF/SELF state, install state and the last run's result.
- **Install with AnyPS5** (right-click): sets the game's own folder up for AnyPS5 and runs the relinker with the game's flags from AnyPS5's `tools\titles.json`. No links of any kind are made.
  - Folder game `Games\Foo\`: its files are moved (renamed) into `Games\Foo\app0\`, and the exe goes into `Games\Foo\`. Run Convert SELF to ELF first if it still has SELF files.
  - `.zar` game `Games\Foo.zar`: the archive is moved unchanged to `Games\Foo\app0.zar`, and only eboot, `sce_sys` and the modules are extracted into `Games\Foo\app0\` (SELF files unwrapped on the way). The game reads the rest from the archive.
  - The exe loads AnyPS5's libraries from AnyPS5's own folder (`--rpath`), so nothing is copied per game. **Remove install** puts everything back.
- **Play** (double-click): moves the profile's saves, trophies, `data` and `download0` beside the exe, starts the game, and moves them back when it exits (or at the next start if the launcher was closed). Input mapping and screenshots go to the profile. **Stop** ends it.
- **Profiles:** each has its own saves, trophies, input mapping, screenshots and console language.
- **Conversions** (right-click): SELF to ELF, `.zar` to folder, folder to `.zar`.
  SELF to ELF writes `eboot.elf` beside `eboot.bin`, and replaces each SELF `.prx`/`.sprx` in `sce_module`, `sce_modules`, `prx` and the title's module folders with its ELF, keeping the original as `<name>.self`. Modules that can't be unwrapped (encrypted) are left as they are and listed.
- The list shows each game's modules by kind, for example `modules: 12 ELF, 3 SELF`.
- **Settings** in `anyps5-launcher.json` next to the `.exe`.

- **AnyPS5 versions** (top bar): download any release of `boykopovar/AnyPS5` or of a fork that publishes releases, or add a local relinker or AnyPS5 build; pick which one relinks games. If none is set up, the launcher offers to download the latest release at start.

The trophy viewer is designed in [docs/DESIGN.md](docs/DESIGN.md) but not built yet.

This is a separate project. It needs no changes to AnyPS5 and talks to it only through its documented command line, runtime layout, environment variables and run report.

## Building on Windows

Requirements: Git, CMake 3.25 or newer, Ninja, and [WinLibs](https://winlibs.com/) GCC 15 (the toolchain AnyPS5 uses), with its `bin` folder on `PATH`.

First time only, from PowerShell in the repository folder:

```powershell
powershell -ExecutionPolicy Bypass -File tools\bootstrap.ps1
```

This runs `git init` if needed and adds the dependencies as pinned, shallow submodules under `3rdparty\`: SDL 3.4.20, Dear ImGui 1.93.0, nlohmann/json 3.12.0, zstd 1.5.7, ZArchive and stb.

Then:

```powershell
cmake --preset release
cmake --build --preset release
ctest --preset release
build\release\anyps5-launcher.exe
```

On first start the Settings window opens: set your games folder, then Save. If no AnyPS5 version is set up, the launcher offers to download the latest release; you can also add your own build (for example `C:\dev\AnyPS5`) under "AnyPS5 versions". Downloaded versions go to `anyps5\` beside the launcher. Logs and shader caches go to `library\` and profiles to `userdata\` beside the launcher unless you choose other folders; converted games stay in their own folders in your games folder; keep userdata on the same drive as your games so saves are renamed, not copied, for each run. The AnyPS5 version's folder path must be plain ASCII (the relinker's `--rpath` needs it). Settings are stored next to the `.exe`, so keep the launcher in a folder you can write to. The `tests` preset builds only the core library and its tests, without SDL or ImGui.

After cloning an existing copy, use `git submodule update --init` instead of the bootstrap script.

## Requirements to run

Only Windows 10 or newer. The launcher is a single `.exe` with no runtime to install: no Python, no Visual C++ redistributable, no DLLs beside it.

## Layout

| Path | Contents |
|---|---|
| `src/core/` | Scanner, metadata parsers, executable check, folder and `.zar` readers. No UI. |
| `src/app/` | SDL3 + Dear ImGui window |
| `tests/` | Core tests; they build real `.zar` files with ZArchive's writer |
| `docs/DESIGN.md` | The full plan: folders, launching, saves, trophies, conversion, updates |
| `tools/compare_unwrap_self.py` | Developer check that the built-in SELF unwrapper still matches AnyPS5's `tools/unwrap_self.py` |

## The built-in SELF unwrapper

`src/core/SelfUnwrap.cpp` is a C++ port of AnyPS5's `tools/unwrap_self.py`, so users don't need Python. It must keep producing the same bytes and the same refusal messages as the script. When AnyPS5 changes the script, port the change and run the comparison, which needs Python and an AnyPS5 checkout:

```powershell
cmake --build --preset release --target unwrap_self_cli
python tools\compare_unwrap_self.py --anyps5 C:\dev\AnyPS5 --cli build\release\unwrap_self_cli.exe
```

It runs both on the containers AnyPS5's own `tools/tests/test_unwrap_self.py` builds, plus thousands of mutated variants, and fails on any difference. `tests/UnwrapGolden.hpp` keeps a few of those containers with the SHA-256 of the script's output for the regular test suite.

## Licence

GPL-2.0, the same as AnyPS5. See [LICENSE](LICENSE).
