#include "core/Metadata.hpp"

#include <algorithm>
#include <cstring>
#include <map>
#include <stdexcept>

#include <nlohmann/json.hpp>

#include "core/I18n.hpp"

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

    if (const auto id = document.find("contentId"); id != document.end() && id->is_string()) result.contentId = id->get<std::string>();

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
    result.contentId = text("CONTENT_ID");
    result.contentVersion = text("APP_VER");
    if (result.contentVersion.empty()) result.contentVersion = text("VERSION");
    return result;
}

namespace {

}

std::string DecodeXmlEntities(std::string_view text) {
    std::string out;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] != '&') {
            out += text[i];
            continue;
        }
        const auto end = text.find(';', i);
        if (end == std::string_view::npos || end - i > 10) {
            out += '&';
            continue;
        }
        const auto name = text.substr(i + 1, end - i - 1);
        if (name == "amp") out += '&';
        else if (name == "lt") out += '<';
        else if (name == "gt") out += '>';
        else if (name == "quot") out += '"';
        else if (name == "apos") out += '\'';
        else if (!name.empty() && name[0] == '#') {
            unsigned long code = 0;
            try {
                code = name.size() > 1 && (name[1] == 'x' || name[1] == 'X') ? std::stoul(std::string(name.substr(2)), nullptr, 16) : std::stoul(std::string(name.substr(1)));
            } catch (const std::exception&) {
                out += '&';
                continue;
            }
            if (code < 0x80) out += static_cast<char>(code);
            else if (code < 0x800) {
                out += static_cast<char>(0xC0 | (code >> 6));
                out += static_cast<char>(0x80 | (code & 0x3F));
            } else if (code < 0x10000) {
                out += static_cast<char>(0xE0 | (code >> 12));
                out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
                out += static_cast<char>(0x80 | (code & 0x3F));
            } else if (code < 0x110000) {
                out += static_cast<char>(0xF0 | (code >> 18));
                out += static_cast<char>(0x80 | ((code >> 12) & 0x3F));
                out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
                out += static_cast<char>(0x80 | (code & 0x3F));
            }
        } else {
            out += '&';
            continue;
        }
        i = end;
    }
    return out;
}

namespace {

// Trims each line and drops blank lines at the start and end.
std::string TidyLines(std::string_view text) {
    std::string out;
    std::size_t start = 0;
    bool any = false;
    std::string pendingBlank;
    while (start <= text.size()) {
        auto end = text.find('\n', start);
        if (end == std::string_view::npos) end = text.size();
        auto line = text.substr(start, end - start);
        while (!line.empty() && (line.front() == ' ' || line.front() == '\t' || line.front() == '\r')) line.remove_prefix(1);
        while (!line.empty() && (line.back() == ' ' || line.back() == '\t' || line.back() == '\r')) line.remove_suffix(1);
        if (line.empty()) {
            if (any) pendingBlank = "\n";
        } else {
            if (any) out += "\n" + pendingBlank;
            pendingBlank.clear();
            out += line;
            any = true;
        }
        start = end + 1;
    }
    return out;
}

std::vector<unsigned long> VersionParts(std::string_view text) {
    std::vector<unsigned long> parts;
    unsigned long value = 0;
    bool digits = false;
    for (const char c : text) {
        if (c >= '0' && c <= '9') {
            value = value * 10 + static_cast<unsigned long>(c - '0');
            digits = true;
        } else if (c == '.') {
            parts.push_back(value);
            value = 0;
            digits = false;
        } else {
            break;
        }
    }
    if (digits || !parts.empty()) parts.push_back(value);
    while (parts.size() > 1 && parts.back() == 0) parts.pop_back();
    return parts;
}

}

std::string RegionOf(std::string_view contentId) {
    if (contentId.size() < 2) return {};
    const auto prefix = contentId.substr(0, 2);
    if (prefix == "EP") return N("Europe");
    if (prefix == "UP") return N("USA");
    if (prefix == "JP") return N("Japan");
    if (prefix == "HP") return N("Asia");
    if (prefix == "KP") return N("Korea");
    if (prefix == "IP") return N("Internal");
    return {};
}

std::string PlatformOf(std::string_view titleId) {
    if (titleId.size() < 9) return {};
    const auto prefix = titleId.substr(0, 3);
    if (prefix == "PPS") return "PS5";
    if (prefix == "CUS" || prefix == "PLA" || prefix == "PCJ" || prefix == "PCA" || prefix == "PCE" || prefix == "PCK") return "PS4";
    return {};
}

std::vector<ChangeNote> ParseChangeInfo(std::string_view xml) {
    std::vector<ChangeNote> notes;
    std::size_t at = 0;
    while ((at = xml.find("<changes", at)) != std::string_view::npos) {
        const auto close = xml.find('>', at);
        if (close == std::string_view::npos) break;
        const auto tag = xml.substr(at, close - at);
        ChangeNote note;
        if (const auto attribute = tag.find("app_ver"); attribute != std::string_view::npos) {
            const auto quote = tag.find_first_of("\"'", attribute);
            if (quote != std::string_view::npos) {
                const auto end = tag.find(tag[quote], quote + 1);
                if (end != std::string_view::npos) note.version = std::string(tag.substr(quote + 1, end - quote - 1));
            }
        }
        if (tag.back() == '/') {
            at = close + 1;
            continue;
        }
        const auto finish = xml.find("</changes>", close);
        if (finish == std::string_view::npos) break;
        auto body = xml.substr(close + 1, finish - close - 1);
        std::string text;
        if (const auto cdata = body.find("<![CDATA["); cdata != std::string_view::npos) {
            const auto end = body.find("]]>", cdata);
            text = std::string(body.substr(cdata + 9, end == std::string_view::npos ? std::string_view::npos : end - cdata - 9));
        } else {
            text = DecodeXmlEntities(body);
        }
        note.text = TidyLines(text);
        if (!note.text.empty()) notes.push_back(std::move(note));
        at = finish + 10;
    }
    std::stable_sort(notes.begin(), notes.end(), [](const ChangeNote& a, const ChangeNote& b) { return CompareVersions(a.version, b.version) > 0; });
    return notes;
}

const ChangeNote* ChangeNoteFor(const std::vector<ChangeNote>& notes, std::string_view contentVersion) {
    for (const auto& note : notes) {
        if (CompareVersions(note.version, contentVersion) == 0) return &note;
    }
    return nullptr;
}

int CompareVersions(std::string_view a, std::string_view b) {
    const auto left = VersionParts(a);
    const auto right = VersionParts(b);
    const auto count = std::max(left.size(), right.size());
    for (std::size_t i = 0; i < count; ++i) {
        const auto x = i < left.size() ? left[i] : 0;
        const auto y = i < right.size() ? right[i] : 0;
        if (x != y) return x < y ? -1 : 1;
    }
    return 0;
}

}
