#include "core/Trophies.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <system_error>

#include <nlohmann/json.hpp>

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

std::optional<TrophySet> LoadTrophies(PackageReader& package, const std::vector<std::string>& languages) {
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
