#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include "core/Launch.hpp"

namespace launcher {

// Why a run ended, in plain words, from its run report and the end of its log.
struct RunDiagnosis {
    enum class Level { Fine, Stopped, Problem };
    Level level = Level::Fine;
    std::string headline;  // "Crashed: access violation (0xC0000005)"
    std::string detail;    // the line of the log or report it comes from
    std::string hint;      // what may help, or empty
};

RunDiagnosis DiagnoseRun(const std::optional<RunReport>& report, std::string_view logTail, std::optional<int> exitCode, bool stopRequested);

// The last bytes of a log (whole lines), for diagnosis and reports.
std::string ReadLogTail(const std::filesystem::path& log, std::size_t bytes = 256 * 1024);

// The newest run log of a game in <library>/logs/<serial>, if any.
std::optional<std::filesystem::path> LatestRunLog(const Layout& layout, std::string_view serial);

// Replaces the user's home folder and user name with ~ and <user>, as AnyPS5's bug_report.py does.
std::string RedactPersonal(std::string text);

}
