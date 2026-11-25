#include "core/Trophies.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <system_error>

#include <nlohmann/json.hpp>

#include "core/Aes.hpp"
#include "core/Metadata.hpp"
#include "core/Package.hpp"
#include "core/Utf8.hpp"

namespace launcher {

namespace {

constexpr std::uint64_t UcpMagic = 0xB228C60A;
constexpr std::uint64_t UcpHeaderSize = 0x20;
constexpr std::uint64_t UcpTocHeaderSize = 0x20;
constexpr std::uint64_t UcpEntrySize = 0x40;
constexpr std::uint64_t UcpNameSize = 0x20;
constexpr std::uint64_t UcpMaxEntries = 4096;
constexpr std::uint64_t UcpMaxSize = 256ull << 20;
constexpr std::uint64_t RecordMaxSize = 4u << 20;
// The engine's ticks are microseconds since 1 January of year 1.
constexpr std::uint64_t UnixEpochTick = 62135596800000000ull;
constexpr std::string_view RecordSuffix = ".txt";
constexpr std::string_view StatsSuffix = ".stats.txt";

std::optional<std::uint64_t> BigEndian(std::span<const std::uint8_t> data, std::uint64_t offset, std::uint64_t size) {
    if (offset > data.size() || size > data.size() - offset) return std::nullopt;
    std::uint64_t value = 0;
    for (std::uint64_t i = 0; i < size; ++i) value = value << 8 | data[offset + i];
    return value;
}

nlohmann::json Json(const std::map<std::string, std::vector<std::uint8_t>>& files, const std::string& name) {
    const auto found = files.find(name);
    if (found == files.end()) return {};
    auto begin = found->second.begin();
    if (found->second.size() >= 3 && found->second[0] == 0xEF && found->second[1] == 0xBB && found->second[2] == 0xBF) begin += 3;
    auto document = nlohmann::json::parse(begin, found->second.end(), nullptr, false);
    return document.is_discarded() ? nlohmann::json() : document;
}

// Ids and stat numbers come as "0007" strings or as plain numbers.
std::optional<std::uint64_t> Unsigned(const nlohmann::json& value) {
    if (value.is_number_unsigned()) return value.get<std::uint64_t>();
    if (value.is_number_integer()) {
        const auto number = value.get<std::int64_t>();
        return number < 0 ? std::nullopt : std::optional<std::uint64_t>(static_cast<std::uint64_t>(number));
    }
    if (value.is_string()) {
        const auto& text = value.get_ref<const std::string&>();
        std::uint64_t number = 0;
        const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), number);
        if (error == std::errc() && end == text.data() + text.size() && !text.empty()) return number;
    }
    return std::nullopt;
}

std::optional<std::int32_t> Identifier(const nlohmann::json& value) {
    const auto number = Unsigned(value);
    if (!number || *number > 0x7FFFFFFF) return std::nullopt;
    return static_cast<std::int32_t>(*number);
}

std::string Text(const nlohmann::json& object, const char* key) {
    if (!object.is_object()) return {};
    const auto found = object.find(key);
    return found != object.end() && found->is_string() ? found->get<std::string>() : std::string();
}

std::optional<TrophyGrade> Grade(const std::string& letter) {
    if (letter == "P" || letter == "p") return TrophyGrade::Platinum;
    if (letter == "G" || letter == "g") return TrophyGrade::Gold;
    if (letter == "S" || letter == "s") return TrophyGrade::Silver;
    if (letter == "B" || letter == "b") return TrophyGrade::Bronze;
    return std::nullopt;
}

int Points(TrophyGrade grade) {
    switch (grade) {
    case TrophyGrade::Gold: return 90;
    case TrophyGrade::Silver: return 30;
    case TrophyGrade::Bronze: return 15;
    case TrophyGrade::Platinum: break;
    }
    return 0;
}

bool EndsWith(std::string_view text, std::string_view suffix) {
    return text.size() >= suffix.size() && text.substr(text.size() - suffix.size()) == suffix;
}

template <typename Number>
bool ParseNumber(std::string_view text, Number& value) {
    if (text.empty()) return false;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    return error == std::errc() && end == text.data() + text.size();
}

// "<user>_<label>" -> label, when both are numbers.
std::optional<std::uint32_t> LabelOf(std::string_view stem) {
    const auto underscore = stem.rfind('_');
    if (underscore == std::string_view::npos) return std::nullopt;
    std::int64_t user = 0;
    std::uint32_t label = 0;
    if (!ParseNumber(stem.substr(0, underscore), user) || !ParseNumber(stem.substr(underscore + 1), label)) return std::nullopt;
    return label;
}

