#include <cstring>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "Fixtures.hpp"
#include "Framework.hpp"

#include "core/InputMapping.hpp"
#include "core/Scanner.hpp"
#include "core/Trophies.hpp"

namespace {

void PutBig(std::vector<std::uint8_t>& out, std::size_t at, std::uint64_t value, int size) {
    for (int i = size - 1; i >= 0; --i) {
        out[at + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(value & 0xFF);
        value >>= 8;
    }
}

// A .ucp as the console writes it: header, table of 0x40-byte entries, file data.
std::vector<std::uint8_t> Ucp(const std::map<std::string, std::vector<std::uint8_t>>& files) {
    const std::size_t toc = 0x20;
    std::size_t data = toc + 0x20 + files.size() * 0x40;
    std::size_t total = data;
    for (const auto& [name, bytes] : files) total += bytes.size();
    std::vector<std::uint8_t> out(total);
    PutBig(out, 0, 0xB228C60A, 4);
    PutBig(out, 4, 1, 4);
    PutBig(out, 8, total, 8);
    PutBig(out, 0x10, files.size(), 4);
    PutBig(out, 0x14, toc, 4);
    std::size_t index = 0;
    for (const auto& [name, bytes] : files) {
        const auto entry = toc + 0x20 + index++ * 0x40;
        std::memcpy(out.data() + entry, name.data(), name.size());
        PutBig(out, entry + 0x20, data, 8);
        PutBig(out, entry + 0x28, bytes.size(), 8);
        std::memcpy(out.data() + data, bytes.data(), bytes.size());
        data += bytes.size();
    }
    return out;
}

const char* const Conf = R"({"defaultLanguage":"en-US","trophies":[
 {"id":"0000","grade":"P","hidden":false},
 {"id":1,"grade":"G","hidden":true},
 {"id":2,"grade":"B","unlockCondition":{"progressive":true,"udsStatId":"7","targetValue":"10"}},
 {"id":3,"grade":"S"}]})";

const char* const MetaEn = R"({"metadata":{"titleMetadata":{"name":"Test Game Trophies"},"trophyMetadata":[
 {"id":0,"name":"All of it","detail":"Every trophy"},
 {"id":1,"name":"Secret","detail":"Find the secret"},
 {"id":2,"name":"Collector","detail":"Collect ten"},
 {"id":3,"name":"Start","detail":"Begin"}]}})";

const char* const MetaFr = R"({"metadata":{"titleMetadata":{"name":"Jeu"},"trophyMetadata":[{"id":3,"name":"Debut","detail":"Commencer"}]}})";

std::map<std::string, std::vector<std::uint8_t>> TrophyFiles() {
    return {{"tropconf.json", fixture::Bytes(Conf)},
            {"tropmeta_en-US.json", fixture::Bytes(MetaEn)},
            {"tropmeta_fr-FR.json", fixture::Bytes(MetaFr)},
            {"trop0003.png", fixture::Bytes("png3")},
            {"icon0.png", fixture::Bytes("set icon")}};
}

void WriteText(const std::filesystem::path& file, const std::string& text) {
    std::filesystem::create_directories(file.parent_path());
    std::ofstream(file, std::ios::binary) << text;
}

}

TEST(ReadsUcpFiles) {
    const auto ucp = Ucp(TrophyFiles());
    const auto files = launcher::ReadUcp(ucp);
    CHECK_EQ(files.size(), std::size_t{5});
    CHECK(files.at("trop0003.png") == fixture::Bytes("png3"));
    auto broken = ucp;
    broken[0] = 0;
    CHECK(launcher::ReadUcp(broken).empty());
    auto truncated = ucp;
    truncated.resize(ucp.size() - 4);
    CHECK(launcher::ReadUcp(truncated).empty());
}

TEST(ParsesTrophyPackage) {
    const auto set = launcher::ParseTrophyPackage(TrophyFiles(), 0, {});
    CHECK(set.has_value());
    CHECK_EQ(set->title, std::string("Test Game Trophies"));
    CHECK_EQ(set->trophies.size(), std::size_t{4});
    CHECK(set->trophies[0].grade == launcher::TrophyGrade::Platinum);
    CHECK(set->trophies[1].hidden);
    CHECK_EQ(set->trophies[2].name, std::string("Collector"));
    CHECK(set->trophies[2].statId.has_value());
    CHECK_EQ(set->trophies[2].target, std::uint64_t{10});
    CHECK(set->trophies[3].icon == fixture::Bytes("png3"));
    CHECK(set->icon == fixture::Bytes("set icon"));

    const auto french = launcher::ParseTrophyPackage(TrophyFiles(), 0, launcher::TrophyLanguages("2"));
    CHECK_EQ(french->trophies[3].name, std::string("Debut"));
    const auto english = launcher::TrophyLanguages("1");
    CHECK_EQ(english.front(), std::string("en-US"));
    CHECK(launcher::TrophyLanguages("auto").empty());
}

