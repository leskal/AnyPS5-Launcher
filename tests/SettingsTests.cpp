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
