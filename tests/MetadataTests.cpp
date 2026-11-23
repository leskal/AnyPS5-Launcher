#include <cstring>

#include "Fixtures.hpp"
#include "Framework.hpp"

#include "core/Metadata.hpp"

using launcher::ParseParamJson;
using launcher::ParseParamSfo;

namespace {

std::vector<std::uint8_t> Sfo(const std::vector<std::pair<std::string, std::string>>& strings) {
    std::vector<std::uint8_t> keys;
    std::vector<std::uint8_t> data;
    std::vector<std::uint8_t> entries;
    const auto put16 = [](std::vector<std::uint8_t>& out, std::uint32_t value) {
        out.push_back(static_cast<std::uint8_t>(value));
        out.push_back(static_cast<std::uint8_t>(value >> 8));
    };
    const auto put32 = [&](std::vector<std::uint8_t>& out, std::uint32_t value) {
        put16(out, value & 0xFFFF);
        put16(out, value >> 16);
    };
    for (const auto& [key, value] : strings) {
        const auto keyOffset = static_cast<std::uint32_t>(keys.size());
        keys.insert(keys.end(), key.begin(), key.end());
        keys.push_back(0);
        const auto dataOffset = static_cast<std::uint32_t>(data.size());
        data.insert(data.end(), value.begin(), value.end());
        data.push_back(0);
        put16(entries, keyOffset);
        put16(entries, 0x0204);
        put32(entries, static_cast<std::uint32_t>(value.size() + 1));
        put32(entries, static_cast<std::uint32_t>(value.size() + 1));
        put32(entries, dataOffset);
    }
    std::vector<std::uint8_t> file{0, 'P', 'S', 'F', 1, 1, 0, 0};
    const auto keyTable = static_cast<std::uint32_t>(20 + entries.size());
    put32(file, keyTable);
    put32(file, static_cast<std::uint32_t>(keyTable + keys.size()));
    put32(file, static_cast<std::uint32_t>(strings.size()));
    file.insert(file.end(), entries.begin(), entries.end());
    file.insert(file.end(), keys.begin(), keys.end());
    file.insert(file.end(), data.begin(), data.end());
    return file;
}

}

TEST(ParamJsonReadsTitleNameAndVersion) {
    const auto metadata = ParseParamJson(fixture::Bytes(fixture::ParamJson("PPSA01234", "Some Game", "01.002.000")));
    CHECK_EQ(metadata.titleId, std::string("PPSA01234"));
    CHECK_EQ(metadata.name, std::string("Some Game"));
    CHECK_EQ(metadata.contentVersion, std::string("01.002.000"));
    CHECK_EQ(metadata.source, std::string("param.json"));
}

TEST(ParamJsonPrefersRequestedLocaleThenDefaultThenEnglish) {
    const std::string json = R"({"titleId":"PPSA01234","localizedParameters":{
        "defaultLanguage":"ja-JP",
        "ja-JP":{"titleName":"ゲーム"},
        "en-US":{"titleName":"Game"},
        "el-GR":{"titleName":"Παιχνίδι"}}})";
    CHECK_EQ(ParseParamJson(fixture::Bytes(json), "el-GR").name, std::string("Παιχνίδι"));
    CHECK_EQ(ParseParamJson(fixture::Bytes(json), "fr-FR").name, std::string("ゲーム"));
    const std::string noDefault = R"({"titleId":"PPSA01234","localizedParameters":{
        "defaultLanguage":"ja-JP","en-US":{"titleName":"Game"}}})";
    CHECK_EQ(ParseParamJson(fixture::Bytes(noDefault)).name, std::string("Game"));
    const std::string onlyOther = R"({"titleId":"PPSA01234","localizedParameters":{"de-DE":{"titleName":"Spiel"}}})";
    CHECK_EQ(ParseParamJson(fixture::Bytes(onlyOther)).name, std::string("Spiel"));
}

TEST(ParamJsonFallsBackToTitleIdForBlankName) {
    const std::string json = R"({"titleId":"PPSA01234","localizedParameters":{"en-US":{"titleName":"  "}}})";
    CHECK_EQ(ParseParamJson(fixture::Bytes(json)).name, std::string("PPSA01234"));
}

TEST(ParamJsonAcceptsUtf8Bom) {
    auto bytes = fixture::Bytes("\xEF\xBB\xBF" + fixture::ParamJson("PPSA01234", "Game"));
    CHECK_EQ(ParseParamJson(bytes).titleId, std::string("PPSA01234"));
}

TEST(ParamJsonRejectsBadInput) {
    CHECK_THROWS(ParseParamJson(fixture::Bytes("{")));
    CHECK_THROWS(ParseParamJson(fixture::Bytes("[]")));
    CHECK_THROWS(ParseParamJson(fixture::Bytes(R"({"contentVersion":"1"})")));
    CHECK_THROWS(ParseParamJson(fixture::Bytes(fixture::ParamJson("ppsa01234", "Game"))));
    CHECK_THROWS(ParseParamJson(fixture::Bytes(R"({"titleId":"PPSA01234","localizedParameters":[]})")));
}

TEST(ParamSfoReadsTitleAndVersion) {
    const auto metadata = ParseParamSfo(Sfo({{"APP_VER", "01.05"}, {"TITLE", "Old Game"}, {"TITLE_ID", "CUSA12345"}}));
    CHECK_EQ(metadata.titleId, std::string("CUSA12345"));
    CHECK_EQ(metadata.name, std::string("Old Game"));
    CHECK_EQ(metadata.contentVersion, std::string("01.05"));
    CHECK_EQ(metadata.source, std::string("param.sfo"));
}

TEST(ParamSfoRejectsBadInput) {
    CHECK_THROWS(ParseParamSfo(fixture::Bytes("not an sfo file at all")));
    CHECK_THROWS(ParseParamSfo(Sfo({{"TITLE", "No Id"}})));
    CHECK_THROWS(ParseParamSfo(Sfo({{"TITLE_ID", "12345"}})));
    auto truncated = Sfo({{"TITLE_ID", "CUSA12345"}});
    truncated.resize(30);
    CHECK_THROWS(ParseParamSfo(truncated));
}
