#include "core/Settings.hpp"

#include <fstream>
#include <iterator>
#include <system_error>

#include <nlohmann/json.hpp>

#include "core/FileOps.hpp"
#include "core/Utf8.hpp"

namespace launcher {

namespace {

constexpr int SettingsVersion = 1;

std::optional<nlohmann::json> ReadJson(const std::filesystem::path& file, std::string& error) {
    std::ifstream stream(file, std::ios::binary);
    if (!stream) {
        error = "cannot open " + ToUtf8(file);
        return std::nullopt;
    }
    const std::string text((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    auto document = nlohmann::json::parse(text, nullptr, false);
    if (document.is_discarded() || !document.is_object()) {
        error = ToUtf8(file) + " is not a valid settings file";
        return std::nullopt;
    }
    return document;
}

void ReadString(const nlohmann::json& document, const char* key, std::string& target, std::string& error) {
    const auto found = document.find(key);
    if (found == document.end()) return;
    if (!found->is_string()) {
        if (error.empty()) error = std::string("setting \"") + key + "\" is not a string and was ignored";
        return;
    }
    target = found->get<std::string>();
}

std::optional<std::filesystem::path> FirstFile(const std::filesystem::path& root, std::initializer_list<const char*> candidates) {
    std::error_code error;
    for (const char* candidate : candidates) {
        const auto path = root / FromUtf8(candidate);
        if (std::filesystem::is_regular_file(path, error)) return path;
    }
    return std::nullopt;
}

}

LoadedSettings LoadSettings(const std::filesystem::path& file) {
    LoadedSettings result;
    std::error_code error;
    if (!std::filesystem::exists(file, error)) return result;
    result.existed = true;
    const auto document = ReadJson(file, result.error);
    if (!document) return result;
    ReadString(*document, "anyps5Folder", result.settings.anyps5Folder, result.error);
    ReadString(*document, "anyps5Version", result.settings.anyps5Version, result.error);
    ReadString(*document, "gamesFolder", result.settings.gamesFolder, result.error);
    ReadString(*document, "libraryFolder", result.settings.libraryFolder, result.error);
    ReadString(*document, "userdataFolder", result.settings.userdataFolder, result.error);
    ReadString(*document, "profile", result.settings.profile, result.error);
    return result;
}

void SaveSettings(const std::filesystem::path& file, const Settings& settings) {
    nlohmann::json document = nlohmann::json::object();
    std::error_code error;
    if (std::filesystem::exists(file, error)) {
        std::string ignored;
        if (auto existing = ReadJson(file, ignored)) document = std::move(*existing);
    }
    document["version"] = SettingsVersion;
    document["anyps5Folder"] = settings.anyps5Folder;
    document["anyps5Version"] = settings.anyps5Version;
    document["gamesFolder"] = settings.gamesFolder;
    document["libraryFolder"] = settings.libraryFolder;
    document["userdataFolder"] = settings.userdataFolder;
    document["profile"] = settings.profile;
    document.erase("python");
    WriteFileAtomically(file, document.dump(4) + "\n");
}

std::filesystem::path EffectiveLibrary(const Settings& settings, const std::filesystem::path& launcherFolder) {
    return settings.libraryFolder.empty() ? launcherFolder / "library" : FromUtf8(settings.libraryFolder);
}

std::filesystem::path EffectiveUserdata(const Settings& settings, const std::filesystem::path& launcherFolder) {
    return settings.userdataFolder.empty() ? launcherFolder / "userdata" : FromUtf8(settings.userdataFolder);
}

AnyPS5Tools FindAnyPS5Tools(const std::string& anyps5Folder) {
    AnyPS5Tools tools;
    if (anyps5Folder.empty()) return tools;
    const auto root = FromUtf8(anyps5Folder);
#ifdef _WIN32
    tools.relinker = FirstFile(root, {"build/core/relinker/relinker.exe", "build-relinker/core/relinker/relinker.exe", "relinker.exe"});
#else
    tools.relinker = FirstFile(root, {"build/core/relinker/relinker", "build-relinker/core/relinker/relinker", "relinker"});
#endif
    return tools;
}

}
