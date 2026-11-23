#include "core/Metadata.hpp"

#include <algorithm>
#include <cstring>
#include <map>
#include <stdexcept>

#include <nlohmann/json.hpp>

namespace launcher {

namespace {

constexpr std::size_t SfoHeaderSize = 20;
constexpr std::size_t SfoEntrySize = 16;
constexpr std::uint16_t SfoUtf8 = 0x0204;

std::uint16_t ReadU16(std::span<const std::uint8_t> bytes, std::size_t offset) {
    if (offset + 2 > bytes.size()) throw std::runtime_error("param.sfo is truncated");
    return static_cast<std::uint16_t>(bytes[offset] | (bytes[offset + 1] << 8));
}

std::uint32_t ReadU32(std::span<const std::uint8_t> bytes, std::size_t offset) {
    if (offset + 4 > bytes.size()) throw std::runtime_error("param.sfo is truncated");
    return static_cast<std::uint32_t>(bytes[offset]) | (static_cast<std::uint32_t>(bytes[offset + 1]) << 8) |
           (static_cast<std::uint32_t>(bytes[offset + 2]) << 16) | (static_cast<std::uint32_t>(bytes[offset + 3]) << 24);
}

std::string TitleNameFor(const nlohmann::json& localized, const std::string& locale) {
    if (locale.empty()) return {};
    const auto entry = localized.find(locale);
    if (entry == localized.end() || !entry->is_object()) return {};
    const auto name = entry->find("titleName");
    if (name == entry->end() || !name->is_string()) return {};
    auto text = name->get<std::string>();
    if (std::all_of(text.begin(), text.end(), [](unsigned char c) { return c == ' ' || c == '\t'; })) return {};
    return text;
}

}

bool IsTitleId(std::string_view text) {
    if (text.size() != 9) return false;
    for (std::size_t index = 0; index < 4; ++index) {
        if (text[index] < 'A' || text[index] > 'Z') return false;
    }
    for (std::size_t index = 4; index < 9; ++index) {
        if (text[index] < '0' || text[index] > '9') return false;
    }
    return true;
}

GameMetadata ParseParamJson(std::span<const std::uint8_t> bytes, std::string_view preferredLocale) {
    auto begin = reinterpret_cast<const char*>(bytes.data());
    auto end = begin + bytes.size();
    if (bytes.size() >= 3 && bytes[0] == 0xEF && bytes[1] == 0xBB && bytes[2] == 0xBF) begin += 3;
    const auto document = nlohmann::json::parse(begin, end, nullptr, false);
    if (document.is_discarded()) throw std::runtime_error("param.json is not valid JSON");
    if (!document.is_object()) throw std::runtime_error("param.json is not an object");

    GameMetadata result;
    result.source = "param.json";
    const auto titleId = document.find("titleId");
    if (titleId == document.end() || !titleId->is_string()) throw std::runtime_error("param.json has no titleId");
    result.titleId = titleId->get<std::string>();
    if (!IsTitleId(result.titleId)) throw std::runtime_error("param.json titleId \"" + result.titleId + "\" is not a title ID");

    if (const auto version = document.find("contentVersion"); version != document.end()) {
        if (!version->is_string()) throw std::runtime_error("param.json contentVersion is not a string");
        result.contentVersion = version->get<std::string>();
    }

    if (const auto localized = document.find("localizedParameters"); localized != document.end()) {
        if (!localized->is_object()) throw std::runtime_error("param.json localizedParameters is not an object");
        std::string defaultLanguage = "en-US";
        if (const auto language = localized->find("defaultLanguage"); language != localized->end()) {
            if (!language->is_string()) throw std::runtime_error("param.json defaultLanguage is not a string");
            defaultLanguage = language->get<std::string>();
        }
        for (const auto& locale : {std::string(preferredLocale), defaultLanguage, std::string("en-US")}) {
            result.name = TitleNameFor(*localized, locale);
            if (!result.name.empty()) break;
        }
        if (result.name.empty()) {
            for (const auto& [locale, value] : localized->items()) {
                result.name = TitleNameFor(*localized, locale);
                if (!result.name.empty()) break;
            }
        }
    }
    if (result.name.empty()) result.name = result.titleId;
    return result;
}

GameMetadata ParseParamSfo(std::span<const std::uint8_t> bytes) {
    if (bytes.size() < SfoHeaderSize || std::memcmp(bytes.data(), "\0PSF", 4) != 0) throw std::runtime_error("param.sfo has no PSF header");
    const std::uint64_t keyTable = ReadU32(bytes, 8);
    const std::uint64_t dataTable = ReadU32(bytes, 12);
    const std::uint64_t count = ReadU32(bytes, 16);
    if (count > (bytes.size() - SfoHeaderSize) / SfoEntrySize) throw std::runtime_error("param.sfo tables are out of bounds");

    std::map<std::string, std::string> strings;
    for (std::uint64_t index = 0; index < count; ++index) {
        const auto entry = SfoHeaderSize + index * SfoEntrySize;
        const auto keyOffset = keyTable + ReadU16(bytes, entry);
        const auto format = ReadU16(bytes, entry + 2);
        const std::uint64_t length = ReadU32(bytes, entry + 4);
        const auto dataOffset = dataTable + ReadU32(bytes, entry + 12);
        if (keyOffset >= bytes.size() || dataOffset > bytes.size() || length > bytes.size() - dataOffset) throw std::runtime_error("param.sfo entry is out of bounds");
        const auto keyBegin = bytes.begin() + static_cast<std::ptrdiff_t>(keyOffset);
        const auto keyEnd = std::find(keyBegin, bytes.end(), std::uint8_t{0});
        if (keyEnd == bytes.end()) throw std::runtime_error("param.sfo key is not terminated");
        std::string key(keyBegin, keyEnd);
        if (format != SfoUtf8) continue;
        const auto valueBegin = bytes.begin() + static_cast<std::ptrdiff_t>(dataOffset);
        const auto valueEnd = std::find(valueBegin, valueBegin + static_cast<std::ptrdiff_t>(length), std::uint8_t{0});
        strings.emplace(std::move(key), std::string(valueBegin, valueEnd));
    }

    const auto text = [&](const char* key) -> std::string {
        const auto found = strings.find(key);
        return found == strings.end() ? std::string{} : found->second;
    };
    GameMetadata result;
    result.source = "param.sfo";
    result.titleId = text("TITLE_ID");
    if (result.titleId.empty()) throw std::runtime_error("param.sfo is missing TITLE_ID");
    if (!IsTitleId(result.titleId)) throw std::runtime_error("param.sfo TITLE_ID \"" + result.titleId + "\" is not a title ID");
    result.name = text("TITLE");
    if (result.name.empty()) result.name = result.titleId;
    result.contentVersion = text("APP_VER");
    if (result.contentVersion.empty()) result.contentVersion = text("VERSION");
    return result;
}

}