TEST(AppliesTrophyRecords) {
    fixture::TempDir temp;
    const auto records = temp.Path() / "_trophy";
    // 2024-01-01T00:00:00Z in the engine's ticks.
    const std::uint64_t tick = 62135596800000000ull + 1704067200ull * 1000000ull;
    WriteText(records / "1_0.txt", "3 " + std::to_string(tick) + "\r\n1 " + std::to_string(tick + 5000000) + "\nbroken line\n");
    WriteText(records / "2_0.txt", "3 " + std::to_string(tick - 1000000) + "\n");
    WriteText(records / "1_0.stats.txt", "7 4\n");
    WriteText(records / "notes.txt", "3 1\n");
    auto set = *launcher::ParseTrophyPackage(TrophyFiles(), 0, {});
    launcher::ApplyTrophyRecords(set, launcher::ReadTrophyRecords(records));
    CHECK(!set.trophies[0].earnedAt);
    CHECK_EQ(*set.trophies[1].earnedAt, std::int64_t{1704067205});
    CHECK_EQ(*set.trophies[3].earnedAt, std::int64_t{1704067199});
    CHECK_EQ(set.trophies[2].progress, std::uint64_t{4});
    CHECK_EQ(set.Count().earned, 2);
    // Gold 90 + silver 30 of gold 90 + bronze 15 + silver 30; the platinum doesn't count.
    CHECK_EQ(set.Percent(), 120 * 100 / 135);
    CHECK_EQ(set.Count(launcher::TrophyGrade::Silver).earned, 1);
}

TEST(LoadsTrophiesFromGamePackage) {
    fixture::TempDir temp;
    auto tree = fixture::Game("PPSA00001", "First");
    tree["sce_sys/trophy2/trophy00.ucp"] = Ucp(TrophyFiles());
    fixture::WriteFolder(temp.Path() / "First", tree);
    fixture::WriteZar(temp.Path() / "Packed.zar", tree);
    const auto result = launcher::Scan({.roots = {temp.Path()}});
    CHECK_EQ(result.games.size(), std::size_t{2});
    for (const auto& game : result.games) {
        auto package = launcher::OpenGamePackage(game);
        const auto set = launcher::LoadTrophies(*package, {});
        CHECK(set.has_value());
        CHECK_EQ(set->trophies.size(), std::size_t{4});
    }
    fixture::TempDir empty;
    fixture::WriteFolder(empty.Path() / "Plain", fixture::Game("PPSA00002", "Plain"));
    auto plain = launcher::OpenGamePackage(launcher::Scan({.roots = {empty.Path()}}).games.at(0));
    CHECK(!launcher::LoadTrophies(*plain, {}).has_value());
}

TEST(ParsesAndWritesInputFile) {
    const auto file = launcher::ParseInputFile(
        "\xEF\xBB\xBF# AnyPS5 input mapping\n# header\n"
        "cross = key:F\nCross = KEY:Space\nCross = KEY:F\n"
        "LeftStickDeadzone = 12 ; worn stick\n"
        "SDLButton:a = Circle\n# keep me\nNotAnAction = KEY:Q\n");
    CHECK_EQ(file.bindings.at("Cross").size(), std::size_t{2});
    CHECK_EQ(file.bindings.at("Cross")[0], std::string("KEY:F"));
    CHECK_EQ(file.settings.at("LeftStickDeadzone"), std::string("12"));
    CHECK_EQ(file.other.size(), std::size_t{3});
    const auto text = launcher::FormatInputFile(file, "# new header");
    CHECK(text.rfind("# new header\n", 0) == 0);
    CHECK(text.find("# header") == std::string::npos);
    CHECK(text.find("Cross = KEY:F\nCross = KEY:Space\n") != std::string::npos);
    CHECK(text.find("SDLButton:a = Circle") != std::string::npos);
    const auto again = launcher::ParseInputFile(text);
    CHECK(again.bindings == file.bindings);
    CHECK(again.settings == file.settings);
    CHECK(again.other == file.other);
}

TEST(EffectiveBindingsAndConflicts) {
    const auto* cross = launcher::FindInputAction("cross");
    CHECK(cross != nullptr);
    launcher::InputFile profile;
    launcher::InputFile game;
    CHECK(launcher::EffectiveBindings(*cross, profile, game) == cross->defaults);
    profile.bindings["Cross"] = {"KEY:F"};
    const auto fromProfile = launcher::EffectiveBindings(*cross, profile, game);
    CHECK_EQ(fromProfile.at(0), std::string("KEY:F"));
    game.bindings["Cross"] = {"KEY:Q"};
    const auto fromGame = launcher::EffectiveBindings(*cross, profile, game);
    CHECK_EQ(fromGame.at(0), std::string("KEY:Q"));
    // F is the right stick's left by default, and Q is L1's.
    const auto conflicts = launcher::InputConflicts(profile, game);
    CHECK(conflicts.count("key:q") == 1);
    CHECK(conflicts.count("key:f") == 0);
    CHECK(launcher::InputConflicts({}, {}).empty());
    CHECK_EQ(launcher::DescribeInputSource("MOUSE:X1"), std::string("Mouse 4"));
    CHECK_EQ(launcher::DescribeInputSource("KEY:Left Shift"), std::string("Left Shift"));
    CHECK_EQ(launcher::FormatSettingValue(*launcher::FindInputSetting("MouseSensitivity"), 2.5), std::string("2.5"));
    CHECK_EQ(launcher::FormatSettingValue(*launcher::FindInputSetting("LeftStickDeadzone"), 120), std::string("90"));
}
