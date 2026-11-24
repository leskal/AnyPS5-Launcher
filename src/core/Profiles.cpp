#include "core/Profiles.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <system_error>

#include <nlohmann/json.hpp>

#include "core/FileOps.hpp"
#include "core/Utf8.hpp"

namespace launcher {

namespace {

constexpr std::array<ConsoleLanguage, 31> Languages{{
    {0, "Japanese"}, {1, "English (United States)"}, {2, "French"}, {3, "Spanish"}, {4, "German"}, {5, "Italian"},
    {6, "Dutch"}, {7, "Portuguese (Portugal)"}, {8, "Russian"}, {9, "Korean"}, {10, "Chinese (Traditional)"},
    {11, "Chinese (Simplified)"}, {12, "Finnish"}, {13, "Swedish"}, {14, "Danish"}, {15, "Norwegian"}, {16, "Polish"},
    {17, "Portuguese (Brazil)"}, {18, "English (United Kingdom)"}, {19, "Turkish"}, {20, "Spanish (Latin America)"},
    {21, "Arabic"}, {22, "French (Canada)"}, {23, "Czech"}, {24, "Hungarian"}, {25, "Greek"}, {26, "Romanian"},
    {27, "Thai"}, {28, "Vietnamese"}, {29, "Indonesian"}, {30, "Ukrainian"},
}};

bool IsValidLanguage(const std::string& value) {
    if (value.empty()) return true;
    std::string lower = value;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (lower == "auto") return true;
    if (value.size() > 2 || !std::all_of(value.begin(), value.end(), [](unsigned char c) { return std::isdigit(c) != 0; })) return false;
    const int number = std::stoi(value);
    return number >= 0 && number <= 30;
}

}

std::filesystem::path Layout::Profile() const {
    if (!IsValidProfileName(profile)) throw std::runtime_error("\"" + profile + "\" is not a valid profile name");
    return ProfilesRoot() / FromUtf8(profile);
}

std::span<const ConsoleLanguage> ConsoleLanguages() {
    return Languages;
}

bool IsValidProfileName(std::string_view name) {
    if (name.empty() || name.size() > 64) return false;
    for (const unsigned char c : name) {
        if (c < 0x20 || c == 0x7F) return false;
        if (c == '<' || c == '>' || c == ':' || c == '"' || c == '/' || c == '\\' || c == '|' || c == '?' || c == '*') return false;
    }
    if (name.front() == ' ' || name.back() == ' ' || name.back() == '.' || name == "." || name == "..") return false;
    std::string base(name.substr(0, name.find('.')));
    std::transform(base.begin(), base.end(), base.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    for (const char* reserved : {"con", "prn", "aux", "nul"}) {
        if (base == reserved) return false;
    }
    if (base.size() == 4 && (base.starts_with("com") || base.starts_with("lpt")) && base[3] >= '0' && base[3] <= '9') return false;
    return true;
}

std::vector<std::string> ListProfiles(const std::filesystem::path& userdata) {
    std::vector<std::string> names;
    std::error_code error;
    for (std::filesystem::directory_iterator it(userdata / "profiles", error), end; !error && it != end; it.increment(error)) {
        if (!it->is_directory(error)) continue;
        auto name = ToUtf8(it->path().filename());
        if (IsValidProfileName(name)) names.push_back(std::move(name));
    }
    std::sort(names.begin(), names.end());
    return names;
}

void CreateProfile(const Layout& layout) {
    std::error_code error;
    std::filesystem::create_directories(layout.Profile(), error);
    if (error) throw std::system_error(error, "cannot create the profile folder " + ToUtf8(layout.Profile()));
    if (!std::filesystem::exists(layout.ProfileFile(), error)) SaveProfile(layout, {});
}

ProfileSettings LoadProfile(const Layout& layout) {
    ProfileSettings settings;
    std::ifstream file(layout.ProfileFile(), std::ios::binary);
    if (!file) return settings;
    const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    const auto document = nlohmann::json::parse(text, nullptr, false);
    if (document.is_discarded() || !document.is_object()) throw std::runtime_error(ToUtf8(layout.ProfileFile()) + " is not valid JSON");
    if (const auto language = document.find("language"); language != document.end() && language->is_string()) {
        settings.language = language->get<std::string>();
        if (!IsValidLanguage(settings.language)) throw std::runtime_error(ToUtf8(layout.ProfileFile()) + ": language must be empty, auto or 0 to 30");
    }
    return settings;
}

void SaveProfile(const Layout& layout, const ProfileSettings& settings) {
    if (!IsValidLanguage(settings.language)) throw std::runtime_error("language must be empty, auto or 0 to 30");
    nlohmann::json document = nlohmann::json::object();
    std::error_code error;
    if (std::filesystem::exists(layout.ProfileFile(), error)) {
        std::ifstream file(layout.ProfileFile(), std::ios::binary);
        const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        auto existing = nlohmann::json::parse(text, nullptr, false);
        if (!existing.is_discarded() && existing.is_object()) document = std::move(existing);
    }
    document["version"] = 1;
    document["name"] = layout.profile;
    document["language"] = settings.language;
    std::filesystem::create_directories(layout.Profile(), error);
    WriteFileAtomically(layout.ProfileFile(), document.dump(4) + "\n");
}

void RenameProfile(const Layout& layout, std::string_view newName) {
    if (!IsValidProfileName(newName)) throw std::runtime_error("\"" + std::string(newName) + "\" can't be a profile name (no \\ / : * ? \" < > |)");
    const auto from = layout.Profile();
    Layout renamed = layout;
    renamed.profile = std::string(newName);
    const auto to = renamed.Profile();
    if (from == to) return;
    const auto lower = [](std::string text) {
        std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return text;
    };
    const bool caseOnly = lower(layout.profile) == lower(std::string(newName));
    std::error_code error;
    if (!caseOnly && std::filesystem::exists(to, error)) throw std::runtime_error("a profile named \"" + std::string(newName) + "\" already exists");
    if (!std::filesystem::is_directory(from, error)) throw std::runtime_error("the profile folder " + ToUtf8(from) + " is missing");
    if (caseOnly) {
        // Windows matches names without case: go through a temporary name.
        auto step = from;
        step += ".renaming";
        std::filesystem::rename(from, step);
        std::filesystem::rename(step, to);
    } else {
        std::filesystem::rename(from, to);
    }
}

}
