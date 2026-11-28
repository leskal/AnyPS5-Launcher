#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

// Finds the launcher's translatable texts: the string literals in T("...") and N("...")
// calls. Used by the update-texts build target, which writes lang/launcher.pot (the
// file translations are made from), and by a test that checks that file is current.

namespace texts {

struct Text {
    std::string english;
    std::vector<std::string> files;  // where it is used, relative to the source tree
};

// The texts in one C++ source, in order. Comments and other literals are skipped;
// adjacent literals ("a" "b") are joined; a call whose argument isn't only literals
// (T(name), T("a" + b)) is not a text.
std::vector<std::string> FindTexts(std::string_view source);

// The texts of every .cpp and .hpp under root/src, in order of first use.
std::vector<Text> CollectTexts(const std::filesystem::path& root);

// The gettext template for them, with the "language name" entry first.
std::string MakePot(const std::vector<Text>& texts);

}
