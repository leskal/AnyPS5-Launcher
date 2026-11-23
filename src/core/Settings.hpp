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
};

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
