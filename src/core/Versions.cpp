#include "core/Versions.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <system_error>

#include <nlohmann/json.hpp>

extern "C" {
#include <sha_256.h>
}

#include "core/AnyPS5.hpp"
#include "core/FileOps.hpp"
#include "core/Links.hpp"
#include "core/Utf8.hpp"

namespace launcher {

namespace {

std::string Now() {
    const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    char text[32];
    std::strftime(text, sizeof(text), "%Y-%m-%d %H:%M:%S", &local);
    return text;
}

bool IsNameCharacter(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' || c == '.';
}

// A release tag as it may appear in a folder and file name.
bool IsSafeTag(std::string_view tag) {
    return !tag.empty() && tag.size() <= 100 && tag != "." && tag != ".." && std::all_of(tag.begin(), tag.end(), IsNameCharacter);
}

std::string Text(const nlohmann::json& object, const char* key) {
    const auto found = object.find(key);
    return found != object.end() && found->is_string() ? found->get<std::string>() : std::string{};
}

std::optional<Release> ReleaseFrom(const nlohmann::json& item) {
    if (!item.is_object()) return std::nullopt;
    if (const auto draft = item.find("draft"); draft != item.end() && draft->is_boolean() && draft->get<bool>()) return std::nullopt;
    Release release;
    release.tag = Text(item, "tag_name");
    if (!IsSafeTag(release.tag)) return std::nullopt;
    release.name = Text(item, "name");
    release.publishedAt = Text(item, "published_at");
    if (const auto pre = item.find("prerelease"); pre != item.end() && pre->is_boolean()) release.prerelease = pre->get<bool>();
    if (const auto assets = item.find("assets"); assets != item.end() && assets->is_array()) {
        for (const auto& asset : *assets) {
            if (!asset.is_object()) continue;
            ReleaseAsset file;
            file.name = Text(asset, "name");
            file.url = Text(asset, "browser_download_url");
            if (const auto size = asset.find("size"); size != asset.end() && size->is_number_unsigned()) file.size = size->get<std::uint64_t>();
            auto digest = Text(asset, "digest");
            if (digest.rfind("sha256:", 0) == 0 && digest.size() == 7 + 64) {
                file.sha256 = digest.substr(7);
                std::transform(file.sha256.begin(), file.sha256.end(), file.sha256.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            }
            if (!file.name.empty() && file.url.rfind("https://", 0) == 0) release.assets.push_back(std::move(file));
        }
    }
    return release;
}

nlohmann::json Parse(std::string_view json) {
    auto document = nlohmann::json::parse(json, nullptr, false);
    if (document.is_discarded()) throw std::runtime_error("GitHub's answer isn't JSON");
    if (document.is_object() && document.contains("message") && !document.contains("tag_name")) {
        throw std::runtime_error("GitHub: " + Text(document, "message"));
    }
    return document;
}

std::string FileSha256(const std::filesystem::path& file) {
    std::ifstream stream(file, std::ios::binary);
    std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
    std::uint8_t hash[SIZE_OF_SHA_256_HASH];
    calc_sha_256(hash, bytes.data(), bytes.size());
    std::string text;
    for (const auto byte : hash) {
        char pair[3];
        std::snprintf(pair, sizeof(pair), "%02x", byte);
        text += pair;
    }
    return text;
}

std::filesystem::path TarProgram() {
#ifdef _WIN32
    const char* root = std::getenv("SystemRoot");
    const auto system = FromUtf8(root != nullptr ? root : "C:\\Windows") / "System32" / "tar.exe";
    std::error_code error;
    if (!std::filesystem::is_regular_file(system, error)) throw std::runtime_error(ToUtf8(system) + " is missing; Windows 10 (1803) or later includes it");
    return system;
#else
    return "/usr/bin/tar";
#endif
}

void Fetch(const Downloader& download, const ReleaseAsset& asset, const std::filesystem::path& file, JobControl& control) {
    const auto before = control.Done();
    download(asset.url, file, control);
    ThrowIfCancelled(control);
    std::error_code error;
    const auto size = std::filesystem::file_size(file, error);
    if (asset.size != 0 && size != asset.size) throw std::runtime_error(asset.name + " is " + std::to_string(size) + " bytes, GitHub says " + std::to_string(asset.size));
    if (!asset.sha256.empty() && FileSha256(file) != asset.sha256) throw std::runtime_error(asset.name + " doesn't match the SHA-256 GitHub lists for it");
    const auto counted = control.Done() - before;
    if (counted < size) control.Advance(size - counted);
}

}

void FetchAsset(const Downloader& download, const ReleaseAsset& asset, const std::filesystem::path& file, JobControl& control) {
    Fetch(download, asset, file, control);
}

std::filesystem::path SystemTar() {
    return TarProgram();
}

bool IsValidRepository(std::string_view repository) {
    const auto slash = repository.find('/');
    if (slash == std::string_view::npos || slash == 0 || slash + 1 == repository.size() || repository.find('/', slash + 1) != std::string_view::npos) return false;
    const auto owner = repository.substr(0, slash);
    const auto name = repository.substr(slash + 1);
    return owner.size() <= 39 && name.size() <= 100 && name != "." && name != ".." && std::all_of(owner.begin(), owner.end(), [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '-'; }) &&
           std::all_of(name.begin(), name.end(), IsNameCharacter);
}

std::string ReleasesUrl(std::string_view repository) {
    return "https://api.github.com/repos/" + std::string(repository) + "/releases?per_page=30";
}

std::string LatestReleaseUrl(std::string_view repository) {
    return "https://api.github.com/repos/" + std::string(repository) + "/releases/latest";
}

std::vector<Release> ParseReleases(std::string_view json) {
    const auto document = Parse(json);
    if (!document.is_array()) throw std::runtime_error("GitHub's answer isn't a list of releases");
    std::vector<Release> releases;
    for (const auto& item : document) {
        if (auto release = ReleaseFrom(item)) releases.push_back(std::move(*release));
    }
    return releases;
}

Release ParseRelease(std::string_view json) {
    auto release = ReleaseFrom(Parse(json));
    if (!release) throw std::runtime_error("GitHub's answer isn't a usable release");
    return *release;
}

std::optional<ReleaseFiles> FilesFor(const Release& release, std::string* problem) {
#ifdef _WIN32
    const std::string relinker = "relinker-" + release.tag + ".exe";
    const std::string runtime = "prx-windows-" + release.tag + ".zip";
#else
    const std::string relinker = "relinker-" + release.tag;
    const std::string runtime = "prx-linux-" + release.tag + ".tar.gz";
#endif
    ReleaseFiles files;
    bool haveRelinker = false;
    bool haveRuntime = false;
    for (const auto& asset : release.assets) {
        if (asset.name == relinker) {
            files.relinker = asset;
            haveRelinker = true;
        } else if (asset.name == runtime) {
            files.runtime = asset;
            haveRuntime = true;
        }
    }
    if (haveRelinker && haveRuntime) return files;
    if (problem != nullptr) {
        *problem = "release " + release.tag + " has no " + (haveRelinker ? runtime : haveRuntime ? relinker : relinker + " and " + runtime);
    }
    return std::nullopt;
}

std::string VersionEntry::Label() const {
    if (Local()) return "local: " + ToUtf8(folder);
    return tag + (repository == OfficialRepository ? std::string() : " (" + repository + ")");
}

std::filesystem::path VersionsRoot(const std::filesystem::path& launcherFolder) {
    return launcherFolder / "anyps5";
}

VersionStore LoadVersions(const std::filesystem::path& root) {
    VersionStore store;
    std::ifstream stream(root / VersionsFileName, std::ios::binary);
    if (!stream) return store;
    const std::string text{std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
    const auto document = nlohmann::json::parse(text, nullptr, false);
    if (document.is_discarded() || !document.is_object()) return store;
    if (const auto list = document.find("versions"); list != document.end() && list->is_array()) {
        for (const auto& item : *list) {
            if (!item.is_object()) continue;
            VersionEntry entry;
            entry.id = Text(item, "id");
            entry.repository = Text(item, "repository");
            entry.tag = Text(item, "tag");
            entry.addedAt = Text(item, "addedAt");
            const auto folder = Text(item, "folder");
            if (entry.id.empty() || folder.empty()) continue;
            entry.folder = FromUtf8(folder);
            if (entry.folder.is_relative()) entry.folder = root / entry.folder;
            store.versions.push_back(std::move(entry));
        }
    }
    if (const auto list = document.find("repositories"); list != document.end() && list->is_array()) {
        for (const auto& item : *list) {
            if (item.is_string() && IsValidRepository(item.get<std::string>())) store.repositories.push_back(item.get<std::string>());
        }
    }
    if (const auto pins = document.find("pins"); pins != document.end() && pins->is_object()) {
        for (const auto& [serial, id] : pins->items()) {
            if (id.is_string() && !serial.empty()) store.pins[serial] = id.get<std::string>();
        }
    }
    return store;
}

void SaveVersions(const std::filesystem::path& root, const VersionStore& store) {
    nlohmann::json versions = nlohmann::json::array();
    for (const auto& entry : store.versions) {
        // Downloaded versions are stored relative to the root, so the launcher folder can move.
        auto folder = entry.folder;
        if (!entry.Local()) {
            const auto relative = entry.folder.lexically_relative(root);
            if (!relative.empty() && *relative.begin() != "..") folder = relative;
        }
        versions.push_back({{"id", entry.id}, {"repository", entry.repository}, {"tag", entry.tag}, {"folder", ToUtf8(folder.generic_u8string())}, {"addedAt", entry.addedAt}});
    }
    std::error_code error;
    std::filesystem::create_directories(root, error);
    nlohmann::json pins = nlohmann::json::object();
    for (const auto& [serial, id] : store.pins) pins[serial] = id;
    WriteFileAtomically(root / VersionsFileName, nlohmann::json{{"version", 1}, {"versions", versions}, {"repositories", store.repositories}, {"pins", pins}}.dump(4) + "\n");
}

const VersionEntry* FindVersion(const VersionStore& store, std::string_view id) {
    for (const auto& entry : store.versions) {
        if (entry.id == id) return &entry;
    }
    return nullptr;
}

void PutVersion(VersionStore& store, VersionEntry entry) {
    for (auto& existing : store.versions) {
        if (existing.id == entry.id) {
            existing = std::move(entry);
            return;
        }
    }
    store.versions.push_back(std::move(entry));
}

VersionEntry DownloadRelease(const std::filesystem::path& root, const std::string& repository, const Release& release,
                             const Downloader& download, const ProcessRunner& run, JobControl& control) {
    if (!IsValidRepository(repository)) throw std::runtime_error("\"" + repository + "\" isn't a GitHub repository (owner/name)");
    if (!IsSafeTag(release.tag)) throw std::runtime_error("unusable release tag \"" + release.tag + "\"");
    std::string problem;
    const auto files = FilesFor(release, &problem);
    if (!files) throw std::runtime_error(problem);
    const auto slash = repository.find('/');
    const auto folder = root / FromUtf8(repository.substr(0, slash)) / FromUtf8(repository.substr(slash + 1)) / FromUtf8(release.tag);
    auto partial = folder;
    partial += ".partial";
    RemoveOwnTree(partial);
    std::error_code error;
    std::filesystem::create_directories(partial, error);
    if (error) throw std::system_error(error, "cannot create " + ToUtf8(partial));
    control.SetTotal(files->relinker.size + files->runtime.size);
    try {
        const auto relinker = partial / FromUtf8(files->relinker.name);
        Fetch(download, files->relinker, relinker, control);
#ifndef _WIN32
        std::filesystem::permissions(relinker, std::filesystem::perms::owner_exec | std::filesystem::perms::group_exec | std::filesystem::perms::others_exec,
                                     std::filesystem::perm_options::add, error);
#endif
        const auto archive = partial / FromUtf8(files->runtime.name);
        Fetch(download, files->runtime, archive, control);
        const auto log = partial / "unpack.log";
        const int code = run({ToUtf8(TarProgram()), "-xf", ToUtf8(archive), "-C", ToUtf8(partial)}, partial, log, control);
        if (code != 0) throw std::runtime_error("unpacking " + files->runtime.name + " failed (tar exit code " + std::to_string(code) + ")");
        std::filesystem::remove(archive, error);
        std::filesystem::remove(log, error);
        try {
            JobControl part;
            download("https://raw.githubusercontent.com/" + repository + "/" + release.tag + "/tools/titles.json", partial / "tools" / "titles.json", part);
        } catch (const Cancelled&) {
            throw;
        } catch (const std::exception&) {
            // Optional: without it titles get the relinker's default flags.
        }
        const auto probe = ProbeAnyPS5(ToUtf8(partial));
        if (!probe.runtime) throw std::runtime_error("the downloaded release isn't usable: " + probe.problem);
        RemoveOwnTree(folder);
        RenameNew(partial, folder);
    } catch (...) {
        try {
            RemoveOwnTree(partial);
        } catch (...) {
        }
        throw;
    }
    VersionEntry entry;
    entry.id = repository + "@" + release.tag;
    entry.repository = repository;
    entry.tag = release.tag;
    entry.folder = folder;
    entry.addedAt = Now();
    return entry;
}

VersionEntry LocalVersion(const std::filesystem::path& relinkerOrFolder) {
    std::error_code error;
    const auto start = std::filesystem::absolute(relinkerOrFolder, error).lexically_normal();
    std::vector<std::filesystem::path> candidates;
    if (std::filesystem::is_directory(start, error)) {
        candidates.push_back(start);
    } else {
        const auto parent = start.parent_path();
        candidates.push_back(parent);
        // <clone>/build/core/relinker/relinker.exe
        if (parent.filename() == "relinker" && parent.parent_path().filename() == "core") candidates.push_back(parent.parent_path().parent_path().parent_path());
    }
    std::string problem;
    for (const auto& candidate : candidates) {
        const auto probe = ProbeAnyPS5(ToUtf8(candidate));
        if (probe.runtime) {
            VersionEntry entry;
            entry.id = "local:" + ToUtf8(candidate);
            entry.folder = candidate;
            entry.addedAt = Now();
            return entry;
        }
        if (problem.empty()) problem = probe.problem;
    }
    throw std::runtime_error("not a usable AnyPS5: " + problem +
                             ". Pick the relinker of an AnyPS5 build (build\\core\\relinker), or one with the runtime (libs\\) of the same release beside it");
}

void RemoveVersion(const std::filesystem::path& root, VersionStore& store, std::string_view id) {
    const auto found = std::find_if(store.versions.begin(), store.versions.end(), [&](const VersionEntry& entry) { return entry.id == id; });
    if (found == store.versions.end()) return;
    if (!found->Local()) {
        const auto relative = found->folder.lexically_normal().lexically_relative(root.lexically_normal());
        if (!relative.empty() && *relative.begin() != ".." && !relative.is_absolute()) RemoveOwnTree(found->folder);
    }
    store.versions.erase(found);
    // Games pinned to it go back to the selected version.
    for (auto it = store.pins.begin(); it != store.pins.end();) {
        if (it->second == id) it = store.pins.erase(it);
        else ++it;
    }
}

std::optional<Release> NewestRelease(const std::vector<Release>& releases, bool includePrereleases) {
    std::optional<Release> newest;
    for (const auto& release : releases) {
        if (release.prerelease && !includePrereleases) continue;
        if (!FilesFor(release)) continue;
        if (!newest || release.publishedAt > newest->publishedAt) newest = release;
    }
    return newest;
}

}
