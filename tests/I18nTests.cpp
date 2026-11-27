#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <regex>
#include <set>
#include <string>

#include "Fixtures.hpp"
#include "Framework.hpp"

#include "core/I18n.hpp"
#include "core/Settings.hpp"

using launcher::Fmt;
using launcher::T;

TEST(FmtPutsValuesWhereThePatternSays) {
    CHECK_EQ(Fmt("Played {0}, last {1}", {"2 h", "today"}), std::string("Played 2 h, last today"));
    CHECK_EQ(Fmt("{1} before {0}", {"a", "b"}), std::string("b before a"));
    CHECK_EQ(Fmt("{0} and {5}", {"x"}), std::string("x and "));
    CHECK_EQ(Fmt("50% {x}", {"y"}), std::string("50% {x}"));
}

TEST(GreekIsBuiltInAndFollowsTheSystem) {
    fixture::TempDir temp;
    CHECK_EQ(launcher::SelectLanguage("", {"el-GR", "en-US"}, temp.Path()), std::string("el"));
    CHECK_EQ(launcher::CurrentLanguage(), std::string("el"));
    CHECK(std::strcmp(T("Library"), "Library") != 0);
    const char* unknown = "Not a text of the launcher";
    CHECK(T(unknown) == unknown);  // the same text back, not a copy
    // "Label##id": the label is translated, the id kept.
    const std::string withId = T("Browse...##library");
    CHECK(withId.size() > 9 && withId.substr(withId.size() - 9) == "##library");
    CHECK(withId.rfind("Browse...", 0) != 0);

    CHECK_EQ(launcher::SelectLanguage("", {"fr-FR", "de-DE"}, temp.Path()), std::string("en"));
    CHECK(std::strcmp(T("Library"), "Library") == 0);
    CHECK_EQ(launcher::SelectLanguage("el", {"fr-FR"}, temp.Path()), std::string("el"));  // the setting wins
    CHECK_EQ(launcher::SelectLanguage("xx", {"el"}, temp.Path()), std::string("en"));     // an unknown setting: English
    CHECK_EQ(launcher::MatchLanguage("", {"el"}, temp.Path()), std::string("el"));
    CHECK_EQ(launcher::CurrentLanguage(), std::string("en"));  // matching doesn't switch
    launcher::SelectLanguage("en", {}, temp.Path());
}

TEST(LangFilesAddLanguagesAndChangeTexts) {
    fixture::TempDir temp;
    const auto lang = temp.Path() / "lang";
    std::filesystem::create_directories(lang);
    std::ofstream(lang / "es.json") << R"({"_name": "Español", "Library": "Biblioteca", "Save": ""})";
    std::ofstream(lang / "el.json") << R"({"Library": "Δική μου"})";
    bool spanish = false;
    for (const auto& language : launcher::AvailableLanguages()) spanish = spanish || language.code == "es";
    CHECK(!spanish);  // not before the folder is known
    CHECK_EQ(launcher::SelectLanguage("es", {}, lang), std::string("es"));
    CHECK_EQ(std::string(T("Library")), std::string("Biblioteca"));
    CHECK_EQ(std::string(T("Save")), std::string("Save"));  // empty means not translated
    for (const auto& language : launcher::AvailableLanguages()) {
        if (language.code == "es") spanish = language.name == "Espa\xC3\xB1ol";
    }
    CHECK(spanish);
    launcher::SelectLanguage("el", {}, lang);
    CHECK_EQ(std::string(T("Library")), std::string("Δική μου"));  // a file changes a built-in text
    CHECK(std::strcmp(T("Save"), "Save") != 0);                     // and keeps the rest
    launcher::SelectLanguage("en", {}, lang);
}

// Every translation keeps the printf specifiers and {N} places of its English text,
// in the same order, so no text can read the wrong value or crash.
TEST(GreekKeepsEveryPlaceholder) {
    const std::regex printf(R"(%[-+ #0-9.]*(?:ll|l|z|h)?[sdiufxXcp%])");
    const std::regex places(R"(\{\d\})");
    const auto list = [](const std::string& text, const std::regex& pattern) {
        std::string found;
        for (auto it = std::sregex_iterator(text.begin(), text.end(), pattern); it != std::sregex_iterator(); ++it) found += it->str() + "|";
        return found;
    };
    const auto pairs = launcher::BuiltInTranslations("el");
    CHECK(pairs.size() > 700);
    std::set<std::string> seen;
    for (const auto& [english, greek] : pairs) {
        CHECK(seen.insert(english).second);  // no text twice
        CHECK(!greek.empty());
        if (list(english, printf) != list(greek, printf) || list(english, places) != list(greek, places)) {
            std::fprintf(stderr, "placeholders differ: %s\n", english.c_str());
            CHECK(false);
        }
    }
}

TEST(TheLanguageSettingIsSavedOnlyWhenChosen) {
    fixture::TempDir temp;
    const auto file = temp.Path() / "settings.json";
    launcher::Settings settings;
    launcher::SaveSettings(file, settings);
    {
        // Closed before the next save: Windows can't replace a file that is still open.
        std::ifstream stream(file);
        const std::string text((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
        CHECK(text.find("\"language\"") == std::string::npos);
    }
    settings.language = "el";
    launcher::SaveSettings(file, settings);
    CHECK_EQ(launcher::LoadSettings(file).settings.language, std::string("el"));
}