template <typename Visit>
void ReadPairs(const std::filesystem::path& file, Visit visit) {
    std::error_code error;
    const auto size = std::filesystem::file_size(file, error);
    if (error || size > RecordMaxSize) return;
    std::ifstream stream(file, std::ios::binary);
    std::string line;
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const auto space = line.find(' ');
        if (space == std::string::npos) continue;
        std::uint64_t key = 0;
        std::uint64_t value = 0;
        if (!ParseNumber(std::string_view(line).substr(0, space), key) || !ParseNumber(std::string_view(line).substr(space + 1), value)) continue;
        visit(key, value);
    }
}

}

const char* TrophyGradeName(TrophyGrade grade) {
    switch (grade) {
    case TrophyGrade::Platinum: return "Platinum";
    case TrophyGrade::Gold: return "Gold";
    case TrophyGrade::Silver: return "Silver";
    case TrophyGrade::Bronze: return "Bronze";
    }
    return "";
}

TrophyCount TrophySet::Count() const {
    TrophyCount count;
    for (const auto& trophy : trophies) {
        ++count.total;
        if (trophy.earnedAt) ++count.earned;
    }
    return count;
}

TrophyCount TrophySet::Count(TrophyGrade grade) const {
    TrophyCount count;
    for (const auto& trophy : trophies) {
        if (trophy.grade != grade) continue;
        ++count.total;
        if (trophy.earnedAt) ++count.earned;
    }
    return count;
}

int TrophySet::Percent() const {
    int total = 0;
    int earned = 0;
    for (const auto& trophy : trophies) {
        total += Points(trophy.grade);
        if (trophy.earnedAt) earned += Points(trophy.grade);
    }
    return total == 0 ? 0 : earned * 100 / total;
}

std::map<std::string, std::vector<std::uint8_t>> ReadUcp(std::span<const std::uint8_t> data) {
    std::map<std::string, std::vector<std::uint8_t>> files;
    const auto magic = BigEndian(data, 0, 4);
    const auto version = BigEndian(data, 4, 4);
    const auto size = BigEndian(data, 8, 8);
    const auto count = BigEndian(data, 0x10, 4);
    const auto toc = BigEndian(data, 0x14, 4);
    if (!magic || *magic != UcpMagic || !version || *version != 1 || !size || *size < UcpHeaderSize || *size > data.size() || !count || *count > UcpMaxEntries ||
        !toc || *toc > *size || UcpTocHeaderSize + *count * UcpEntrySize > *size - *toc) {
        return files;
    }
    for (std::uint64_t index = 0; index < *count; ++index) {
        const auto entry = *toc + UcpTocHeaderSize + index * UcpEntrySize;
        const auto* field = reinterpret_cast<const char*>(data.data() + entry);
        const std::string name(field, std::find(field, field + UcpNameSize, '\0'));
        const auto offset = BigEndian(data, entry + UcpNameSize, 8);
        const auto length = BigEndian(data, entry + UcpNameSize + 8, 8);
        if (!offset || !length || *offset > *size || *length > *size - *offset) return {};
        files.try_emplace(name, data.begin() + static_cast<std::ptrdiff_t>(*offset), data.begin() + static_cast<std::ptrdiff_t>(*offset + *length));
    }
    return files;
}

