#include "core/Diagnosis.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <system_error>

#include "core/Utf8.hpp"

namespace launcher {

namespace {

std::string Lower(std::string_view text) {
    std::string out(text);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

// The whole line of text that holds position at.
std::string LineAt(std::string_view text, std::size_t at) {
    const auto start = text.rfind('\n', at);
    const auto end = text.find('\n', at);
    auto line = text.substr(start == std::string_view::npos ? 0 : start + 1, (end == std::string_view::npos ? text.size() : end) - (start == std::string_view::npos ? 0 : start + 1));
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.remove_suffix(1);
    while (!line.empty() && line.front() == ' ') line.remove_prefix(1);
    return std::string(line.substr(0, 300));
}

// The last occurrence of any of the needles (lowercase), as a line.
std::optional<std::string> FindLast(std::string_view text, const std::string& lower, std::initializer_list<const char*> needles) {
    std::size_t best = std::string::npos;
    for (const char* needle : needles) {
        const auto at = lower.rfind(needle);
        if (at != std::string::npos && (best == std::string::npos || at > best)) best = at;
    }
    if (best == std::string::npos) return std::nullopt;
    return LineAt(text, best);
}

const char* ExceptionName(unsigned long code) {
    switch (code) {
    case 0xC0000005: return "access violation";
    case 0xC0000094: return "integer division by zero";
    case 0xC0000095: return "integer overflow";
    case 0xC00000FD: return "stack overflow";
    case 0xC000001D: return "illegal instruction";
    case 0xC0000096: return "privileged instruction";
    case 0x80000003: return "breakpoint";
    case 0xC0000409: return "fast fail";
    case 0xC0000374: return "heap corruption";
    case 0xE06D7363: return "C++ exception";
    default: return nullptr;
    }
}

std::string Hex(unsigned long code) {
    char text[16];
    std::snprintf(text, sizeof(text), "0x%08lX", code);
    return text;
}

}

RunDiagnosis DiagnoseRun(const std::optional<RunReport>& report, std::string_view logTail, std::optional<int> exitCode, bool stopRequested) {
    using Level = RunDiagnosis::Level;
    const auto lower = Lower(logTail);
    if (stopRequested) return {Level::Stopped, "You stopped the game", {}, {}};

    if (report) {
        if (report->stopKind == "missing-function") {
            return {Level::Problem, "Needs a system function AnyPS5 doesn't have yet", report->missingFunction.empty() ? report->stopMessage : report->missingFunction,
                    "Nothing to fix on your side: the game needs a newer AnyPS5. Make a report to share it."};
        }
        if (report->stopKind == "unsupported-shader" || report->stopKind == "unsupported-format") {
            return {Level::Problem, "Uses graphics AnyPS5 can't draw yet", report->stopMessage, "The game needs a newer AnyPS5. Make a report to share it."};
        }
        if (report->stopKind == "other" || report->stopKind == "assert" || report->exit == "abort") {
            return {Level::Problem, "Stopped on an engine error", report->stopMessage.empty() ? report->summary : report->stopMessage, {}};
        }
    }

    if (report && report->exit == "clean") return {Level::Fine, "Closed normally", report->summary, {}};

    if (auto line = FindLast(logTail, lower, {"dependency module not found", "failed to load module", "module not found"})) {
        return {Level::Problem, "A module is missing", *line, "Reinstall the game. If it stays, AnyPS5 doesn't provide this module yet."};
    }
    if (auto line = FindLast(logTail, lower, {"vk_error_out_of_device_memory", "out of device memory"})) {
        return {Level::Problem, "The GPU ran out of memory", *line, "Close other programs that use the GPU, or lower the game's own graphics settings."};
    }
    if (auto line = FindLast(logTail, lower, {"vk_error_device_lost", "device lost"})) {
        return {Level::Problem, "The GPU stopped", *line, "Update the graphics driver. If it keeps happening at the same place, make a report."};
    }
    if (auto line = FindLast(logTail, lower, {"ran out of memory", "out of memory", "commit limit"})) {
        return {Level::Problem, "The system ran out of memory", *line, "Close other programs, or let Windows grow the page file (System > About > Advanced system settings)."};
    }
    if (const auto at = lower.rfind("unhandled exception 0x"); at != std::string::npos) {
        const auto line = LineAt(logTail, at);
        const unsigned long code = std::strtoul(std::string(lower.substr(at + 20, 10)).c_str(), nullptr, 16);
        const char* name = ExceptionName(code);
        std::string hint;
        if (lower.find("nvgpucomp64.dll") != std::string::npos) hint = "The crash is in NVIDIA's shader compiler: update the graphics driver.";
        return {Level::Problem, std::string("Crashed: ") + (name != nullptr ? name : "exception") + " (" + Hex(code) + ")", line, hint};
    }
    if (auto line = FindLast(logTail, lower, {"[libc] unhandled exception", "terminate called", "fatal:"})) {
        return {Level::Problem, "Stopped on an error", *line, {}};
    }

    if (exitCode && *exitCode != 0) {
        const auto code = static_cast<unsigned long>(static_cast<unsigned int>(*exitCode));
        if (const char* name = ExceptionName(code)) return {Level::Problem, std::string("Crashed: ") + name + " (" + Hex(code) + ")", {}, {}};
        return {Level::Problem, "Ended with exit code " + std::to_string(*exitCode), {}, {}};
    }
    if (report) return {Level::Fine, "Ended", report->summary, {}};
    return {Level::Fine, "Closed", {}, {}};
}

std::string ReadLogTail(const std::filesystem::path& log, std::size_t bytes) {
    std::ifstream stream(log, std::ios::binary | std::ios::ate);
    if (!stream) return {};
    const auto size = static_cast<std::size_t>(stream.tellg());
    const auto start = size > bytes ? size - bytes : 0;
    stream.seekg(static_cast<std::streamoff>(start));
    std::string text(size - start, '\0');
    stream.read(text.data(), static_cast<std::streamsize>(text.size()));
    text.resize(static_cast<std::size_t>(stream.gcount()));
    if (start > 0) {
        if (const auto newline = text.find('\n'); newline != std::string::npos) text.erase(0, newline + 1);
    }
    return text;
}

std::optional<std::filesystem::path> LatestRunLog(const Layout& layout, std::string_view serial) {
    std::optional<std::filesystem::path> newest;
    std::string newestName;
    std::error_code error;
    for (std::filesystem::directory_iterator it(layout.Logs(serial), error), end; !error && it != end; it.increment(error)) {
        const auto name = ToUtf8(it->path().filename());
        // Run logs are <yyyy-mm-dd_hh-mm-ss>.log; install.log and the rest are not runs.
        if (name.size() < 5 || name.substr(name.size() - 4) != ".log" || !std::isdigit(static_cast<unsigned char>(name[0]))) continue;
        if (name > newestName) {
            newestName = name;
            newest = it->path();
        }
    }
    return newest;
}

std::string RedactPersonal(std::string text) {
    const auto replaceAll = [&](const std::string& from, const std::string& to) {
        if (from.size() < 2) return;
        for (std::size_t at = 0; (at = text.find(from, at)) != std::string::npos; at += to.size()) text.replace(at, from.size(), to);
    };
    for (const char* variable : {"USERPROFILE", "HOME"}) {
        if (const char* home = std::getenv(variable); home != nullptr && home[0] != 0) {
            std::string path(home);
            replaceAll(path, "~");
            std::replace(path.begin(), path.end(), '\\', '/');
            replaceAll(path, "~");
        }
    }
    for (const char* variable : {"USERNAME", "USER"}) {
        if (const char* user = std::getenv(variable); user != nullptr && std::string(user).size() >= 3) replaceAll(user, "<user>");
    }
    for (const char* variable : {"COMPUTERNAME", "HOSTNAME"}) {
        if (const char* host = std::getenv(variable); host != nullptr && std::string(host).size() >= 3) replaceAll(host, "<host>");
    }
    return text;
}

}
