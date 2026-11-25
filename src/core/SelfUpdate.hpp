#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/Http.hpp"
#include "core/Process.hpp"
#include "core/Versions.hpp"

namespace launcher {

// The launcher updates itself from the GitHub releases of its own repository.
// Each release (made by .github/workflows/release.yml for a v* tag) carries
// anyps5-launcher-<tag>-windows-x64.zip with anyps5-launcher.exe inside.
inline constexpr const char* LauncherRepository = "leskal/AnyPS5-Launcher";
inline constexpr const char* LauncherExeName = "anyps5-launcher.exe";

std::string LauncherAssetName(std::string_view tag);

struct LauncherUpdate {
    Release release;
    ReleaseAsset asset;
};

// The newest release (not a pre-release) with the Windows zip that is newer than
// currentVersion ("0.1.0"; the tag may start with v). nullopt: up to date.
std::optional<LauncherUpdate> FindLauncherUpdate(const std::vector<Release>& releases, std::string_view currentVersion);

// Downloads and unpacks the update into <launcher>/update/<tag>/; returns the new exe.
std::filesystem::path StageLauncherUpdate(const LauncherUpdate& update, const std::filesystem::path& launcherFolder, const Downloader& download,
                                          const ProcessRunner& run, JobControl& control);

// Puts the new exe in place of the running one: Windows lets a running exe be
// renamed, so the old one becomes <exe>.old and is deleted at the next start.
void SwapLauncherExe(const std::filesystem::path& current, const std::filesystem::path& fresh);

// At start: removes <exe>.old and the update folder left by an update.
void CleanUpLauncherUpdate(const std::filesystem::path& current, const std::filesystem::path& launcherFolder);

// A release's notes as plain lines to show: Markdown headings, bullets and bold
// marks become plain text, links keep their text, and GitHub's "Full Changelog"
// line and blank runs are dropped (an empty text is a gap). At most maxLines lines.
struct NoteLine {
    std::string text;
    bool heading = false;  // was a "## ..." line, such as "New" or "Fixed"
    bool operator==(const NoteLine&) const = default;
};
std::vector<NoteLine> ReleaseNoteLines(std::string_view notes, std::size_t maxLines = 40);

// Where the launcher's window was, so the updated one opens in the same place:
// passed as --window=x,y,width,height[,max].
struct WindowPlacement {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    bool maximized = false;
};

std::string FormatWindowArgument(const WindowPlacement& placement);
std::optional<WindowPlacement> ParseWindowArgument(const std::vector<std::string>& arguments);

}
