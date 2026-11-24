#include "core/Launch.hpp"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <system_error>

#include <nlohmann/json.hpp>

#include "core/FileOps.hpp"
#include "core/Install.hpp"
#include "core/Session.hpp"
#include "core/Utf8.hpp"

namespace launcher {

namespace {

std::string Timestamp() {
    const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    char text[32];
    std::strftime(text, sizeof(text), "%Y-%m-%d_%H-%M-%S", &local);
    return text;
}

void Create(const std::filesystem::path& folder) {
    std::error_code error;
    std::filesystem::create_directories(folder, error);
    if (error) throw std::system_error(error, "cannot create " + ToUtf8(folder));
}

std::string ReadText(const std::filesystem::path& file) {
    std::ifstream stream(file, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

}

RunPlan PrepareRun(const GameEntry& game, const Layout& layout, const ProfileSettings& profile, const TitleEntry* title, const GameSettings& settings) {
    const auto& serial = game.metadata.titleId;
    const auto install = game.home;
    RunPlan plan;
    std::error_code error;
    if (install.empty() || !ReadInstallManifest(install)) throw std::runtime_error(game.metadata.name + " is not installed");
    plan.executable = ExecutableIn(install, serial);

    CreateProfile(layout);
    const auto saves = layout.Saves(serial);
    const auto data = layout.GameData(serial);
    for (const auto& folder : {saves / "_sd", saves / "_sd_mem", layout.Trophies(serial), data / "data", data / "download0", layout.Screenshots(serial),
                               layout.InputConfig().parent_path(), layout.Logs(serial), layout.ShaderCache(serial)}) {
        Create(folder);
    }
    if (!std::filesystem::exists(layout.InputConfig(), error)) {
        WriteFileAtomically(layout.InputConfig(),
                            "# AnyPS5 input mapping for the profile \"" + layout.profile + "\".\n"
                            "# Empty keeps AnyPS5's default bindings; see docs/user/INPUT_MAPPING.md in AnyPS5.\n"
                            "# Bindings for one game go into anyps5-input.<serial>.ini beside this file.\n");
    }

    // The converted exe makes its own folder the current one at start, and AnyPS5
    // keeps saves, trophies, data and download0 there. The profile's folders are
    // moved in for the run and moved back by FinishRun.
    MoveInSession(install, {{"_sd", saves / "_sd"},
                            {"_sd_mem", saves / "_sd_mem"},
                            {"_trophy", layout.Trophies(serial)},
                            {"data", data / "data"},
                            {"download0", data / "download0"}});

    plan.workingDirectory = install;
    const auto stamp = Timestamp();
    plan.logFile = layout.Logs(serial) / (stamp + ".log");
    plan.reportSource = install / RunReportName;
    plan.reportTarget = layout.Logs(serial) / (stamp + ".report.json");
    std::filesystem::remove(plan.reportSource, error);
    std::filesystem::remove(install / RunRecipeName, error);

    auto& env = plan.environment;
    env.emplace_back("ANYPS5_INPUT_CONFIG", ToUtf8(layout.InputConfig()));
    env.emplace_back("ANYPS5_SCREENSHOTS", ToUtf8(layout.Screenshots(serial)));
    env.emplace_back("ANYPS5_LOG_FILE", ToUtf8(plan.logFile));
    env.emplace_back("ANYPS5_SHADER_CACHE_DIR", ToUtf8(layout.ShaderCache(serial)));
    if (!profile.language.empty()) env.emplace_back("ANYPS5_LANGUAGE", profile.language);
    if (const auto entitlements = data / "anyps5-entitlements.ini"; std::filesystem::is_regular_file(entitlements, error)) {
        env.emplace_back("ANYPS5_ENTITLEMENTS", ToUtf8(entitlements));
    }
    if (title != nullptr) {
        for (const auto& [name, value] : title->env) env.emplace_back(name, value);
    }
    for (auto& [name, value] : GameSettingsEnvironment(settings)) {
        std::erase_if(env, [&](const auto& entry) { return entry.first == name; });
        env.emplace_back(std::move(name), std::move(value));
    }
    return plan;
}

std::optional<RunReport> ReadRunReport(const std::filesystem::path& file) {
    const auto document = nlohmann::json::parse(ReadText(file), nullptr, false);
    if (document.is_discarded() || !document.is_object()) return std::nullopt;
    RunReport report;
    const auto run = document.value("run", nlohmann::json::object());
    const auto stop = document.value("stop", nlohmann::json::object());
    if (run.is_object()) {
        if (run.contains("exit") && run["exit"].is_string()) report.exit = run["exit"].get<std::string>();
        if (run.contains("stage") && run["stage"].is_string()) report.stage = run["stage"].get<std::string>();
        if (run.contains("seconds") && run["seconds"].is_number()) report.seconds = run["seconds"].get<double>();
    }
    if (stop.is_object()) {
        if (stop.contains("kind") && stop["kind"].is_string()) report.stopKind = stop["kind"].get<std::string>();
        if (stop.contains("message") && stop["message"].is_string()) report.stopMessage = stop["message"].get<std::string>();
    }
    if (const auto missing = document.find("missing"); missing != document.end() && missing->is_object()) {
        if (const auto functions = missing->find("functions"); functions != missing->end() && functions->is_array() && !functions->empty()) {
            const auto& first = functions->front();
            if (first.is_object() && first.contains("name") && first["name"].is_string()) {
                const auto library = first.contains("library") && first["library"].is_string() ? first["library"].get<std::string>() : std::string();
                report.missingFunction = (library.empty() ? std::string() : library + "::") + first["name"].get<std::string>();
            }
        }
    }
    char seconds[32];
    std::snprintf(seconds, sizeof(seconds), "%.0f s", report.seconds);
    report.summary = (report.exit.empty() ? std::string("ended") : report.exit) + " after " + seconds;
    if (!report.stage.empty() && report.stage != "presents") report.summary += ", reached " + report.stage;
    if (!report.stopKind.empty() && report.stopKind != "none") {
        report.summary += " (" + report.stopKind;
        if (!report.stopMessage.empty()) report.summary += ": " + report.stopMessage.substr(0, 160);
        report.summary += ")";
    }
    return report;
}

std::optional<RunReport> FinishRun(const RunPlan& plan) {
    MoveOutSession(plan.workingDirectory);
    return CollectRunReport(plan);
}

// Moves a file into the logs, copying when a rename can't (another drive).
static void MoveToLogs(const std::filesystem::path& source, const std::filesystem::path& target) {
    std::error_code error;
    std::filesystem::rename(source, target, error);
    if (!error) return;
    error.clear();
    std::filesystem::copy_file(source, target, std::filesystem::copy_options::overwrite_existing, error);
    if (!error) std::filesystem::remove(source, error);
}

std::optional<RunReport> CollectRunReport(const RunPlan& plan) {
    std::error_code error;
    // The recipe sits beside the report; left in the game's folder it would later be
    // taken for one of the game's own files.
    const auto recipe = plan.reportSource.parent_path() / RunRecipeName;
    if (std::filesystem::is_regular_file(recipe, error)) {
        auto target = plan.reportTarget;
        auto name = ToUtf8(target.filename());
        if (name.ends_with(".report.json")) name.replace(name.size() - 12, 12, ".recipe.json");
        else name += ".recipe.json";
        MoveToLogs(recipe, target.replace_filename(FromUtf8(name)));
    }
    error.clear();
    if (!std::filesystem::is_regular_file(plan.reportSource, error)) return std::nullopt;
    std::filesystem::rename(plan.reportSource, plan.reportTarget, error);
    if (error) {
        error.clear();
        std::filesystem::copy_file(plan.reportSource, plan.reportTarget, std::filesystem::copy_options::overwrite_existing, error);
        if (error) return ReadRunReport(plan.reportSource);
        std::filesystem::remove(plan.reportSource, error);
    }
    return ReadRunReport(plan.reportTarget);
}

std::optional<RunReport> LatestRunReport(const Layout& layout, std::string_view serial) {
    std::filesystem::path newest;
    std::error_code error;
    for (std::filesystem::directory_iterator it(layout.Logs(serial), error), end; !error && it != end; it.increment(error)) {
        const auto name = ToUtf8(it->path().filename());
        if (name.ends_with(".report.json") && (newest.empty() || it->path().filename() > newest.filename())) newest = it->path();
    }
    if (newest.empty()) return std::nullopt;
    return ReadRunReport(newest);
}

}
