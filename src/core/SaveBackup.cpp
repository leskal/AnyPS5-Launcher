#include "core/SaveBackup.hpp"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <stdexcept>
#include <system_error>
#include <utility>

#include "core/FileOps.hpp"
#include "core/Utf8.hpp"
#include "core/ZipStore.hpp"

namespace launcher {

namespace {

std::vector<std::pair<std::string, std::filesystem::path>> FilesUnder(const std::vector<std::pair<std::string, std::filesystem::path>>& roots) {
    std::vector<std::pair<std::string, std::filesystem::path>> files;
    for (const auto& [name, root] : roots) {
        std::error_code error;
        if (!std::filesystem::is_directory(root, error)) continue;
        for (std::filesystem::recursive_directory_iterator it(root, error), end; !error && it != end; it.increment(error)) {
            if (!it->is_regular_file(error)) continue;
            auto relative = ToUtf8(it->path().lexically_relative(root));
            std::replace(relative.begin(), relative.end(), '\\', '/');
            files.emplace_back(name + "/" + relative, it->path());
        }
    }
    std::sort(files.begin(), files.end());
    return files;
}

std::vector<std::pair<std::string, std::filesystem::path>> FilesIn(const Layout& layout, std::string_view serial) {
    return FilesUnder(SaveRoots(layout, serial));
}

std::string Stamp() {
    const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    char text[32];
    std::strftime(text, sizeof(text), "%Y-%m-%d_%H-%M-%S", &local);
    return text;
}

// A name inside the zip must stay inside its root: no "..", no absolute paths.
bool SafeEntry(const std::string& name) {
    if (name.empty() || name.front() == '/' || name.find('\\') != std::string::npos || name.find(':') != std::string::npos) return false;
    std::size_t start = 0;
    while (start <= name.size()) {
        auto end = name.find('/', start);
        if (end == std::string::npos) end = name.size();
        const auto part = name.substr(start, end - start);
        if (part.empty() || part == "." || part == "..") return false;
        start = end + 1;
    }
    return true;
}

}

std::vector<std::pair<std::string, std::filesystem::path>> SaveRoots(const Layout& layout, std::string_view serial) {
    return {{"_sd", layout.Saves(serial) / "_sd"}, {"_sd_mem", layout.Saves(serial) / "_sd_mem"}, {"_trophy", layout.Trophies(serial)}};
}

std::filesystem::path SaveBackupFolder(const Layout& layout, std::string_view serial) {
    return layout.userdata / "backups" / FromUtf8(layout.profile) / std::string(serial);
}

bool HasSaves(const Layout& layout, std::string_view serial) {
    return !FilesIn(layout, serial).empty();
}

std::filesystem::path ZipFolders(const std::vector<std::pair<std::string, std::filesystem::path>>& roots, const std::filesystem::path& folder,
                                 const std::string& suffix, JobControl& control) {
    std::filesystem::create_directories(folder);
    const auto stamp = Stamp();
    auto target = folder / (stamp + suffix + ".zip");
    for (int n = 2; std::filesystem::exists(target); ++n) target = folder / (stamp + suffix + "-" + std::to_string(n) + ".zip");
    ZipFoldersTo(roots, target, control);
    return target;
}

void ZipFoldersTo(const std::vector<std::pair<std::string, std::filesystem::path>>& roots, const std::filesystem::path& target, JobControl& control) {
    const auto files = FilesUnder(roots);
    if (files.empty()) throw std::runtime_error("there is nothing to back up");
    std::uint64_t total = 0;
    std::error_code error;
    for (const auto& [name, file] : files) total += std::filesystem::file_size(file, error);
    control.SetTotal(control.Total() + total + 1);
    auto partial = target;
    partial += ".partial";
    try {
        {
            ZipWriter writer(partial);
            for (const auto& [name, file] : files) writer.AddFile(name, file, control);
            writer.Finish();
        }
        {
            ZipReader check(partial);  // a backup that can't be read back is no backup
            if (check.Entries().size() != files.size()) throw std::runtime_error("the backup zip didn't read back whole");
        }
        // The user may pick an existing file to overwrite: it goes only once the new one reads back.
        std::filesystem::remove(target, error);
        RenameNew(partial, target);
    } catch (...) {
        std::filesystem::remove(partial, error);
        throw;
    }
    control.Advance(1);
}

AllSavesBackup BackUpAllSaves(const std::filesystem::path& userdata, const std::vector<std::string>& profiles, const std::filesystem::path& target,
                              JobControl& control) {
    AllSavesBackup result;
    std::vector<std::pair<std::string, std::filesystem::path>> roots;
    for (const auto& profile : profiles) {
        const Layout layout{userdata, {}, profile};
        std::vector<std::string> serials;
        std::error_code error;
        for (const auto& folder : {layout.Profile() / "saves", layout.Profile() / "_trophy"}) {
            for (std::filesystem::directory_iterator it(folder, error), end; !error && it != end; it.increment(error)) {
                if (it->is_directory(error)) serials.push_back(ToUtf8(it->path().filename()));
            }
            error.clear();
        }
        std::sort(serials.begin(), serials.end());
        serials.erase(std::unique(serials.begin(), serials.end()), serials.end());
        bool any = false;
        for (const auto& serial : serials) {
            if (!HasSaves(layout, serial)) continue;
            for (const auto& [name, folder] : SaveRoots(layout, serial)) roots.emplace_back(profile + "/" + serial + "/" + name, folder);
            ++result.games;
            any = true;
        }
        if (any) ++result.profiles;
    }
    if (roots.empty()) throw std::runtime_error(profiles.size() == 1 ? "this profile has no saves yet" : "no profile has saves yet");
    ZipFoldersTo(roots, target, control);
    return result;
}

std::filesystem::path BackUpSaves(const Layout& layout, std::string_view serial, JobControl& control) {
    if (!HasSaves(layout, serial)) throw std::runtime_error("profile " + layout.profile + " has no saves or trophies of " + std::string(serial) + " yet");
    return ZipFolders(SaveRoots(layout, serial), SaveBackupFolder(layout, serial), "", control);
}

std::vector<SaveBackup> ListSaveBackups(const Layout& layout, std::string_view serial) {
    std::vector<SaveBackup> backups;
    std::error_code error;
    for (std::filesystem::directory_iterator it(SaveBackupFolder(layout, serial), error), end; !error && it != end; it.increment(error)) {
        if (!it->is_regular_file(error) || it->path().extension() != ".zip") continue;
        SaveBackup backup;
        backup.file = it->path();
        backup.size = it->file_size(error);
        auto stem = ToUtf8(it->path().stem());
        // 2026-11-24_19-42-10 -> 2026-11-24 19:42:10
        if (stem.size() >= 19 && stem[10] == '_') {
            backup.created = stem.substr(0, 10) + " " + stem.substr(11, 2) + ":" + stem.substr(14, 2) + ":" + stem.substr(17, 2);
        } else {
            backup.created = stem;
        }
        backups.push_back(std::move(backup));
    }
    std::sort(backups.begin(), backups.end(), [](const SaveBackup& a, const SaveBackup& b) { return a.file.filename() > b.file.filename(); });
    return backups;
}

std::optional<std::filesystem::path> RestoreSaves(const Layout& layout, std::string_view serial, const std::filesystem::path& backup, JobControl& control) {
    const auto roots = SaveRoots(layout, serial);
    const auto staging = layout.Saves(serial).parent_path() / (std::string(serial) + ".restoring");
    std::error_code error;
    std::filesystem::remove_all(staging, error);

    // 1. Unpack the backup beside the saves, every file checked.
    {
        ZipReader reader(backup);
        std::uint64_t total = 0;
        for (const auto& entry : reader.Entries()) total += entry.size;
        control.SetTotal(total + 1);
        for (const auto& entry : reader.Entries()) {
            if (!SafeEntry(entry.name)) throw std::runtime_error("the backup holds an unsafe name: " + entry.name);
            const auto slash = entry.name.find('/');
            const auto root = entry.name.substr(0, slash);
            if (std::none_of(roots.begin(), roots.end(), [&](const auto& known) { return known.first == root; })) continue;
            const auto target = staging / FromUtf8(entry.name);
            std::filesystem::create_directories(target.parent_path());
            reader.Extract(entry, target, control);
        }
    }

    // 2. Keep what is there now.
    std::optional<std::filesystem::path> kept;
    try {
        if (HasSaves(layout, serial)) kept = BackUpSaves(layout, serial, control);
    } catch (...) {
        std::filesystem::remove_all(staging, error);
        throw;
    }

    // 3. Swap: each folder is replaced by the backup's (or removed when the backup has none).
    for (const auto& [name, root] : roots) {
        std::filesystem::remove_all(root, error);
        if (error) throw std::system_error(error, "cannot clear " + ToUtf8(root) + "; the backup of the current saves is " + (kept ? ToUtf8(*kept) : std::string("not needed")));
        const auto unpacked = staging / name;
        if (std::filesystem::is_directory(unpacked, error)) {
            std::filesystem::create_directories(root.parent_path());
            MoveTree(unpacked, root);
        }
    }
    std::filesystem::remove_all(staging, error);
    control.Advance(1);
    return kept;
}

}
