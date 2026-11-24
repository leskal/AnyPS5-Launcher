#pragma once

#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace launcher {

// One game's own settings in a profile, applied at each start as the AnyPS5
// environment variables docs/user/USAGE.md describes. Empty means AnyPS5's
// default, so only what was changed reaches the game.
struct GameSettings {
    std::string screen;            // ANYPS5_FULLSCREEN: "" windowed, "borderless"
    std::string displayIndex;      // ANYPS5_DISPLAY_INDEX: "" the first display, "0".."n"
    std::string presentMode;       // ANYPS5_PRESENT_MODE: "" fifo (vertical sync), "fifo_latest_ready", "mailbox", "immediate"
    std::string fpsLimit;          // ANYPS5_FPS_LIMIT: "" no limit, "30", "59.94", ...
    std::string upscale;           // ANYPS5_UPSCALE: "" bilinear, "fsr"
    std::string fsrSharpness;      // ANYPS5_FSR_SHARPNESS: "" (0.2), "0.0".."2.0"
    std::string outputResolution;  // APS5_OUTPUT_RESOLUTION: "" the display's class, "1920x1080", "3840x2160"
    std::string overlay;           // ANYPS5_OVERLAY: "" off, "1" page 1, "2" page 2
    std::string overlayPosition;   // ANYPS5_OVERLAY_POS: "" top left, "tr", "bl", "br"
    std::string language;          // ANYPS5_LANGUAGE for this game: "" the profile's, "auto", "0".."30"
    std::string volume;            // ANYPS5_VOLUME: "" 100, "0".."100"
    bool muteOnFocusLoss = false;  // ANYPS5_MUTE_ON_FOCUS_LOSS=1
    std::string rumble;            // ANYPS5_RUMBLE: "" 100, "0".."100"
    bool shaderCache = true;       // false: ANYPS5_NO_SHADER_CACHE=1

    bool operator==(const GameSettings&) const = default;
};

// Reads a game's settings; missing or unreadable files give the defaults, and
// a value AnyPS5 would refuse is dropped (so the game still starts).
GameSettings LoadGameSettings(const std::filesystem::path& file);
void SaveGameSettings(const std::filesystem::path& file, const GameSettings& settings);

// The environment for the settings that differ from AnyPS5's defaults.
std::vector<std::pair<std::string, std::string>> GameSettingsEnvironment(const GameSettings& settings);

// Checks a value as AnyPS5 does: empty or one AnyPS5 accepts.
bool ValidFpsLimit(const std::string& value);
bool ValidSharpness(const std::string& value);
bool ValidPercent(const std::string& value);

}