std::optional<TrophySet> ParseTrophyPackage(const std::map<std::string, std::vector<std::uint8_t>>& files, std::uint32_t label,
                                            const std::vector<std::string>& languages) {
    const auto conf = Json(files, "tropconf.json");
    if (!conf.is_object()) return std::nullopt;
    const auto definitions = conf.find("trophies");
    if (definitions == conf.end() || !definitions->is_array()) return std::nullopt;

    std::vector<std::string> candidates = languages;
    candidates.insert(candidates.end(), {"en-US", "en-GB"});
    if (auto fallback = Text(conf, "defaultLanguage"); !fallback.empty()) candidates.push_back(fallback);
    std::string chosen;
    for (const auto& language : candidates) {
        if (files.count("tropmeta_" + language + ".json") != 0) {
            chosen = language;
            break;
        }
    }
    if (chosen.empty()) {
        for (const auto& [name, data] : files) {
            if (name.rfind("tropmeta_", 0) == 0 && EndsWith(name, ".json")) {
                chosen = name.substr(9, name.size() - 14);
                break;
            }
        }
    }
    const auto meta = Json(files, "tropmeta_" + chosen + ".json");
    const nlohmann::json metadata = meta.is_object() && meta.contains("metadata") ? meta["metadata"] : nlohmann::json();

    std::map<std::int32_t, std::pair<std::string, std::string>> texts;
    if (metadata.is_object()) {
        if (const auto list = metadata.find("trophyMetadata"); list != metadata.end() && list->is_array()) {
            for (const auto& entry : *list) {
                if (!entry.is_object() || !entry.contains("id")) continue;
                if (const auto id = Identifier(entry["id"])) texts[*id] = {Text(entry, "name"), Text(entry, "detail")};
            }
        }
    }

    TrophySet set;
    if (metadata.is_object() && metadata.contains("titleMetadata")) set.title = Text(metadata["titleMetadata"], "name");
    for (const auto& definition : *definitions) {
        if (!definition.is_object() || !definition.contains("id")) continue;
        const auto id = Identifier(definition["id"]);
        const auto grade = Grade(Text(definition, "grade"));
        if (!id || !grade) continue;
        Trophy trophy;
        trophy.label = label;
        trophy.id = *id;
        trophy.grade = *grade;
        if (const auto hidden = definition.find("hidden"); hidden != definition.end() && hidden->is_boolean()) trophy.hidden = hidden->get<bool>();
        if (const auto text = texts.find(*id); text != texts.end()) {
            trophy.name = text->second.first;
            trophy.detail = text->second.second;
        }
        if (const auto condition = definition.find("unlockCondition"); condition != definition.end() && condition->is_object()) {
            const auto progressive = condition->find("progressive");
            if (progressive != condition->end() && progressive->is_boolean() && progressive->get<bool>()) {
                const auto stat = condition->contains("udsStatId") ? Unsigned((*condition)["udsStatId"]) : std::nullopt;
                const auto target = condition->contains("targetValue") ? Unsigned((*condition)["targetValue"]) : std::nullopt;
                if (stat && target && *target > 1) {
                    trophy.statId = stat;
                    trophy.target = *target;
                }
            }
        }
        std::array<char, 16> icon{};
        std::snprintf(icon.data(), icon.size(), "trop%04d.png", *id);
        if (const auto found = files.find(icon.data()); found != files.end()) trophy.icon = found->second;
        set.trophies.push_back(std::move(trophy));
    }
    for (const auto& name : {"icon0_" + chosen + ".png", std::string("icon0.png")}) {
        if (const auto found = files.find(name); found != files.end()) {
            set.icon = found->second;
            break;
        }
    }
    if (set.icon.empty()) {
        for (const auto& [name, data] : files) {
            if (name.rfind("icon0", 0) == 0 && EndsWith(name, ".png")) {
                set.icon = data;
                break;
            }
        }
    }
    return set;
}

TrophyRecords ReadTrophyRecords(const std::filesystem::path& folder) {
    TrophyRecords records;
    std::error_code error;
    for (std::filesystem::directory_iterator it(folder, error), end; !error && it != end; it.increment(error)) {
        if (!it->is_regular_file(error)) continue;
        const auto name = ToUtf8(it->path().filename());
        if (EndsWith(name, StatsSuffix)) {
            const auto label = LabelOf(std::string_view(name).substr(0, name.size() - StatsSuffix.size()));
            if (!label) continue;
            ReadPairs(it->path(), [&](std::uint64_t stat, std::uint64_t value) {
                auto& kept = records.stats[{*label, stat}];
                kept = std::max(kept, value);
            });
        } else if (EndsWith(name, RecordSuffix)) {
            const auto label = LabelOf(std::string_view(name).substr(0, name.size() - RecordSuffix.size()));
            if (!label) continue;
            ReadPairs(it->path(), [&](std::uint64_t id, std::uint64_t tick) {
                if (id > 0x7FFFFFFF || tick < UnixEpochTick) return;
                const std::pair<std::uint32_t, std::int32_t> key{*label, static_cast<std::int32_t>(id)};
                const auto found = records.unlocks.find(key);
                if (found == records.unlocks.end() || tick < found->second) records.unlocks[key] = tick;
            });
        }
    }
    return records;
}

