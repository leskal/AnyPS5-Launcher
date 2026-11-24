#include "core/GameSettings.hpp"

#include <charconv>
#include <fstream>
#include <iterator>
#include <system_error>

#include <nlohmann/json.hpp>

#include "core/FileOps.hpp"

namespace launcher {

namespace {

bool OneOf(const std::string& value, std::initializer_list<const char*> choices) {
    if (value.empty()) return true;
    for (const char* choice : choices) {
        if (value == choice) return true;
    }
    return false;
}

bool WholeNumber(const std::string& value, long minimum, long maximum) {
    long number = 0;
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), number);
    return error == std::errc() && end == value.data() + value.size() && number >= minimum && number <= maximum;
}

std::string Text(const nlohmann::json& document, const char* key) {
    const auto found = document.find(key);
    return found != document.end() && found->is_string() ? found->get<std::string>() : std::string();
}

bool ValidLanguage(const std::string& value) {
    return value.empty() || value == "auto" || WholeNumber(value, 0, 30);
}

bool ValidResolution(const std::string& value) {
    if (value.empty()) return true;
    const auto x = value.find_first_of("xX");
    return x != std::string::npos && WholeNumber(value.substr(0, x), 1, 100000) && WholeNumber(value.substr(x + 1), 1, 100000);
}

}

bool ValidFpsLimit(const std::string& value) {
    if (value.empty()) return true;
    // 0.5 to 1000000, at most three decimals.
    const auto dot = value.find('.');
    if (dot != std::string::npos && (value.size() - dot - 1 == 0 || value.size() - dot - 1 > 3)) return false;
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (i != dot && (value[i] < '0' || value[i] > '9')) return false;
    }
    double number = 0;
    try {
        number = std::stod(value);
    } catch (const std::exception&) {
        return false;
    }
    return number == 0 || (number >= 0.5 && number <= 1000000);
}

bool ValidSharpness(const std::string& value) {
    if (value.empty()) return true;
    for (const char c : value) {
        if ((c < '0' || c > '9') && c != '.') return false;
    }
    try {
        const double number = std::stod(value);
        return number >= 0.0 && number <= 2.0;
    } catch (const std::exception&) {
        return false;
    }
}

bool ValidPercent(const std::string& value) {
    return value.empty() || WholeNumber(value, 0, 100);
}

GameSettings LoadGameSettings(const std::filesystem::path& file) {
    GameSettings settings;
    std::ifstream stream(file, std::ios::binary);
    if (!stream) return settings;
    const std::string text{std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
    const auto document = nlohmann::json::parse(text, nullptr, false);
    if (document.is_discarded() || !document.is_object()) return settings;
    const auto keep = [&](std::string& target, const char* key, bool valid) {
        if (valid) target = Text(document, key);
    };
    auto value = [&](const char* key) { return Text(document, key); };
    keep(settings.screen, "screen", OneOf(value("screen"), {"borderless"}));
    keep(settings.displayIndex, "displayIndex", value("displayIndex").empty() || WholeNumber(value("displayIndex"), 0, 63));
    keep(settings.presentMode, "presentMode", OneOf(value("presentMode"), {"fifo_latest_ready", "mailbox", "immediate"}));
    keep(settings.fpsLimit, "fpsLimit", ValidFpsLimit(value("fpsLimit")));
    keep(settings.upscale, "upscale", OneOf(value("upscale"), {"fsr"}));
    keep(settings.fsrSharpness, "fsrSharpness", ValidSharpness(value("fsrSharpness")));
    keep(settings.outputResolution, "outputResolution", ValidResolution(value("outputResolution")));
    keep(settings.overlay, "overlay", OneOf(value("overlay"), {"1", "2"}));
    keep(settings.overlayPosition, "overlayPosition", OneOf(value("overlayPosition"), {"tr", "bl", "br"}));
    keep(settings.language, "language", ValidLanguage(value("language")));
    keep(settings.volume, "volume", ValidPercent(value("volume")));
    keep(settings.rumble, "rumble", ValidPercent(value("rumble")));
    if (const auto found = document.find("muteOnFocusLoss"); found != document.end() && found->is_boolean()) settings.muteOnFocusLoss = found->get<bool>();
    if (const auto found = document.find("shaderCache"); found != document.end() && found->is_boolean()) settings.shaderCache = found->get<bool>();
    return settings;
}

void SaveGameSettings(const std::filesystem::path& file, const GameSettings& settings) {
    const nlohmann::json document = {{"version", 1},
                                     {"screen", settings.screen},
                                     {"displayIndex", settings.displayIndex},
                                     {"presentMode", settings.presentMode},
                                     {"fpsLimit", settings.fpsLimit},
                                     {"upscale", settings.upscale},
                                     {"fsrSharpness", settings.fsrSharpness},
                                     {"outputResolution", settings.outputResolution},
                                     {"overlay", settings.overlay},
                                     {"overlayPosition", settings.overlayPosition},
                                     {"language", settings.language},
                                     {"volume", settings.volume},
                                     {"muteOnFocusLoss", settings.muteOnFocusLoss},
                                     {"rumble", settings.rumble},
                                     {"shaderCache", settings.shaderCache}};
    std::error_code error;
    std::filesystem::create_directories(file.parent_path(), error);
    WriteFileAtomically(file, document.dump(2) + "\n");
}

std::vector<std::pair<std::string, std::string>> GameSettingsEnvironment(const GameSettings& settings) {
    std::vector<std::pair<std::string, std::string>> env;
    const auto put = [&](const char* name, const std::string& value) {
        if (!value.empty()) env.emplace_back(name, value);
    };
    put("ANYPS5_FULLSCREEN", settings.screen);
    put("ANYPS5_DISPLAY_INDEX", settings.displayIndex);
    put("ANYPS5_PRESENT_MODE", settings.presentMode);
    if (settings.fpsLimit != "0") put("ANYPS5_FPS_LIMIT", settings.fpsLimit);
    put("ANYPS5_UPSCALE", settings.upscale);
    if (settings.upscale == "fsr") put("ANYPS5_FSR_SHARPNESS", settings.fsrSharpness);
    put("APS5_OUTPUT_RESOLUTION", settings.outputResolution);
    put("ANYPS5_OVERLAY", settings.overlay);
    if (!settings.overlay.empty()) put("ANYPS5_OVERLAY_POS", settings.overlayPosition);
    put("ANYPS5_LANGUAGE", settings.language);
    if (settings.volume != "100") put("ANYPS5_VOLUME", settings.volume);
    if (settings.muteOnFocusLoss) env.emplace_back("ANYPS5_MUTE_ON_FOCUS_LOSS", "1");
    if (settings.rumble != "100") put("ANYPS5_RUMBLE", settings.rumble);
    if (!settings.shaderCache) env.emplace_back("ANYPS5_NO_SHADER_CACHE", "1");
    return env;
}

}
