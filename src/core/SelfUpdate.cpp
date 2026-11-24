#include "core/SelfUpdate.hpp"

#include <cctype>
#include <stdexcept>
#include <system_error>

#include "core/FileOps.hpp"
#include "core/Metadata.hpp"
#include "core/Utf8.hpp"

namespace launcher {

namespace {

std::string_view WithoutV(std::string_view tag) {
    if (!tag.empty() && (tag.front() == 'v' || tag.front() == 'V')) tag.remove_prefix(1);
    return tag;
}

}

std::string LauncherAssetName(std::string_view tag) {
    return "anyps5-launcher-" + std::string(tag) + "-windows-x64.zip";
}

std::optional<LauncherUpdate> FindLauncherUpdate(const std::vector<Release>& releases, std::string_view currentVersion) {
    std::optional<LauncherUpdate> best;
    for (const auto& release : releases) {
        if (release.prerelease) continue;
        if (CompareVersions(WithoutV(release.tag), WithoutV(currentVersion)) <= 0) continue;
        if (best && CompareVersions(WithoutV(release.tag), WithoutV(best->release.tag)) <= 0) continue;
        const auto name = LauncherAssetName(release.tag);
        for (const auto& asset : release.assets) {
            if (asset.name == name) best = LauncherUpdate{release, asset};
        }
    }
    return best;
}

std::filesystem::path StageLauncherUpdate(const LauncherUpdate& update, const std::filesystem::path& launcherFolder, const Downloader& download,
                                          const ProcessRunner& run, JobControl& control) {
    for (const char c : update.release.tag) {
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '.' && c != '-' && c != '_') throw std::runtime_error("the release tag \"" + update.release.tag + "\" isn't usable as a folder name");
    }
    const auto folder = launcherFolder / "update" / update.release.tag;
    std::error_code error;
    std::filesystem::remove_all(folder, error);
    std::filesystem::create_directories(folder);
    control.SetTotal(update.asset.size + 1);
    const auto zip = folder / update.asset.name;
    FetchAsset(download, update.asset, zip, control);
    const auto unpacked = folder / "files";
    std::filesystem::create_directories(unpacked);
    const int code = run({ToUtf8(SystemTar()), "-xf", ToUtf8(zip), "-C", ToUtf8(unpacked)}, unpacked, folder / "unpack.log", control);
    if (code != 0) throw std::runtime_error("unpacking " + update.asset.name + " failed (tar exit code " + std::to_string(code) + ")");
    for (std::filesystem::recursive_directory_iterator it(unpacked, error), end; !error && it != end; it.increment(error)) {
        if (it->is_regular_file() && ToUtf8(it->path().filename()) == LauncherExeName) {
            control.Advance(1);
            return it->path();
        }
    }
    throw std::runtime_error(update.asset.name + " has no " + LauncherExeName);
}

void SwapLauncherExe(const std::filesystem::path& current, const std::filesystem::path& fresh) {
    auto old = current;
    old += ".old";
    std::error_code error;
    std::filesystem::remove(old, error);
    std::filesystem::rename(current, old);
    try {
        std::filesystem::copy_file(fresh, current, std::filesystem::copy_options::overwrite_existing);
    } catch (...) {
        std::filesystem::rename(old, current, error);  // put the running one back
        throw;
    }
}

void CleanUpLauncherUpdate(const std::filesystem::path& current, const std::filesystem::path& launcherFolder) {
    auto old = current;
    old += ".old";
    std::error_code error;
    std::filesystem::remove(old, error);
    std::filesystem::remove_all(launcherFolder / "update", error);
}

}
