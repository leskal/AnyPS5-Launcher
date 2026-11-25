#include <fstream>
#include <iterator>

#include "Fixtures.hpp"
#include "Framework.hpp"

#include "core/Settings.hpp"
#include "core/Utf8.hpp"

using launcher::LoadSettings;
using launcher::SaveSettings;

namespace {

std::string ReadText(const std::filesystem::path& file) {
    std::ifstream stream(file, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

void WriteText(const std::filesystem::path& file, const std::string& text) {
    std::ofstream(file, std::ios::binary) << text;
}

}

TEST(MissingSettingsFileGivesDefaults) {
    fixture::TempDir temp;
    const auto loaded = LoadSettings(temp.Path() / "anyps5-launcher.json");
    CHECK(!loaded.existed);
    CHECK(loaded.error.empty());
    CHECK(loaded.settings.gamesFolder.empty());
}

TEST(SettingsRoundTripAndKeepUnknownKeys) {
    fixture::TempDir temp;
    const auto file = temp.Path() / "anyps5-launcher.json";
    WriteText(file, R"({"futureOption": [1, 2], "python": "py"})");
    launcher::Settings settings;
    settings.anyps5Folder = "C:\\dev\\AnyPS5";
    settings.gamesFolder = "D:\\Παιχνίδια";
    SaveSettings(file, settings);
    const auto loaded = LoadSettings(file);
    CHECK(loaded.existed);
    CHECK(loaded.error.empty());
    CHECK_EQ(loaded.settings.anyps5Folder, settings.anyps5Folder);
    CHECK_EQ(loaded.settings.gamesFolder, settings.gamesFolder);
    CHECK(ReadText(file).find("python") == std::string::npos);
    CHECK(ReadText(file).find("futureOption") != std::string::npos);
    CHECK(!std::filesystem::exists(temp.Path() / "anyps5-launcher.json.tmp"));
}

TEST(InvalidSettingsFileReportsError) {
    fixture::TempDir temp;
    const auto file = temp.Path() / "anyps5-launcher.json";
    WriteText(file, "{ broken");
    auto loaded = LoadSettings(file);
    CHECK(loaded.existed);
    CHECK(!loaded.error.empty());
    CHECK(loaded.settings.gamesFolder.empty());
    WriteText(file, R"({"gamesFolder": 5, "anyps5Folder": "X"})");
    loaded = LoadSettings(file);
    CHECK(loaded.error.find("gamesFolder") != std::string::npos);
    CHECK_EQ(loaded.settings.anyps5Folder, std::string("X"));
}

TEST(FindsAnyPS5Tools) {
    fixture::TempDir temp;
    CHECK(!launcher::FindAnyPS5Tools(launcher::ToUtf8(temp.Path())).relinker);
#ifdef _WIN32
    const char* relinker = "build/core/relinker/relinker.exe";
#else
    const char* relinker = "build/core/relinker/relinker";
#endif
    fixture::WriteFolder(temp.Path(), {{relinker, fixture::Bytes("")}});
    const auto tools = launcher::FindAnyPS5Tools(launcher::ToUtf8(temp.Path()));
    CHECK(tools.relinker.has_value());
    CHECK(!launcher::FindAnyPS5Tools("").relinker);
}

TEST(IntelFlagFollowsTheCpuUnlessSet) {
    fixture::TempDir temp;
    const auto file = temp.Path() / "anyps5-launcher.json";
    launcher::Settings settings;
    CHECK(launcher::UseIntelFlag(settings, true));
    CHECK(!launcher::UseIntelFlag(settings, false));
    SaveSettings(file, settings);
    CHECK(ReadText(file).find("intelFlag") == std::string::npos);
    CHECK(!LoadSettings(file).settings.intelFlag.has_value());

    settings.intelFlag = false;
    SaveSettings(file, settings);
    auto loaded = LoadSettings(file);
    CHECK(loaded.settings.intelFlag.has_value());
    CHECK(!launcher::UseIntelFlag(loaded.settings, true));
    settings.intelFlag = true;
    SaveSettings(file, settings);
    CHECK(launcher::UseIntelFlag(LoadSettings(file).settings, false));

    settings.intelFlag.reset();  // back to the default: the key goes
    SaveSettings(file, settings);
    CHECK(ReadText(file).find("intelFlag") == std::string::npos);

    WriteText(file, R"({"intelFlag": "yes"})");
    loaded = LoadSettings(file);
    CHECK(!loaded.settings.intelFlag.has_value());
    CHECK(!loaded.error.empty());
}

TEST(Ps5AndPs4GamesFolders) {
    fixture::TempDir temp;
    const auto file = temp.Path() / "anyps5-launcher.json";
    launcher::Settings settings;
    CHECK(launcher::GameFolders(settings).empty());
    settings.gamesFolder = (temp.Path() / "PS5").string();
    settings.ps4GamesFolder = (temp.Path() / "PS4").string();
    SaveSettings(file, settings);
    const auto loaded = LoadSettings(file).settings;
    CHECK_EQ(loaded.ps4GamesFolder, settings.ps4GamesFolder);
    CHECK(launcher::GameFolders(loaded) == (std::vector<std::filesystem::path>{temp.Path() / "PS5", temp.Path() / "PS4"}));

    // Only PS4, the same folder twice, or one inside the other: no game shows twice.
    settings.gamesFolder.clear();
    CHECK(launcher::GameFolders(settings) == (std::vector<std::filesystem::path>{temp.Path() / "PS4"}));
    settings.gamesFolder = settings.ps4GamesFolder;
    CHECK_EQ(launcher::GameFolders(settings).size(), std::size_t{1});
    settings.gamesFolder = (temp.Path() / "Games").string();
    settings.ps4GamesFolder = (temp.Path() / "Games" / "PS4").string();
    CHECK(launcher::GameFolders(settings) == (std::vector<std::filesystem::path>{temp.Path() / "Games"}));
    settings.gamesFolder = (temp.Path() / "Games" / "PS5").string();
    settings.ps4GamesFolder = (temp.Path() / "Games").string();
    CHECK(launcher::GameFolders(settings) == (std::vector<std::filesystem::path>{temp.Path() / "Games"}));
}
