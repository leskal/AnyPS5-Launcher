#include "core/I18n.hpp"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <unordered_map>

#include <nlohmann/json.hpp>

#include "core/Utf8.hpp"

namespace launcher {

namespace {

struct Pair {
    const char* english;
    const char* text;
};

// The built-in Greek, one {"English", "Ελληνικά"} pair per text.
constexpr Pair Greek[] = {
#include "core/I18nGreek.inc"
};

struct BuiltIn {
    const char* code;
    const char* name;
    const Pair* pairs;
    std::size_t count;
};

constexpr BuiltIn BuiltIns[] = {
    {"el", "\xCE\x95\xCE\xBB\xCE\xBB\xCE\xB7\xCE\xBD\xCE\xB9\xCE\xBA\xCE\xAC", Greek, std::size(Greek)},  // Ελληνικά
};

struct Language {
    std::string code = "en";
    std::unordered_map<std::string, std::string> texts;
    // "Label##id" texts: the label translated, the id kept (made once, then kept here).
    std::mutex mutex;
    std::unordered_map<std::string, std::unique_ptr<std::string>> withIds;
};

std::unique_ptr<Language>& Current() {
    static std::unique_ptr<Language> language = std::make_unique<Language>();
    return language;
}

std::string LowerCode(std::string code) {
    for (auto& c : code) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        if (c == '_') c = '-';
    }
    return code;
}

const BuiltIn* FindBuiltIn(std::string_view code) {
    for (const auto& builtIn : BuiltIns) {
        if (code == builtIn.code) return &builtIn;
    }
    return nullptr;
}

// A lang\<code>.json: {"_name": "Español", "English text": "Texto", ...}.
bool ReadLangFile(const std::filesystem::path& file, std::string* name, std::unordered_map<std::string, std::string>* texts) {
    std::ifstream stream(file, std::ios::binary);
    if (!stream) return false;
    const auto document = nlohmann::json::parse(stream, nullptr, false);
    if (document.is_discarded() || !document.is_object()) return false;
    for (const auto& [key, value] : document.items()) {
        if (!value.is_string()) continue;
        if (key == "_name") {
            if (name != nullptr) *name = value.get<std::string>();
        } else if (texts != nullptr && !value.get<std::string>().empty()) {
            (*texts)[key] = value.get<std::string>();
        }
    }
    return true;
}

std::vector<std::string> LangFileCodes(const std::filesystem::path& folder) {
    std::vector<std::string> codes;
    std::error_code error;
    if (folder.empty() || !std::filesystem::is_directory(folder, error)) return codes;
    for (const auto& entry : std::filesystem::directory_iterator(folder, error)) {
        if (entry.path().extension() != ".json") continue;
        codes.push_back(LowerCode(ToUtf8(entry.path().stem())));
    }
    std::sort(codes.begin(), codes.end());
    return codes;
}

std::filesystem::path& LangFolder() {
    static std::filesystem::path folder;
    return folder;
}

bool Known(const std::string& code) {
    if (code == "en" || FindBuiltIn(code) != nullptr) return true;
    const auto codes = LangFileCodes(LangFolder());
    return std::find(codes.begin(), codes.end(), code) != codes.end();
}

}

std::vector<LanguageInfo> AvailableLanguages() {
    std::vector<LanguageInfo> languages{{"en", "English"}};
    for (const auto& builtIn : BuiltIns) languages.push_back({builtIn.code, builtIn.name});
    for (const auto& code : LangFileCodes(LangFolder())) {
        if (code == "en" || FindBuiltIn(code) != nullptr) continue;
        std::string name = code;
        ReadLangFile(LangFolder() / FromUtf8(code + ".json"), &name, nullptr);
        languages.push_back({code, name});
    }
    return languages;
}

std::string MatchLanguage(const std::string& setting, const std::vector<std::string>& preferred, const std::filesystem::path& langFolder) {
    LangFolder() = langFolder;
    if (!setting.empty()) return Known(LowerCode(setting)) ? LowerCode(setting) : std::string("en");
    // The first preferred language we have; "el-GR" matches "el".
    for (const auto& wanted : preferred) {
        auto code = LowerCode(wanted);
        if (Known(code)) return code;
        code = code.substr(0, code.find('-'));
        if (Known(code)) return code;
    }
    return "en";
}

std::string SelectLanguage(const std::string& setting, const std::vector<std::string>& preferred, const std::filesystem::path& langFolder) {
    const auto chosen = MatchLanguage(setting, preferred, langFolder);
    auto language = std::make_unique<Language>();
    language->code = chosen;
    if (const auto* builtIn = FindBuiltIn(chosen)) {
        for (std::size_t i = 0; i < builtIn->count; ++i) language->texts[builtIn->pairs[i].english] = builtIn->pairs[i].text;
    }
    if (chosen != "en") ReadLangFile(langFolder / FromUtf8(chosen + ".json"), nullptr, &language->texts);
    // The old one is kept: a text from it may still be in use (a frame being drawn,
    // a background task); switching languages is rare, so this costs little.
    static std::vector<std::unique_ptr<Language>> retired;
    retired.push_back(std::move(Current()));
    Current() = std::move(language);
    return chosen;
}

const std::string& CurrentLanguage() {
    return Current()->code;
}

const char* T(const char* english) {
    auto& language = *Current();
    const auto& texts = language.texts;
    if (texts.empty() || english == nullptr) return english;
    if (const auto found = texts.find(english); found != texts.end()) return found->second.c_str();
    // ImGui's "Label##id": translate the label, keep the id as it is.
    const std::string_view text(english);
    const auto id = text.find("##");
    if (id == std::string_view::npos || id == 0) return english;
    const auto label = texts.find(std::string(text.substr(0, id)));
    if (label == texts.end()) return english;
    std::lock_guard lock(language.mutex);
    auto& made = language.withIds[std::string(text)];
    if (!made) made = std::make_unique<std::string>(label->second + std::string(text.substr(id)));
    return made->c_str();
}

std::string T(const std::string& english) {
    return T(english.c_str());
}

std::string Fmt(std::string_view pattern, std::initializer_list<std::string_view> values) {
    std::string result;
    result.reserve(pattern.size() + 16);
    for (std::size_t at = 0; at < pattern.size(); ++at) {
        if (pattern[at] == '{' && at + 2 < pattern.size() && pattern[at + 1] >= '0' && pattern[at + 1] <= '9' && pattern[at + 2] == '}') {
            const auto index = static_cast<std::size_t>(pattern[at + 1] - '0');
            if (index < values.size()) result += *(values.begin() + index);
            at += 2;
            continue;
        }
        result += pattern[at];
    }
    return result;
}

std::vector<std::pair<std::string, std::string>> BuiltInTranslations(std::string_view code) {
    std::vector<std::pair<std::string, std::string>> pairs;
    if (const auto* builtIn = FindBuiltIn(code)) {
        for (std::size_t i = 0; i < builtIn->count; ++i) pairs.emplace_back(builtIn->pairs[i].english, builtIn->pairs[i].text);
    }
    return pairs;
}

}
