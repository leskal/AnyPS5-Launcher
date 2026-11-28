#pragma once

#include <filesystem>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

namespace launcher {

// The launcher's texts are written in English in the code and looked up in the chosen
// language by T(). The translations are gettext files: lang/launcher.pot lists the
// texts (made by the update-texts build target), lang/<code>.po translates them (made
// with any gettext tool), and every lang/*.po of the source tree is built in. A
// lang\<code>.po beside the launcher adds a language or changes texts of a built-in one.

struct LanguageInfo {
    std::string code;  // "en", "el"
    std::string name;  // in the language itself: "English", "Ελληνικά"
};

// The languages there are: English, the built-in ones, then those from lang\ files.
std::vector<LanguageInfo> AvailableLanguages();

// Picks the language: a code from the settings, or empty to follow the system's
// preferred languages (best first, like "el-GR"). Unknown ones fall back to English.
// langFolder may hold <code>.po files. Returns the code in use.
std::string SelectLanguage(const std::string& setting, const std::vector<std::string>& preferred, const std::filesystem::path& langFolder);

// The code SelectLanguage would pick, without switching to it.
std::string MatchLanguage(const std::string& setting, const std::vector<std::string>& preferred, const std::filesystem::path& langFolder);

// The code of the language in use.
const std::string& CurrentLanguage();

// Marks a text for translation where T() can't run (a constexpr table): the text is
// translated with T() where it is shown.
constexpr const char* N(const char* english) {
    return english;
}

// The text in the language in use; the English text itself when it has no translation.
// The result lives as long as the language does (until the next SelectLanguage).
const char* T(const char* english);
std::string T(const std::string& english);

// Fills {0}, {1}... in a (translated) pattern with the values, so a translation can
// put them in any order: Fmt(T("Played {0}, last {1}"), time, date).
std::string Fmt(std::string_view pattern, std::initializer_list<std::string_view> values);

// A .po file: its "language name" entry, and the translated texts (fuzzy and empty
// ones left out), as {English, translation} in file order.
struct Translation {
    std::string name;
    std::vector<std::pair<std::string, std::string>> texts;
};
Translation ParsePo(std::string_view po);

// For tests: the built-in pairs of a language (empty for English or unknown codes).
std::vector<std::pair<std::string, std::string>> BuiltInTranslations(std::string_view code);

}
