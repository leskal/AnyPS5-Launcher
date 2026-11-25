#include "core/SelfUpdate.hpp"

#include <algorithm>
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

std::vector<NoteLine> ReleaseNoteLines(std::string_view notes, std::size_t maxLines) {
    std::vector<NoteLine> lines;
    std::size_t start = 0;
    while (start <= notes.size() && lines.size() < maxLines) {
        auto end = notes.find('\n', start);
        if (end == std::string_view::npos) end = notes.size();
        std::string line(notes.substr(start, end - start));
        start = end + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        // [text](url) -> text
        for (auto open = line.find('['); open != std::string::npos; open = line.find('[', open + 1)) {
            const auto close = line.find("](", open);
            const auto paren = close == std::string::npos ? std::string::npos : line.find(')', close);
            if (paren == std::string::npos) break;
            line = line.substr(0, open) + line.substr(open + 1, close - open - 1) + line.substr(paren + 1);
        }
        for (auto bold = line.find("**"); bold != std::string::npos; bold = line.find("**")) line.erase(bold, 2);
        std::size_t lead = line.find_first_not_of(" \t");
        if (lead == std::string::npos) {
            if (!lines.empty() && !lines.back().text.empty()) lines.emplace_back();
            continue;
        }
        line.erase(0, lead);
        const bool heading = !line.empty() && line.front() == '#';
        while (!line.empty() && line.front() == '#') line.erase(0, 1);
        if (line.size() > 1 && (line[0] == '*' || line[0] == '-') && line[1] == ' ') line = "\xE2\x80\xA2 " + line.substr(2);  // a bullet
        line.erase(0, std::min(line.find_first_not_of(' '), line.size()));
        while (!line.empty() && line.back() == ' ') line.pop_back();
        if (line.rfind("Full Changelog", 0) == 0) continue;
        if (line.empty()) continue;
        // A heading after text gets a gap above it.
        if (heading && !lines.empty() && !lines.back().text.empty()) lines.emplace_back();
        if (lines.size() >= maxLines) break;
        lines.push_back({std::move(line), heading});
    }
    while (!lines.empty() && lines.back().text.empty()) lines.pop_back();
    return lines;
}

std::string FormatWindowArgument(const WindowPlacement& placement) {
    return "--window=" + std::to_string(placement.x) + "," + std::to_string(placement.y) + "," + std::to_string(placement.width) + "," +
           std::to_string(placement.height) + (placement.maximized ? ",max" : "");
}

std::optional<WindowPlacement> ParseWindowArgument(const std::vector<std::string>& arguments) {
    constexpr std::string_view prefix = "--window=";
    for (const auto& argument : arguments) {
        if (argument.rfind(prefix, 0) != 0) continue;
        std::vector<std::string> parts;
        std::size_t start = prefix.size();
        while (true) {
            const auto comma = argument.find(',', start);
            parts.push_back(argument.substr(start, comma == std::string::npos ? std::string::npos : comma - start));
            if (comma == std::string::npos) break;
            start = comma + 1;
        }
        if (parts.size() != 4 && !(parts.size() == 5 && parts[4] == "max")) return std::nullopt;
        int values[4] = {};
        for (int i = 0; i < 4; ++i) {
            const auto& part = parts[static_cast<std::size_t>(i)];
            if (part.empty() || part.size() > 6) return std::nullopt;
            std::size_t digits = part[0] == '-' ? 1 : 0;
            if (digits == part.size()) return std::nullopt;
            for (std::size_t c = digits; c < part.size(); ++c) {
                if (part[c] < '0' || part[c] > '9') return std::nullopt;
            }
            values[i] = std::stoi(part);
        }
        if (values[2] < 200 || values[3] < 150) return std::nullopt;  // too small to be a real window
        return WindowPlacement{values[0], values[1], values[2], values[3], parts.size() == 5};
    }
    return std::nullopt;
}

void CleanUpLauncherUpdate(const std::filesystem::path& current, const std::filesystem::path& launcherFolder) {
    auto old = current;
    old += ".old";
    std::error_code error;
    std::filesystem::remove(old, error);
    std::filesystem::remove_all(launcherFolder / "update", error);
}

}