void ApplyTrophyRecords(TrophySet& set, const TrophyRecords& records) {
    for (auto& trophy : set.trophies) {
        trophy.earnedAt.reset();
        if (const auto found = records.unlocks.find({trophy.label, trophy.id}); found != records.unlocks.end()) {
            trophy.earnedAt = static_cast<std::int64_t>((found->second - UnixEpochTick) / 1000000);
        }
        trophy.progress = 0;
        if (trophy.statId) {
            if (const auto found = records.stats.find({trophy.label, *trophy.statId}); found != records.stats.end()) {
                trophy.progress = std::min(found->second, trophy.target);
            }
            if (trophy.earnedAt) trophy.progress = trophy.target;
        }
    }
}

std::optional<TrophyKey> ParseTrophyKey(std::string_view text) {
    TrophyKey key{};
    std::size_t nibbles = 0;
    for (const char c : text) {
        if (c == ' ' || c == '-' || c == ':' || c == '\t') continue;
        int value = -1;
        if (c >= '0' && c <= '9') value = c - '0';
        else if (c >= 'a' && c <= 'f') value = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') value = c - 'A' + 10;
        if (value < 0 || nibbles >= 32) return std::nullopt;
        key[nibbles / 2] = static_cast<std::uint8_t>(key[nibbles / 2] | (nibbles % 2 == 0 ? value << 4 : value));
        ++nibbles;
    }
    if (nibbles != 32) return std::nullopt;
    return key;
}

std::map<std::string, TrpEntry> ReadTrp(std::span<const std::uint8_t> data) {
    constexpr std::uint64_t TrpMagic = 0xDCA24D00;
    constexpr std::uint64_t HeaderSize = 0x60;
    std::map<std::string, TrpEntry> files;
    const auto magic = BigEndian(data, 0, 4);
    const auto count = BigEndian(data, 0x10, 4);
    const auto entrySize = BigEndian(data, 0x14, 4);
    if (!magic || *magic != TrpMagic || !count || *count > 4096 || !entrySize || *entrySize < 0x40 || *entrySize > 0x1000) return files;
    for (std::uint64_t index = 0; index < *count; ++index) {
        const auto entry = HeaderSize + index * *entrySize;
        if (entry + 0x40 > data.size()) return {};
        const auto* field = reinterpret_cast<const char*>(data.data() + entry);
        const std::string name(field, std::find(field, field + 32, '\0'));
        const auto offset = BigEndian(data, entry + 32, 8);
        const auto length = BigEndian(data, entry + 40, 8);
        const auto flag = BigEndian(data, entry + 48, 4);
        if (!offset || !length || !flag || *offset > data.size() || *length > data.size() - *offset) return {};
        files[name] = {static_cast<std::uint32_t>(*flag),
                       std::vector<std::uint8_t>(data.begin() + static_cast<std::ptrdiff_t>(*offset), data.begin() + static_cast<std::ptrdiff_t>(*offset + *length))};
    }
    return files;
}

std::vector<std::string> NpCommIds(std::span<const std::uint8_t> npbind) {
    std::vector<std::string> ids;
    const std::string_view text(reinterpret_cast<const char*>(npbind.data()), npbind.size());
    for (auto at = text.find("NPWR"); at != std::string_view::npos; at = text.find("NPWR", at + 4)) {
        if (at + 12 > text.size()) break;
        const auto id = text.substr(at, 12);  // NPWR12345_00
        bool ok = id[9] == '_';
        for (const std::size_t i : {4u, 5u, 6u, 7u, 8u, 10u, 11u}) ok = ok && id[i] >= '0' && id[i] <= '9';
        if (ok && std::find(ids.begin(), ids.end(), id) == ids.end()) ids.emplace_back(id);
    }
    return ids;
}

std::string DecryptEsfm(std::span<const std::uint8_t> esfm, const TrophyKey& key, std::string_view npCommId) {
    if (esfm.size() <= 16) return {};
    // The file key: the NP communication ID (16 bytes, zero padded) encrypted with the
    // trophy key (one CBC block with a zero IV is one plain block).
    std::array<std::uint8_t, 16> id{};
    std::copy_n(npCommId.begin(), std::min<std::size_t>(npCommId.size(), 16), id.begin());
    const Aes128 fileKey(Aes128(key).EncryptBlock(id));
    const auto plain = fileKey.DecryptCbc(esfm.subspan(16), esfm.first<16>());
    std::string xml(plain.begin(), plain.end());
    if (const auto end = xml.rfind('>'); end != std::string::npos) xml.resize(end + 1);  // padding after the last tag
    else xml.clear();
    return xml;
}

