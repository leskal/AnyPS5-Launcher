# AnyPS5 Launcher design

A desktop frontend for [AnyPS5](https://github.com/boykopovar/AnyPS5): it lists the user's PS5 games, converts each one with the AnyPS5 relinker, launches it, and keeps the player's saves, trophies and settings in one place.

Status: implemented are the game scanner, the settings file, profiles, installing games with the relinker, launching them in their profile's run folder, and the right-click conversions (SELF to ELF, `.zar` to folder, folder to `.zar`). The AnyPS5 version manager is implemented too. The trophy viewer and the launcher's own updater are still plans; sections marked *(planned)* describe them.

## Principles

- **No changes to AnyPS5 are required.** The launcher uses AnyPS5 only through documented interfaces: the relinker command line and exit codes, the runtime layout, environment variables and the run report. The one feature that needs an upstream change (player name) is left out until the change exists.
- **Separate repository.** AnyPS5 is the relinker and runtime. Its rules (no code comments, every dependency a submodule built from source, a C++20-standard-library-only relinker, CI gates) and its release schedule don't fit a GUI app.
- **The player's data is precious; everything else can be rebuilt.** Converted games, caches and logs can be deleted at any time. Saves, trophies and settings live in one folder the user backs up.
- **Refuse rather than guess.** Damaged archives, unknown executables, name collisions and the like are reported with the reason, never worked around silently.

## Stack

C++20, CMake 3.25+, Ninja, WinLibs GCC 15 (the same toolchain as AnyPS5 on Windows). SDL3 for windowing and controllers, Dear ImGui with the SDL renderer backend, nlohmann/json, ZArchive with zstd, stb_image. The Windows build links statically into one `.exe`, which the self-updater relies on. Licence GPL-2.0, the same as AnyPS5 and KytyPS5.

## Interfaces relied on in AnyPS5

Designed against upstream `514a52b` (23 Nov 2026). Re-check these when upstream changes `docs/user/USAGE.md`.

| Interface | Where it's defined |
|---|---|
| `relinker --windows [flags] <eboot.elf> <out.exe>`, exit codes 0 (ok), 1 (bad arguments), 2 (conversion failed), `FAIL:` lines on stderr | `docs/user/USAGE.md`, `core/relinker/main.cpp` |
| Runtime layout: `app0/` and the state folders relative to the working directory; `libs/` beside the `.exe` | `docs/user/USAGE.md` |
| `app0.zar` found at the path of `/app0` plus `.zar` | `core/libs/prx/libc/src/AppArchive.cpp` |
| `anyps5-report.json`, schema version 1 | `docs/run-report.schema.json` |
| Environment variables `ANYPS5_INPUT_CONFIG`, `ANYPS5_SCREENSHOTS`, `ANYPS5_LANGUAGE`, `ANYPS5_LOG_FILE`, `ANYPS5_SHADER_CACHE_DIR`, `ANYPS5_ENTITLEMENTS` | `docs/dev/ENVIRONMENT.md` |
| Release assets `relinker-<tag>.exe`, `prx-windows-<tag>.zip` (files under `libs/`), each with a `sha256:` digest | `.github/workflows/release.yml` |
| SELF magic numbers and the decrypted-or-encrypted test | `tools/unwrap_self.py`, `core/relinker/cli/src/NativeModules.cpp` |
| Trophy files `_trophy/<user id>_<label>.txt` and `.stats.txt`; `trophyNN.ucp` container | `core/libs/prx/libSceNpTrophy2` |
| `tools/titles.json` per-title relink flags | `tools/title_run.py` |
| Patch suggestions, `tools/suggest_patches.py --json`, `schemaVersion` 1 | `tools/suggest_patches.py` |

The relinker's ordinary stdout is human text with no stability promise: show it in logs, don't parse it.

## Folders

```
<launcher folder>\anyps5-launcher.json     settings, next to the .exe (portable)
<userdata>\                                one setting; this is what gets backed up
  profiles\<profile>\
    profile.json                           name, language
    saves\<serial>\_sd\  _sd_mem\
    _trophy\<serial>\
    input\anyps5-input.ini, anyps5-input.<serial>.ini
    screenshots\<serial>\
  games\<serial>\
    data\  download0\  anyps5-entitlements.ini
    meta\                                  trophyNN.ucp, icon0.png, param.json copies
<library>\                                 one setting; rebuildable
  logs\<serial>\                          install.log, <time>.log, <time>.report.json
  shader-cache\<serial>\
<games>\<game>\                            a game set up for AnyPS5 (see "Installing a game")
  anyps5-launcher-install.json            marks the folder as set up
  <serial>.exe                            the relinker's output (+ its other outputs)
  app0\                                   the game's files, or the extracted part of a .zar
  app0.zar                                .zar games: the archive, moved here unchanged
  anyps5-launcher-session.json            only while a game runs: where the save folders came from
  _sd\ _sd_mem\ _trophy\ data\ download0\  only while a game runs: the profile's folders, moved in
```

The launcher makes no links of any kind: no hard links, junctions or symbolic links. Files are only moved (renamed, or copied and deleted across drives) or extracted.

Both default to folders beside the launcher (`library\`, `userdata\`), so a fresh launcher folder is self-contained. Planned: `<launcherdata>\anyps5\<source>\<tag>\` for AnyPS5 versions the launcher downloads itself.

Settings live in `anyps5-launcher.json` next to the launcher's `.exe`, so the launcher is portable. That folder must be writable: don't install the launcher under `Program Files`. Today the file holds:

| Key | Meaning |
|---|---|
| `anyps5Folder` | AnyPS5 clone with a full build, or an extracted release (see "Finding AnyPS5") |
| `gamesFolder` | the folder scanned for games |
| `libraryFolder` | empty means `library\` beside the launcher |
| `userdataFolder` | empty means `userdata\` beside the launcher |
| `profile` | the current profile |
| `version` | file format version, currently 1 |

A profile's own settings are in `profiles\<profile>\profile.json`: `name` and `language` (empty, `auto` or 0 to 30, passed as `ANYPS5_LANGUAGE`). Profile names become folder names, so characters Windows forbids are refused; non-Latin names are fine.

Saving rewrites the file atomically (temporary file, then rename) and keeps keys it doesn't know. An unreadable file is reported at the top of the window and replaced on the next save.

Planned: more than one games folder. Scans never write to games folders; only the conversions below do, and only when the user asks.

Rules:

- The library and userdata folders must not be inside the games folder (Settings refuses to save that), and scans skip them anyway: a converted game's `app0\` holds both `eboot.bin` and `sce_sys\param.json` and would be listed again.
- Keep userdata on the same drive as the games, so saves are renamed in and out for each run rather than copied.

## Who owns what

| Data | Owner | How it's redirected |
|---|---|---|
| Saves (`_sd`, `_sd_mem`) | player | moved beside the exe for the run, moved back after |
| Trophies (`_trophy`) | player | moved in and back |
| Input mapping | player | `ANYPS5_INPUT_CONFIG`; the runtime also reads `anyps5-input.<TITLEID>.ini` beside it |
| Screenshots | player | `ANYPS5_SCREENSHOTS` |
| Language | player | `ANYPS5_LANGUAGE` |
| Player name | player | not possible yet: `"Player"` is hard-coded in `libSceUserService` |
| `data`, `download0` | game | moved in and back |
| Owned add-ons | game | `ANYPS5_ENTITLEMENTS` |
| Shader cache | machine | `ANYPS5_SHADER_CACHE_DIR` |
| Log | machine | `ANYPS5_LOG_FILE`; the launcher builds the file name, `{date}` isn't expanded |
| Run report | machine | always beside the `.exe`; moved into the logs folder after exit |

Save paths are `_sd\<save dir name>` with no title ID, and trophy files are named `<user id>_<label>.txt` with no title ID. So every game needs its own working directory and its own trophy folder: a shared one would mix games' data.

## Launching

Implemented in `src/core/Launch.cpp` and `src/app/Processes.cpp`.

1. Create missing target folders and `anyps5-input.ini`. The game stops if `ANYPS5_INPUT_CONFIG` names a file that doesn't exist; an empty file or one with only comments keeps the default bindings (`LoadInputMapping` in AnyPS5's `InputMapping.cpp`). The launcher writes a short comment header.
2. Move the profile's folders beside the exe. The converted exe makes its own folder the current directory at start (`SetCurrentDirectoryW` in the relinker's Windows entry stub), and AnyPS5 keeps these relative to it:
   - `profiles\<profile>\saves\<serial>\_sd`, `_sd_mem` → `<game>\_sd`, `_sd_mem`
   - `profiles\<profile>\_trophy\<serial>` → `<game>\_trophy`
   - `games\<serial>\data`, `download0` → `<game>\data`, `download0`
   `anyps5-launcher-session.json` is written first and lists where each came from. A folder with files already at one of those places is never touched; the start is refused and names it.
3. Start `<game>\<serial>.exe` in its own folder, with `CREATE_NO_WINDOW`, the environment variables above plus the title's `env` from `tools\titles.json`, and inside a Job Object. The game counts as running while any process in the job is alive, so a title that restarts itself is still tracked, and Stop ends all of them. Output goes to the log through `ANYPS5_LOG_FILE`; `ANYPS5_ENTITLEMENTS` is set only when `games\<serial>\anyps5-entitlements.ini` exists, because AnyPS5 stops the game when the variable names a file it can't read.
4. After exit, move the folders back to the profile (`FinishRun`), then move `anyps5-report.json` to the logs folder and read it for the list's status column. No report means the game crashed or was killed. If the launcher or the PC stopped during a game, the next scan finds the session file and moves the folders back.

Safety rules:

- Install and Remove install are refused while a session file exists, so saves are never deleted or moved with the game.
- Reinstalling replaces the `.exe` and the relinker's other outputs; a `.zar` game's extracted files are extracted again.
- Install, conversions, profile switches and Settings are blocked while a game runs; one game runs at a time.
- A folder with files where a profile folder goes is never deleted: the launch is refused with a message naming it. An empty folder there is removed.

Never pass `--autorun` (it waits for Enter). Don't use `--windows-gui` for now: a console build with redirected output gives a per-game log.

## Game discovery

Implemented in `src/core/Scanner.cpp`.

A **folder** is a game when it holds `sce_sys/param.json` (or `param.sfo`) and `eboot.bin`. The scan doesn't descend into a game folder, doesn't follow symlinks, and skips excluded folders.

A **`.zar`** (any case) is opened with ZArchive and checked for `sce_sys/param.json` or `param.sfo`:

- at the archive root: **app0 at root**, usable directly as `app0.zar`;
- inside exactly one top-level folder: **nested folder**, listed but needs repacking or extraction before it can run;
- neither, or several candidates: reported as a problem.

A `.zar` without `eboot.bin` is listed with the executable marked missing. Only a few kilobytes are read per game, even from very large archives.

Metadata, matching AnyPS5's own parsers:

| Field | Source |
|---|---|
| Serial | `titleId`, accepted only as `[A-Z]{4}[0-9]{5}` (the `title_run.py` rule) |
| Name | `localizedParameters[<locale>].titleName`, trying the profile's locale, then `defaultLanguage`, then `en-US`, then the first entry with a name, then the serial |
| Version | `contentVersion` |
| Fallback | `param.sfo`: `TITLE_ID`, `TITLE`, `APP_VER` or `VERSION`; same header layout as `ParamSfoParser.cpp` |
| Icon | `sce_sys/icon0.png`, up to 8 MiB |

Any read or parse failure becomes a problem entry for that path and the scan continues. Entries with the same serial (a folder and a `.zar`, or two versions) are grouped, and the user picks which one to use.

To do: cache results by path, size and modification time; watch folders for changes.

Open question: MinGW's `recursive_directory_iterator` may follow NTFS junctions without reporting them as symlinks. The exclusion list covers the launcher's own folders; check the behaviour on Windows.

## Executable check

Implemented in `src/core/Executable.cpp`.

| First bytes | Result |
|---|---|
| `7F 45 4C 46` with class 64-bit, little-endian, machine 62 | ELF, ready for the relinker |
| `7F 45 4C 46` otherwise | ELF, unsupported (with the reason) |
| `4F 15 3D 1D` or `54 14 F5 EE` | SELF |
| anything else | unknown (shows the first four bytes) |

An ELF can still be encrypted: some dumps carry a readable ELF header over segments that were never decrypted, and the relinker then fails with errors such as "Unterminated dynamic segment". `src/core/Encryption.cpp` applies `unwrap_self.py`'s test to the ELF's own load segments: a segment counts as encrypted when 64 KiB samples at its start, middle and end are all at or above 7.5 bits per byte, and the file is refused when every load segment looks encrypted or when large ones (0x1000 bytes and up) hold more than half of the loaded bytes. The scan runs it on `eboot.bin` and `eboot.elf` (a few 64 KiB reads per segment) and the list shows "ELF, still encrypted" with the segments. Install runs it on every ELF it would relink (eboot, modules, extra executables) and refuses before moving anything. The decrypted eboot.bin and libc.prx of PPSA21567 measure 0 to 6.3 bits per byte per segment, well below the limit.

## Right-click conversions (implemented)

All three write next to the source, keep the source, run as a background job (one at a time) and rescan when they finish. The menu shows why an action is unavailable.

| Action | Available for | Result |
|---|---|---|
| Convert SELF to ELF | folder games whose `eboot.bin` or any module is a SELF | `eboot.elf` beside `eboot.bin`, made by the built-in port of AnyPS5's `tools/unwrap_self.py` (see below), written through a temporary file and kept only if it is a supported ELF. Each SELF module directly inside `sce_module`, `sce_modules`, `prx` and the title's `--module-dir` folders is converted in place: the ELF goes to `<name>.elf.tmp`, the original is renamed to `<name>.self`, then the ELF takes the original name; a failure at either rename puts the original back. A module whose `<name>.self` already exists is not touched. Refused modules stay as they are and are listed in the result with the script's message. |
| Convert .zar to folder | `.zar` games | `<name>` beside `<name>.zar`; for a nested archive the top folder is dropped, so the result is a normal game folder |
| Convert folder to .zar | folder games | `<folder>.zar` beside the folder, with the game at the archive root; checked by reopening it (every folder present, every file's size) before the rename |

Pack and extract have progress and Cancel. They work in `<output>.partial` and remove it on failure or cancel. A leftover `.partial` from a crash is reported rather than deleted. The scanner ignores `*.partial`.

SELF to ELF for a `.zar` game isn't offered: convert it to a folder first.

## Built-in SELF unwrapper

The launcher has no runtime requirements, so it doesn't call Python. `src/core/SelfUnwrap.cpp` is a line-by-line port of `tools/unwrap_self.py` (AnyPS5 `514a52b`). Python semantics that matter are reproduced:

- unbounded integers (128-bit arithmetic);
- Python's non-negative `%`;
- slice clamping;
- the order of byte counts in the entropy sum, with the compensated float summation CPython 3.12+ uses, so the 7.5 bits-per-byte threshold is decided identically.

Python 3.11 and older sum entropy terms without compensation, which can differ in the last bit; that only matters for a segment at exactly 7.5.

Checked by `tools/compare_unwrap_self.py`. On 23 Nov 2026 it ran about 30,000 containers twice with different seeds, and every result matched under AddressSanitizer and UBSan: about 3,700 identical ELFs and 26,000 identical refusals. The inputs were the 33 distinct containers from AnyPS5's `test_unwrap_self.py` plus mutations of their records, ELF headers and program headers, encrypted-looking payloads and random containers. `tests/UnwrapGolden.hpp` pins three successful conversions and one refusal for the normal test run. Re-run the comparison whenever AnyPS5 changes the script.

## Finding AnyPS5

Implemented in `src/core/AnyPS5.cpp`. The AnyPS5 folder can be:

- **A clone with a full build** (`build\core\relinker\relinker.exe` and `build\core\libs\libs\*.prx`). The runtime is every `.prx` there plus `mimalloc.dll` and `mimalloc-redirect.dll` when present. The three MinGW DLLs come from `libs` if they're there, otherwise from the folder of the compiler recorded in `build\CMakeCache.txt`, as `windows_runtime()` in AnyPS5's `tools/package_release.py` does. A relinker-only build is reported as such.
- **An extracted release**: one `relinker.exe` or `relinker-<tag>.exe` at the top, and the release's `libs\` folder beside it (everything in it is used).

`tools\titles.json`, when present, gives per-title `relink` flags, `fixed_base`, `executables` and `env`, read the way `load_titles()` in `tools/title_run.py` reads them. `relinker --version` is run once per settings change; its revision is stored with each install.

## Installing a game (implemented)

`src/core/Install.cpp`. AnyPS5 fixes where things are: `/app0` is the `app0` folder beside the exe, the archive is `<that folder>.zar`, and the save folders are beside the exe. Without links, the game has to be beside its exe, so Install sets up the game's own folder:

1. **Folder game** `Games\Foo\`: `eboot.elf` (or `eboot.bin` itself), every module directly inside `sce_module`, `sce_modules`, `prx` and the title's `--module-dir` folders, and every extra executable must be ELF; anything still SELF stops the install before anything moves, with the list of files (run Convert SELF to ELF). Then every top-level entry is renamed into `Games\Foo\app0\`, which is instant and copies nothing. A game with an `app0` entry of its own is refused.
2. **.zar game** `Games\Foo.zar` (archive root = the game; a game inside a top folder must be converted to a folder first): `Games\Foo\` is created (refused if it exists), and only these are extracted into `app0\`: `eboot.bin` as `eboot.elf`, all of `sce_sys\`, the module files of the module folders and the extra executables, SELF files unwrapped on the way (an encrypted one stops the install and nothing is left behind). Then the `.zar` is moved, unchanged and still compressed, to `Games\Foo\app0.zar`. The game reads everything else from it.
3. **libs:** the exe loads AnyPS5's libraries from AnyPS5's own libs folder (`build\core\libs\libs` or the release's `libs\`) through `--rpath <absolute path>`. The relinker takes only ASCII there on Windows (`normalizeRunPath`), so a non-ASCII AnyPS5 path is refused. The runtime loads DLLs only from that folder, the exe's folder and System32 (`LoadLibraryExW` with `LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS`), so with an AnyPS5 build the MinGW DLLs from the compiler's folder are copied into that libs folder once.
4. **Relink:** `relinker [--windows] [title flags] [--to-intel on Intel CPUs] [--windows-fixed-base X] [--executable Y]... --rpath <libs> app0\eboot.elf <game>\<serial>.exe`, with `--skip-sce-module` when there are no module folders, run in the game's folder. The relinker writes each module's `<name>.guest.prx` beside it in `app0\`. New top-level files after the run are recorded as its outputs. Output goes to `logs\<serial>\install.log`. A non-zero exit fails the install with the log's first `FAIL:` line; the folder stays set up, not installed, and Install can be run again.
5. **Manifest** `anyps5-launcher-install.json`: format, original name, whether relinking finished, content version, AnyPS5 folder and layout, relinker revision, arguments, outputs, module counts.

**Remove install** undoes it: the exe and outputs are deleted; a folder game's `.guest.prx` files are deleted and its files renamed back out of `app0\`; a `.zar` is moved back to its old name and the extracted files are deleted. Files the launcher didn't make are left and listed.

The list shows "Reinstall" with a reason when the game's content version changed, or when AnyPS5's relinker revision differs from the one that converted it.

Not done yet: an "extract fully" switch for `.zar` games, and pinning a game to an AnyPS5 version.

Some games won't run from an archive: listing a folder that exists both on disk and in the archive is still unsupported. Each game gets an "extract fully" switch, and the list remembers which mode last worked. AnyPS5's `fix/libc-app0-zar-reads` branch extends archive reads to `fopen`, `access`, APR and `sceNgs2ParseWaveformFile`.

## Folder ↔ `.zar` conversion

The archive's root is always the game's `app0`, so a packed game is both a storage format and directly usable as `app0.zar`.

Packing:

- Walk the folder in sorted order: `MakeDir` for every folder, `StartNewFile` plus `AppendData` in chunks, then `Finalize`.
- Write `<name>.zar.partial` beside the folder and rename it once finished.
- Refuse case-only name collisions, links inside the dump, and non-ASCII names (until the runtime's matching is checked). ZArchive compares names case-insensitively for a–z only.
- Verify by reopening: quick (tree and sizes) or full (SHA-256 of every file). The reader doesn't verify the stored hash by itself.
- Keep the source folder unless the user deletes it after a full verify.

Extracting:

- Walk with `GetDirEntryCount` and `GetDirEntry`, read in chunks with `ReadFromFile`, write into `<name>.partial\`, then rename.
- Reject `..`, `:`, absolute paths, reserved names (`CON`, `NUL`, `AUX`, `COM1`…) and names ending in a dot or space.
- Offer "remove the top folder" for nested archives.
- A short read means the archive is damaged: stop and delete the partial output.

Both:

- Run as background jobs with progress and cancel; one job per drive.
- Check free space first.
- The writer compresses at a fixed zstd level 6 on one thread, so packing is the slow part.
- `ZArchiveWriter` doesn't free its path nodes (seen with AddressSanitizer); harmless for one archive per job.

ZArchive's `LookUp` ignores its "files only / folders only" flags; `src/core/Package.cpp` checks the node type itself.

## Trophies *(planned)*

The viewer reads `profiles\<profile>\_trophy\<serial>\<user id>_<label>.txt` (unlocks, `<id> <tick>` per line) and `.stats.txt` (progress), plus the game's `trophyNN.ucp` copied into `games\<serial>\meta\`.

`.ucp` container: everything big-endian.

| Offset | Field |
|---|---|
| `0x00` | magic `0xB228C60A` |
| `0x04` | version `1` |
| `0x08` | total size |
| `0x10` | number of files |
| `0x14` | file-table offset |

Each file entry is `0x40` bytes from `table + 0x20`: a 32-byte name, then an 8-byte offset and an 8-byte length.

Files inside:

- **`tropconf.json`**: `defaultLanguage` and `trophies[]`, each with `id`, `grade` (`P`, `G`, `S` or `B`), `groupId`, `platinumTrophyId`, `hidden`, `hasReward` and `unlockCondition`.
- **`tropmeta_<locale>.json`**: `metadata.titleMetadata.name`, `groupMetadata[]` and `trophyMetadata[]`, each trophy with `name`, `detail` and `reward`.
- **`tropNNNN.png`**: one icon per trophy.

Pick the language the way AnyPS5 does: console language, then `en-US`, then `defaultLanguage`. KytyPS5 skips `en-US`.

- Unlock ticks are microseconds since 0001-01-01 UTC; subtract `62135596800000000` to get Unix microseconds.
- Locked hidden trophies show a placeholder; other locked ones show their icon in greyscale.
- Never edit the unlock files: a blank line, a `\r` or a duplicate stops the game. "Reset" deletes both files while the game isn't running.
- Pop-ups while playing: match `[trophy] … unlocked trophy N` in the game's output and show a Windows toast. That line isn't a documented contract, so also re-read the files after the game exits.

## AnyPS5 versions (implemented)

`src/core/Versions.cpp`, `src/core/Http.cpp`; the window is "AnyPS5 versions" in the top bar.

A version is a relinker **and** the runtime libraries of the same release: a relinker and a runtime from different releases aren't guaranteed to fit each other, so the two always travel together. Every version is a folder that `ProbeAnyPS5` accepts:

- **Downloaded:** `<launcher>\anyps5\<owner>\<repo>\<tag>\` holding `relinker-<tag>.exe`, `libs\` and, when the tag has one, `tools\titles.json`.
- **Local:** a folder of the user's, added by picking a relinker or a folder: an AnyPS5 clone with a build (`build\core\relinker\relinker.exe` is mapped to the clone), or a release extracted by hand with `libs\` beside the relinker. A fork you build yourself is added this way. Local folders are never deleted, only forgotten.

The list is `<launcher>\anyps5\versions.json` (downloaded folders stored relative to it, so the launcher folder can move) together with the custom repositories. The chosen one is `anyps5Version` in the settings file; `anyps5Folder` keeps its folder, and a folder set by hand or by an older launcher is adopted as a local version at start.

Releases come from `GET https://api.github.com/repos/<owner>/<repo>/releases?per_page=30` (WinHTTP on Windows, curl on development Linux hosts). Drafts are dropped. A release is offered when it has the two files AnyPS5's release workflow (`tools/package_release.py`) publishes for this system: `relinker-<tag>.exe` and `prx-windows-<tag>.zip` (Linux: `relinker-<tag>` and `prx-linux-<tag>.tar.gz`). "Latest" is the newest by publish date that isn't a pre-release. A fork works the same way when it publishes releases; one without releases is built and added as local.

Download:

1. Both files go into `<tag>.partial\`; each is checked against the size and, when GitHub lists one, the `sha256` digest of the release asset.
2. The runtime archive is unpacked with the system's `tar` (`%SystemRoot%\System32\tar.exe`, part of Windows 10 1803 and later, which reads `.zip`), so no unzip library is needed. The archive holds `libs\…`.
3. `tools/titles.json` of the tag is fetched from `raw.githubusercontent.com` when present (per-title relink flags); without it titles relink with defaults.
4. The folder must pass `ProbeAnyPS5`, then `<tag>.partial` is renamed to `<tag>`. A failed or cancelled download leaves nothing behind.

At start, when no usable version is chosen, the launcher asks whether to download the latest official release, or to open the versions window. Selecting another version re-reads it (`relinker --version`, `titles.json`); games relinked with a different revision show "Reinstall" with the reason.

Not done: per-game pinning, checking for updates in the background, release lists beyond the first 30, and the launcher updating itself. Anonymous GitHub API use is limited to 60 requests an hour per IP; the error says so when it's hit.

## Upstream wishlist (none required)

- An environment variable for the player name (`libSceUserService`); until then the setting is hidden.
- Environment variables for the save, trophy, `data` and `download0` roots, so profiles' folders wouldn't need moving for each run.
- A way to name the archive (for example an environment variable for `app0.zar`), so a `.zar` could stay where it is.
- A machine-readable conversion summary from the relinker.
- A documented trophy-unlock event.

## Still to verify on Windows

The install and launch flow was tested end to end on Linux with a stand-in relinker and games. On Windows these are still to be confirmed with a real AnyPS5 build and real games:

- `--rpath` with an absolute path to AnyPS5's libs folder.
- A `.zar` game with only the essentials extracted. AnyPS5 doesn't support listing a folder that exists both on disk and in `app0.zar` (`/app0` itself, `sce_module`), so a game that does stops with "not implemented"; such games need full extraction for now.
- Moving save folders across drives when userdata and the games are on different drives.
