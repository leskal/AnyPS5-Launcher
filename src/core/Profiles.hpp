#pragma once

#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace launcher {

inline constexpr const char* DefaultProfileName = "Player";

// Where everything lives. userdata is what the user backs up; library can be
// rebuilt at any time.
//
//   <userdata>/profiles/<profile>/profile.json
//   <userdata>/profiles/<profile>/saves/<serial>/_sd, _sd_mem
//   <userdata>/profiles/<profile>/_trophy/<serial>
//   <userdata>/profiles/<profile>/input/anyps5-input.ini (+ anyps5-input.<serial>.ini)
//   <userdata>/profiles/<profile>/screenshots/<serial>
//   <userdata>/games/<serial>/data, download0, anyps5-entitlements.ini
//   <library>/<serial>/                      <serial>.exe and folder links only:
//                                            app0 -> game folder, libs -> AnyPS5 libs,
//                                            _sd, _sd_mem, _trophy, data, download0 -> profile
//   <library>/logs/<serial>/                 install log, run logs, run reports
//   <library>/shader-cache/<serial>/
struct Layout {
    std::filesystem::path userdata;
    std::filesystem::path library;
    std::string profile;

    std::filesystem::path ProfilesRoot() const { return userdata / "profiles"; }
    std::filesystem::path Profile() const;
    std::filesystem::path ProfileFile() const { return Profile() / "profile.json"; }
    std::filesystem::path Saves(std::string_view serial) const { return Profile() / "saves" / std::string(serial); }
    std::filesystem::path Trophies(std::string_view serial) const { return Profile() / "_trophy" / std::string(serial); }
    std::filesystem::path InputConfig() const { return Profile() / "input" / "anyps5-input.ini"; }
    // AnyPS5 reads it after InputConfig() for that title only.
    std::filesystem::path GameInputConfig(std::string_view serial) const { return Profile() / "input" / ("anyps5-input." + std::string(serial) + ".ini"); }
    std::filesystem::path Screenshots(std::string_view serial) const { return Profile() / "screenshots" / std::string(serial); }
    std::filesystem::path GameData(std::string_view serial) const { return userdata / "games" / std::string(serial); }
    std::filesystem::path Logs(std::string_view serial) const { return library / "logs" / std::string(serial); }
    std::filesystem::path ShaderCache(std::string_view serial) const { return library / "shader-cache" / std::string(serial); }
};

struct ConsoleLanguage {
    int number;
    const char* name;
};

std::span<const ConsoleLanguage> ConsoleLanguages();

struct ProfileSettings {
    // Empty: leave ANYPS5_LANGUAGE unset (English, United States). Otherwise "auto" or 0 to 30.
    std::string language;
};

bool IsValidProfileName(std::string_view name);
std::vector<std::string> ListProfiles(const std::filesystem::path& userdata);
void CreateProfile(const Layout& layout);
ProfileSettings LoadProfile(const Layout& layout);
void SaveProfile(const Layout& layout, const ProfileSettings& settings);

}