namespace {

// The text between <tag> and </tag> after from; empty when there's none.
std::string Element(std::string_view xml, std::string_view tag, std::size_t from = 0, std::size_t until = std::string_view::npos) {
    const std::string open = "<" + std::string(tag) + ">";
    const std::string close = "</" + std::string(tag) + ">";
    const auto start = xml.find(open, from);
    if (start == std::string_view::npos || start >= until) return {};
    const auto end = xml.find(close, start);
    if (end == std::string_view::npos || end > until) return {};
    return DecodeXmlEntities(xml.substr(start + open.size(), end - start - open.size()));
}

std::string Attribute(std::string_view tag, std::string_view name) {
    const std::string key = std::string(name) + "=\"";
    for (auto at = tag.find(key); at != std::string_view::npos; at = tag.find(key, at + 1)) {
        if (at > 0 && tag[at - 1] != ' ' && tag[at - 1] != '\t' && tag[at - 1] != '\n' && tag[at - 1] != '\r') continue;
        const auto start = at + key.size();
        const auto end = tag.find('"', start);
        if (end == std::string_view::npos) return {};
        return std::string(tag.substr(start, end - start));
    }
    return {};
}

}

std::optional<TrophySet> ParsePs4TrophyXml(std::string_view xml, std::uint32_t label) {
    if (xml.find("<trophyconf") == std::string_view::npos) return std::nullopt;
    TrophySet set;
    set.title = Element(xml, "title-name");
    for (auto at = xml.find("<trophy "); at != std::string_view::npos; at = xml.find("<trophy ", at + 1)) {
        const auto tagEnd = xml.find('>', at);
        const auto close = xml.find("</trophy>", at);
        if (tagEnd == std::string_view::npos || close == std::string_view::npos) break;
        const auto tag = xml.substr(at, tagEnd - at);
        Trophy trophy;
        trophy.label = label;
        int id = -1;
        const auto idText = Attribute(tag, "id");
        if (!ParseNumber(idText, id) || id < 0) continue;
        trophy.id = id;
        const auto grade = Grade(Attribute(tag, "ttype"));
        if (!grade) continue;
        trophy.grade = *grade;
        trophy.hidden = Attribute(tag, "hidden") == "yes";
        trophy.name = Element(xml, "name", tagEnd, close);
        trophy.detail = Element(xml, "detail", tagEnd, close);
        set.trophies.push_back(std::move(trophy));
    }
    if (set.trophies.empty()) return std::nullopt;
    return set;
}

