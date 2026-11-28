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

#include "TextScan.hpp"

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
    {
        std::ofstream(lang / "es.po") << "msgctxt \"language name\"\nmsgid \"English\"\nmsgstr \"Español\"\n\n"
                                         "msgid \"Library\"\nmsgstr \"Biblioteca\"\n\nmsgid \"Save\"\nmsgstr \"\"\n";
        std::ofstream(lang / "el.po") << "msgid \"Library\"\nmsgstr \"Δική μου\"\n";
    }
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

TEST(PoFilesAreReadLikeGettextDoes) {
    const auto po = launcher::ParsePo(
        "\xEF\xBB\xBF# a comment\r\n"
        "msgid \"\"\r\nmsgstr \"\"\r\n\"Language: xx\\n\"\r\n\r\n"
        "#. for translators\r\n#: src/app/Main.cpp\r\nmsgid \"\"\r\n\"Two\\n\"\r\n\"lines \\\"quoted\\\"\"\r\nmsgstr \"\"\r\n\"Dos\\n\"\r\n\"líneas\"\r\n\r\n"
        "#, fuzzy\r\nmsgid \"Guessed\"\r\nmsgstr \"Adivinado\"\r\n\r\n"
        "#, c-format\r\nmsgid \"%d games\"\r\nmsgstr \"%d juegos\"\r\n"
        "msgctxt \"something else\"\r\nmsgid \"Save\"\r\nmsgstr \"Otro\"\r\n"
        "msgid \"One file\"\r\nmsgid_plural \"Files\"\r\nmsgstr[0] \"Un archivo\"\r\nmsgstr[1] \"Archivos\"\r\n"
        "msgctxt \"language name\"\r\nmsgid \"English\"\r\nmsgstr \"Prueba\"\r\n"
        "#~ msgid \"Old\"\r\n#~ msgstr \"Viejo\"\r\n");
    CHECK_EQ(po.name, std::string("Prueba"));
    CHECK_EQ(po.texts.size(), std::size_t{3});
    CHECK_EQ(po.texts[0].first, std::string("Two\nlines \"quoted\""));
    CHECK_EQ(po.texts[0].second, std::string("Dos\nlíneas"));
    CHECK_EQ(po.texts[1].second, std::string("%d juegos"));  // the fuzzy one is left out
    CHECK_EQ(po.texts[2].second, std::string("Un archivo"));
}

TEST(TextsAreFoundInTheCode) {
    const auto found = texts::FindTexts(
        "// T(\"in a comment\")\n"
        "ImGui::Text(\"%s\", T(\"Hello \\\"you\\\"\\n\"));\n"
        "const char* x = \"T(not a call)\"; /* N(\"also a comment\") */\n"
        "Show(N(\"Joined \"\n      \"text\"), T(name), T(\"a\" + b), FMT(\"no\"));\n"
        "auto raw = R\"x(T(\"raw\"))x\"; char c = '\"'; T(\"after\");\n");
    CHECK_EQ(found.size(), std::size_t{3});
    CHECK_EQ(found[0], std::string("Hello \"you\"\n"));
    CHECK_EQ(found[1], std::string("Joined text"));
    CHECK_EQ(found[2], std::string("after"));
}

// lang/launcher.pot is what translators work from, so it must list the texts of the code.
TEST(TheTextListIsCurrent) {
    const std::filesystem::path root = LAUNCHER_SOURCE_DIR;
    std::ifstream stream(root / "lang" / "launcher.pot", std::ios::binary);
    std::string saved((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    std::erase(saved, '\r');  // a Windows checkout may have CRLF
    const auto made = texts::MakePot(texts::CollectTexts(root));
    if (saved != made) std::fprintf(stderr, "lang/launcher.pot is out of date: build the update-texts target\n");
    CHECK(saved == made);
}

// Every built-in translation keeps the printf specifiers and {N} places of its English
// text, in the same order, so no text can read the wrong value or crash.
TEST(TranslationsKeepEveryPlaceholder) {
    const std::regex printf(R"(%[-+ #0-9.]*(?:ll|l|z|h)?[sdiufxXcp%])");
    const std::regex places(R"(\{\d\})");
    const auto list = [](const std::string& text, const std::regex& pattern) {
        std::string found;
        for (auto it = std::sregex_iterator(text.begin(), text.end(), pattern); it != std::sregex_iterator(); ++it) found += it->str() + "|";
        return found;
    };
    CHECK(launcher::BuiltInTranslations("el").size() > 700);
    for (const auto& language : launcher::AvailableLanguages()) {
        std::set<std::string> seen;
        for (const auto& [english, text] : launcher::BuiltInTranslations(language.code)) {
            CHECK(seen.insert(english).second);  // no text twice
            if (list(english, printf) != list(text, printf) || list(english, places) != list(text, places)) {
                std::fprintf(stderr, "%s: placeholders differ: %s\n", language.code.c_str(), english.c_str());
                CHECK(false);
            }
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
