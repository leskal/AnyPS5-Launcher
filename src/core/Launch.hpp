#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "core/AnyPS5.hpp"
#include "core/Install.hpp"
#include "core/Session.hpp"
#include "core/Profiles.hpp"
#include "core/Scanner.hpp"

namespace launcher {

struct RunPlan {
    std::filesystem::path executable;
    std::filesystem::path workingDirectory;
    std::filesystem::path logFile;
    std::filesystem::path reportSource;
    std::filesystem::path reportTarget;
    std::vector<std::pair<std::string, std::string>> environment;
};

// Creates the profile's folders, moves the ones AnyPS5 keeps beside the exe into
// the game's folder, and describes how to start the game:
//   profiles/<profile>/saves/<serial>/_sd      -> <game>/_sd
//   profiles/<profile>/saves/<serial>/_sd_mem  -> <game>/_sd_mem
//   profiles/<profile>/_trophy/<serial>        -> <game>/_trophy
//   games/<serial>/data, download0             -> <game>/data, download0
// Nothing is linked. FinishRun moves them back.
RunPlan PrepareRun(const GameEntry& game, const Layout& layout, const ProfileSettings& profile, const TitleEntry* title);

struct RunReport {
    std::string exit;
    std::string stage;
    double seconds = 0;
    std::string stopKind;
    std::string stopMessage;
    std::string summary;
};

// After the game exits: moves the profile's folders back, then collects the report.
std::optional<RunReport> FinishRun(const RunPlan& plan);

// Moves anyps5-report.json next to the run's log and reads it.
std::optional<RunReport> CollectRunReport(const RunPlan& plan);

std::optional<RunReport> ReadRunReport(const std::filesystem::path& file);

// The newest run report of a game, if any.
std::optional<RunReport> LatestRunReport(const Layout& layout, std::string_view serial);

}