namespace {

// The file name part of TROP_NN for the first language: "TROP_01" for en-US.
std::string Ps4TextFile(const std::vector<std::string>& languages) {
    static constexpr std::array<std::string_view, 31> Tags{"ja-JP", "en-US", "fr-FR", "es-ES", "de-DE", "it-IT",  "nl-NL", "pt-PT",
                                                           "ru-RU", "ko-KR", "zh-Hant", "zh-Hans", "fi-FI", "sv-SE", "da-DK", "no-NO",
                                                           "pl-PL", "pt-BR", "en-GB", "tr-TR", "es-419", "ar-AE", "fr-CA", "cs-CZ",
                                                           "hu-HU", "el-GR", "ro-RO", "th-TH", "vi-VN", "id-ID", "uk-UA"};
    for (const auto& language : languages) {
        for (std::size_t index = 0; index < Tags.size(); ++index) {
            if (Tags[index] == language) {
                const auto number = std::to_string(index);
                return "TROP_" + std::string(number.size() < 2 ? "0" : "") + number;
            }
        }
    }
    return {};
}

std::optional<TrophySet> LoadPs4Trophies(PackageReader& package, const std::vector<std::string>& languages, const std::optional<TrophyKey>& key) {
    std::vector<std::string> commIds;
    try {
        if (auto npbind = package.ReadAll("sce_sys/npbind.dat", 1u << 20)) commIds = NpCommIds(*npbind);
    } catch (const std::exception&) {
    }
    const auto localized = Ps4TextFile(languages);
    std::optional<TrophySet> result;
    for (std::uint32_t label = 0; label < 100; ++label) {
        std::array<char, 40> name{};
        std::snprintf(name.data(), name.size(), "sce_sys/trophy/trophy%02u.trp", label);
        std::optional<std::vector<std::uint8_t>> data;
        try {
            data = package.ReadAll(name.data(), UcpMaxSize);
        } catch (const std::exception&) {
            continue;
        }
        if (!data) {
            if (label == 0) continue;
            break;
        }
        const auto files = ReadTrp(*data);
        if (files.empty()) continue;
        const auto icon = [&](const std::string& file) {
            const auto found = files.find(file);
            return found == files.end() ? std::vector<std::uint8_t>{} : found->second.data;
        };
        // Plain XML in some packages (.SFM), encrypted in a retail one (.ESFM).
        std::optional<TrophySet> loaded;
        const std::string commId = label < commIds.size() ? commIds[label] : std::string();
        for (const auto& stem : {localized, std::string("TROP"), std::string("TROPCONF")}) {
            if (stem.empty() || loaded) continue;
            if (const auto plain = files.find(stem + ".SFM"); plain != files.end()) {
                loaded = ParsePs4TrophyXml(std::string_view(reinterpret_cast<const char*>(plain->second.data.data()), plain->second.data.size()), label);
            } else if (const auto locked = files.find(stem + ".ESFM"); locked != files.end() && key && !commId.empty()) {
                loaded = ParsePs4TrophyXml(DecryptEsfm(locked->second.data, *key, commId), label);
            }
        }
        if (!loaded) {
            // No key (or a wrong one): what the icons tell.
            TrophySet placeholder;
            placeholder.encrypted = true;
            for (const auto& [file, entry] : files) {
                if (file.size() == 11 && file.starts_with("TROP") && file.ends_with(".PNG")) ++placeholder.lockedCount;
            }
            loaded = std::move(placeholder);
        } else {
            for (auto& trophy : loaded->trophies) {
                char file[16];
                std::snprintf(file, sizeof(file), "TROP%03d.PNG", trophy.id);
                trophy.icon = icon(file);
            }
        }
        loaded->icon = icon("ICON0.PNG");
        if (!result) {
            result = std::move(loaded);
        } else if (result->encrypted || loaded->encrypted) {
            result->encrypted = true;
            result->lockedCount += loaded->lockedCount + static_cast<int>(loaded->trophies.size());
            result->trophies.clear();
        } else {
            for (auto& trophy : loaded->trophies) result->trophies.push_back(std::move(trophy));
        }
    }
    return result;
}

}

std::optional<TrophySet> LoadTrophies(PackageReader& package, const std::vector<std::string>& languages, const std::optional<TrophyKey>& ps4Key) {
    if (!package.IsDirectory("sce_sys/trophy2") && package.IsDirectory("sce_sys/trophy")) return LoadPs4Trophies(package, languages, ps4Key);
    std::optional<TrophySet> result;
    for (std::uint32_t label = 0; label < 100; ++label) {
        std::array<char, 40> name{};
        std::snprintf(name.data(), name.size(), "sce_sys/trophy2/trophy%02u.ucp", label);
        std::optional<std::vector<std::uint8_t>> data;
        try {
            data = package.ReadAll(name.data(), UcpMaxSize);
        } catch (const std::exception&) {
            continue;
        }
        if (!data) {
            if (label == 0) continue;
            break;
        }
        const auto files = ReadUcp(*data);
        auto loaded = ParseTrophyPackage(files, label, languages);
        if (!loaded) continue;
        if (!result) {
            result = std::move(*loaded);
        } else {
            if (result->title.empty()) result->title = loaded->title;
            if (result->icon.empty()) result->icon = std::move(loaded->icon);
            for (auto& trophy : loaded->trophies) result->trophies.push_back(std::move(trophy));
        }
    }
    if (result && result->trophies.empty()) return std::nullopt;
    return result;
}

std::vector<std::string> TrophyLanguages(std::string_view consoleLanguage) {
    // The console's language numbers 0 to 30, as AnyPS5 maps them to trophy text tags.
    static constexpr std::array<std::string_view, 31> Tags{"ja-JP", "en-US", "fr-FR", "es-ES", "de-DE", "it-IT",  "nl-NL", "pt-PT",
                                                           "ru-RU", "ko-KR", "zh-Hant", "zh-Hans", "fi-FI", "sv-SE", "da-DK", "no-NO",
                                                           "pl-PL", "pt-BR", "en-GB", "tr-TR", "es-419", "ar-AE", "fr-CA", "cs-CZ",
                                                           "hu-HU", "el-GR", "ro-RO", "th-TH", "vi-VN", "id-ID", "uk-UA"};
    unsigned number = 0;
    if (ParseNumber(consoleLanguage, number) && number < Tags.size()) return {std::string(Tags[number])};
    return {};
}

}
