#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace launcher {

inline constexpr const char* SettingsFileName = "anyps5-launcher.json";

struct Settings {
    std::string anyps5Folder;   // the chosen AnyPS5 version's folder
    std::string anyps5Version;  // its id in <launcher>/anyps5/versions.json
    std::string gamesFolder;
    // Empty means <launcher folder>/library and <launcher folder>/userdata.
    std::string libraryFolder;
    std::string userdataFolder;
    std::string profile;
    std::string theme = "system";  // "system" follows Windows, or "dark" / "light"
    bool onlineInfo = true;        // look up each game's update list on PROSPEROPatches
    bool trophyPopups = true;      // a card over the game when it earns a trophy
    bool trophySound = true;       // and a short chime with it
    bool hideWhilePlaying = false; // minimize the launcher once the game's window is up
    std::string syncFolder;        // saves shared with other PCs; empty: off
    std::string view = "grid";     // "grid" or "home" (a row of tiles)
    bool checkUpdates = true;      // look for a newer launcher at start
    // Relink games with AnyPS5's --to-intel. Unset: on for an Intel CPU, off otherwise.
    std::optional<bool> intelFlag;
};

// Whether installs pass --to-intel: the setting, or else whether the CPU is Intel.
bool UseIntelFlag(const Settings& settings, bool hostIsIntel);

std::filesystem::path EffectiveLibrary(const Settings& settings, const std::filesystem::path& launcherFolder);
std::filesystem::path EffectiveUserdata(const Settings& settings, const std::filesystem::path& launcherFolder);

struct LoadedSettings {
    Settings settings;
    bool existed = false;
    std::string error;
};

LoadedSettings LoadSettings(const std::filesystem::path& file);

void SaveSettings(const std::filesystem::path& file, const Settings& settings);

struct AnyPS5Tools {
    std::optional<std::filesystem::path> relinker;
};

AnyPS5Tools FindAnyPS5Tools(const std::string& anyps5Folder);

}
