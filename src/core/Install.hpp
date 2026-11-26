#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "core/AnyPS5.hpp"
#include "core/Job.hpp"
#include "core/Process.hpp"
#include "core/Profiles.hpp"
#include "core/Scanner.hpp"

namespace launcher {

inline constexpr const char* InstallManifestName = GameHomeMarker;

// Files AnyPS5 writes next to the game's exe at each run. They are never part of
// the game: the launcher moves them to the run's logs, and set-up and removal
// drop any that were left behind instead of treating them as the game's files.
inline constexpr const char* RunReportName = "anyps5-report.json";
inline constexpr const char* RunRecipeName = "anyps5-recipe.json";
bool IsRunFile(const std::string& name);

struct InstallManifest {
    std::string titleId;
    std::string name;
    std::string contentVersion;
    std::string format;                // "folder" or "zar"
    std::string originalName;          // the folder's or the .zar's name before set-up
    bool keptApp0 = false;             // the game was already in an app0 folder; nothing was moved
    bool complete = false;             // false while set up but not (or not successfully) relinked
    std::string installedAt;
    std::string anyps5Root;
    std::string anyps5Layout;
    std::string relinkerVersion;
    std::vector<std::string> arguments;
    std::vector<std::string> outputs;  // files the relinker wrote beside app0
    int modules = 0;
    int unwrapped = 0;                 // files unwrapped from SELF while extracting a .zar
    std::vector<std::string> addedLibraries;  // copied into AnyPS5's libs folder by this install; not saved
};

// The manifest of a set-up game folder, complete or not.
std::optional<InstallManifest> ReadHomeManifest(const std::filesystem::path& home);

// The manifest when the game is installed: relinked and its exe present.
std::optional<InstallManifest> ReadInstallManifest(const std::filesystem::path& home);

std::filesystem::path ExecutableIn(const std::filesystem::path& home, std::string_view titleId);

// Where a game is set up: its own folder; the parent of an app0 folder that is
// alone in it; or for Games\Foo.zar the new folder Games\Foo.
std::filesystem::path HomeFor(const GameEntry& game);

// A .zar's set-up folder that an earlier removal left because it held files the
// launcher didn't make (the game's devlog and temp0, say): a new install can use it
// again, as nothing in it is in the way. False when the folder isn't there.
bool LeftoverHome(const GameEntry& game);

struct InstallRequest {
    const GameEntry& game;
    const AnyPS5Runtime& anyps5;
    const Layout& layout;
    const TitleEntry* title = nullptr;
    std::string relinkerVersion;
    bool windows = TargetIsWindows;
    bool intelHost = false;
};

// Sets the game up for AnyPS5 and relinks it. Nothing is linked and nothing in
// a .zar is changed or unpacked:
//   folder game Games\Foo\    its files are moved into Games\Foo\app0\ once
//   .zar game   Games\Foo.zar becomes Games\Foo\app0.zar (moved, unchanged), and
//                             only eboot, sce_sys\, the module folders and extra
//                             executables are extracted into Games\Foo\app0\,
//                             SELF files unwrapped on the way
// Then the relinker writes Games\Foo\<serial>.exe, and each <module>.guest.prx
// beside its module in app0\. The exe finds AnyPS5's libraries through --rpath.
// A folder game must be ELF already (Convert SELF to ELF).
InstallManifest InstallGame(const InstallRequest& request, const ProcessRunner& run, JobControl& control);

struct RemovedInstall {
    std::filesystem::path restored;     // the game folder or .zar as it was before set-up
    std::vector<std::string> leftovers; // files left in the set-up folder because the launcher didn't make them
};

// Undoes InstallGame: the exe and the relinker's outputs are deleted, the game's
// files or the .zar are moved back where they were, and extracted files are deleted.
RemovedInstall RemoveInstall(const GameEntry& game);

std::string QueryRelinkerVersion(const AnyPS5Runtime& anyps5, const ProcessRunner& run, const std::filesystem::path& scratchFile);

// Why an installed game should be installed again, or empty when it's current.
std::string ReinstallReason(const GameEntry& game, const InstallManifest& manifest, const std::string& relinkerVersion);

// The first FAIL: line of a relinker log, or its last line.
std::string RelinkerFailure(const std::filesystem::path& log);

}
