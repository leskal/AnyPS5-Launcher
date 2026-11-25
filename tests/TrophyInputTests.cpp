#include <cstring>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "Fixtures.hpp"
#include "Framework.hpp"

#include "core/Aes.hpp"
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

namespace {

std::vector<std::uint8_t> Hex(std::string_view text) {
    std::vector<std::uint8_t> out;
    for (std::size_t i = 0; i + 1 < text.size(); i += 2) out.push_back(static_cast<std::uint8_t>(std::stoi(std::string(text.substr(i, 2)), nullptr, 16)));
    return out;
}

// What a retail .trp holds: the XML encrypted the way the console does.
std::vector<std::uint8_t> Esfm(const std::string& xml, const launcher::TrophyKey& key, const std::string& commId) {
    std::array<std::uint8_t, 16> id{};
    std::copy(commId.begin(), commId.end(), id.begin());
    const launcher::Aes128 fileKey(launcher::Aes128(key).EncryptBlock(id));
    std::vector<std::uint8_t> plain(xml.begin(), xml.end());
    plain.resize((plain.size() + 15) / 16 * 16, 0);
    std::array<std::uint8_t, 16> previous{};
    for (int i = 0; i < 16; ++i) previous[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(0x30 + i);
    std::vector<std::uint8_t> out(previous.begin(), previous.end());
    for (std::size_t at = 0; at < plain.size(); at += 16) {
        std::array<std::uint8_t, 16> block{};
        for (int i = 0; i < 16; ++i) block[static_cast<std::size_t>(i)] = plain[at + static_cast<std::size_t>(i)] ^ previous[static_cast<std::size_t>(i)];
        previous = fileKey.EncryptBlock(block);
        out.insert(out.end(), previous.begin(), previous.end());
    }
    return out;
}

std::vector<std::uint8_t> Trp(const std::vector<std::pair<std::string, std::pair<std::uint32_t, std::vector<std::uint8_t>>>>& files) {
    std::vector<std::uint8_t> out(0x60 + files.size() * 0x40, 0);
    PutBig(out, 0, 0xDCA24D00, 4);
    PutBig(out, 4, 1, 4);
    PutBig(out, 0x10, files.size(), 4);
    PutBig(out, 0x14, 0x40, 4);
    for (std::size_t i = 0; i < files.size(); ++i) {
        const auto entry = 0x60 + i * 0x40;
        std::memcpy(out.data() + entry, files[i].first.data(), files[i].first.size());
        PutBig(out, entry + 32, out.size(), 8);
        PutBig(out, entry + 40, files[i].second.second.size(), 8);
        PutBig(out, entry + 48, files[i].second.first, 4);
        out.insert(out.end(), files[i].second.second.begin(), files[i].second.second.end());
    }
    PutBig(out, 8, out.size(), 8);
    return out;
}

}

TEST(Aes128MatchesTheStandard) {
    const auto key = Hex("000102030405060708090a0b0c0d0e0f");
    const auto plain = Hex("00112233445566778899aabbccddeeff");
    const launcher::Aes128 aes(std::span<const std::uint8_t, 16>(key.data(), 16));
    const auto cipher = aes.EncryptBlock(std::span<const std::uint8_t, 16>(plain.data(), 16));
    CHECK(std::vector<std::uint8_t>(cipher.begin(), cipher.end()) == Hex("69c4e0d86a7b0430d8cdb78070b4c55a"));  // FIPS-197 C.1
    const auto back = aes.DecryptBlock(cipher);
    CHECK(std::vector<std::uint8_t>(back.begin(), back.end()) == plain);
    // NIST SP 800-38A F.2.2, CBC-AES128 decryption, two blocks.
    const auto cbcKey = Hex("2b7e151628aed2a6abf7158809cf4f3c");
    const auto iv = Hex("000102030405060708090a0b0c0d0e0f");
    const launcher::Aes128 cbc(std::span<const std::uint8_t, 16>(cbcKey.data(), 16));
    const auto out = cbc.DecryptCbc(Hex("7649abac8119b246cee98e9b12e9197d5086cb9b507219ee95db113a917678b2"), std::span<const std::uint8_t, 16>(iv.data(), 16));
    CHECK(out == Hex("6bc1bee22e409f96e93d7e117393172aae2d8a571e03ac9c9eb76fac45af8e51"));
}

TEST(ReadsPs4TrophiesWithTheKey) {
    CHECK(!launcher::ParseTrophyKey("1234").has_value());
    CHECK(!launcher::ParseTrophyKey("zz112233445566778899aabbccddeeff").has_value());
    const auto key = *launcher::ParseTrophyKey("00112233-44556677 8899AABB:ccddeeff");
    CHECK_EQ(key[0], std::uint8_t{0x00});
    CHECK_EQ(key[15], std::uint8_t{0xff});
    const std::string commId = "NPWR01234_00";
    const std::string xml = R"(<?xml version="1.0" encoding="UTF-8"?>
<trophyconf version="1.1" platform="ps4" policy="large">
<npcommid>NPWR01234_00</npcommid>
<title-name>Sword &amp; Shield</title-name>
<trophy id="000" hidden="no" ttype="P" pid="000"><name>All of them</name><detail>Every trophy</detail></trophy>
<trophy id="001" hidden="yes" ttype="B"><name>A secret</name><detail>Found it</detail></trophy>
</trophyconf>)";
    const auto english = std::string(xml).replace(xml.find("All of them"), 11, "Everything!");
    const auto png = fixture::Bytes("\x89PNG fake");
    fixture::TempDir temp;
    auto tree = fixture::Game("CUSA01234", "PS4 game");
    tree["sce_sys/npbind.dat"] = fixture::Bytes(std::string("\xD2\x94\xA0\x18 header ") + commId + std::string(8, '\0') + "NPWR01234_00 repeated");
    tree["sce_sys/trophy/trophy00.trp"] = Trp({{"ICON0.PNG", {0, png}},
                                               {"TROP000.PNG", {0, png}},
                                               {"TROP001.PNG", {0, png}},
                                               {"TROP.ESFM", {3, Esfm(xml, key, commId)}},
                                               {"TROP_01.ESFM", {3, Esfm(english, key, commId)}}});
    fixture::WriteFolder(temp.Path() / "Game", tree);
    const auto game = launcher::Scan({.roots = {temp.Path()}}).games.at(0);
    auto package = launcher::OpenGamePackage(game);
    CHECK(launcher::NpCommIds(*package->ReadAll("sce_sys/npbind.dat", 4096)) == std::vector<std::string>{commId});

    // With the key: names, grades, hidden, icons; the profile's language first.
    auto set = launcher::LoadTrophies(*package, {}, key);
    CHECK(set.has_value() && !set->encrypted);
    CHECK_EQ(set->title, std::string("Sword & Shield"));
    CHECK_EQ(set->trophies.size(), std::size_t{2});
    CHECK_EQ(set->trophies[0].name, std::string("All of them"));
    CHECK(set->trophies[0].grade == launcher::TrophyGrade::Platinum);
    CHECK(set->trophies[1].hidden);
    CHECK(set->trophies[1].icon == png);
    CHECK(set->icon == png);
    set = launcher::LoadTrophies(*package, {"en-US"}, key);
    CHECK_EQ(set->trophies[0].name, std::string("Everything!"));

    // Without it (or with a wrong one): how many there are, nothing more.
    for (const auto& other : {std::optional<launcher::TrophyKey>{}, launcher::ParseTrophyKey("ffffffffffffffffffffffffffffffff")}) {
        const auto locked = launcher::LoadTrophies(*package, {}, other);
        CHECK(locked.has_value() && locked->encrypted);
        CHECK_EQ(locked->lockedCount, 2);
        CHECK(locked->trophies.empty());
        CHECK(locked->icon == png);
    }
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
