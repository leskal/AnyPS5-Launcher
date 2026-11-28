#include "core/I18n.hpp"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>

#include "core/Utf8.hpp"

namespace launcher {

namespace {

// The lang/<code>.po files of the source tree, built in by CMake (EmbedTranslations.cmake).
struct EmbeddedPo {
    const char* code;
    const unsigned char* data;
    std::size_t size;
};
#include "LauncherTranslations.inc"

struct BuiltIn {
    std::string code;
    Translation translation;
};

// The built-in languages, read once.
const std::vector<BuiltIn>& BuiltIns() {
    static const std::vector<BuiltIn> builtIns = [] {
        std::vector<BuiltIn> list;
        for (const auto& po : EmbeddedTranslations) {
            if (std::string_view(po.code) == "en") continue;
            list.push_back({po.code, ParsePo(std::string_view(reinterpret_cast<const char*>(po.data), po.size))});
            if (list.back().translation.name.empty()) list.back().translation.name = po.code;
        }
        return list;
    }();
    return builtIns;
}

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
    for (const auto& builtIn : BuiltIns()) {
        if (code == builtIn.code) return &builtIn;
    }
    return nullptr;
}

// A lang\<code>.po beside the launcher.
std::optional<Translation> ReadLangFile(const std::filesystem::path& file) {
    std::ifstream stream(file, std::ios::binary);
    if (!stream) return std::nullopt;
    const std::string text((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    return ParsePo(text);
}

std::vector<std::string> LangFileCodes(const std::filesystem::path& folder) {
    std::vector<std::string> codes;
    std::error_code error;
    if (folder.empty() || !std::filesystem::is_directory(folder, error)) return codes;
    for (const auto& entry : std::filesystem::directory_iterator(folder, error)) {
        if (entry.path().extension() != ".po") continue;
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
    for (const auto& builtIn : BuiltIns()) languages.push_back({builtIn.code, builtIn.translation.name});
    for (const auto& code : LangFileCodes(LangFolder())) {
        if (code == "en" || FindBuiltIn(code) != nullptr) continue;
        const auto file = ReadLangFile(LangFolder() / FromUtf8(code + ".po"));
        languages.push_back({code, file && !file->name.empty() ? file->name : code});
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
        for (const auto& [english, text] : builtIn->translation.texts) language->texts[english] = text;
    }
    // A file beside the launcher adds a language or changes texts of a built-in one.
    if (chosen != "en") {
        if (const auto file = ReadLangFile(langFolder / FromUtf8(chosen + ".po"))) {
            for (const auto& [english, text] : file->texts) language->texts[english] = text;
        }
    }
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
    if (const auto* builtIn = FindBuiltIn(code)) return builtIn->translation.texts;
    return {};
}

namespace {

// The text of a "..." on a PO line, escapes undone; at is just past the opening quote.
std::string PoLiteral(std::string_view line, std::size_t at) {
    std::string text;
    for (; at < line.size() && line[at] != '"'; ++at) {
        if (line[at] != '\\' || at + 1 >= line.size()) {
            text += line[at];
            continue;
        }
        switch (line[++at]) {
        case 'n': text += '\n'; break;
        case 't': text += '\t'; break;
        case 'r': text += '\r'; break;
        default: text += line[at]; break;  // a quote or a backslash
        }
    }
    return text;
}

}

Translation ParsePo(std::string_view po) {
    Translation translation;
    std::string context, english, text;
    std::string* current = nullptr;  // what a "..." line continues
    bool fuzzy = false, translated = false;
    // An entry ends where the next one starts: at its flags, msgctxt or msgid.
    const auto finish = [&] {
        if (translated && !fuzzy && !english.empty() && !text.empty()) {
            if (context == "language name") {
                translation.name = text;
            } else if (context.empty()) {
                translation.texts.emplace_back(english, text);
            }
        }
        context.clear(), english.clear(), text.clear();
        current = nullptr;
        fuzzy = translated = false;
    };
    if (po.substr(0, 3) == "\xEF\xBB\xBF") po.remove_prefix(3);  // a UTF-8 mark
    std::size_t start = 0;
    while (start < po.size()) {
        auto end = po.find('\n', start);
        if (end == std::string_view::npos) end = po.size();
        auto line = po.substr(start, end - start);
        start = end + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) line.remove_suffix(1);
        while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) line.remove_prefix(1);
        if (line.empty()) continue;
        if (line[0] == '#') {
            if (line.substr(0, 2) == "#,") {
                if (translated) finish();
                if (line.find("fuzzy") != std::string_view::npos) fuzzy = true;
            }
            if (line.substr(0, 2) == "#~") current = nullptr;  // an old, removed text
            continue;
        }
        const auto quote = line.find('"');
        if (quote == std::string_view::npos) continue;
        const auto keyword = line.substr(0, quote);
        const auto value = PoLiteral(line, quote + 1);
        if (keyword.empty()) {
            if (current != nullptr) *current += value;
        } else if (keyword.substr(0, 7) == "msgctxt") {
            if (translated) finish();
            context = value;
            current = &context;
        } else if (keyword.substr(0, 12) == "msgid_plural") {
            current = nullptr;
        } else if (keyword.substr(0, 5) == "msgid") {
            if (translated) finish();
            english = value;
            current = &english;
        } else if (keyword.substr(0, 6) == "msgstr") {
            // msgstr, or msgstr[0] of a plural text: the first form is the one used.
            translated = true;
            if (keyword.find('[') != std::string_view::npos && keyword.find("[0]") == std::string_view::npos) {
                current = nullptr;
            } else {
                text = value;
                current = &text;
            }
        }
    }
    finish();
    return translation;
}

}
