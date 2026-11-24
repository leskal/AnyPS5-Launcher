#include "core/SaveSync.hpp"

#include <chrono>
#include <system_error>
#include <utility>
#include <vector>

#include "core/FileOps.hpp"
#include "core/SaveBackup.hpp"
#include "core/Utf8.hpp"

namespace launcher {

namespace {

using Roots = std::vector<std::pair<std::string, std::filesystem::path>>;

// File times on network shares and FAT drives are coarse: two seconds apart counts as equal.
constexpr auto Tolerance = std::chrono::seconds(2);

std::optional<std::filesystem::file_time_type> Newest(const Roots& roots) {
    std::optional<std::filesystem::file_time_type> newest;
    for (const auto& [name, root] : roots) {
        std::error_code error;
        if (!std::filesystem::is_directory(root, error)) continue;
        for (std::filesystem::recursive_directory_iterator it(root, error), end; !error && it != end; it.increment(error)) {
            if (!it->is_regular_file(error)) continue;
            const auto time = it->last_write_time(error);
            if (!error && (!newest || time > *newest)) newest = time;
        }
    }
    return newest;
}

Roots RemoteRoots(const std::filesystem::path& folder) {
    return {{"_sd", folder / "_sd"}, {"_sd_mem", folder / "_sd_mem"}, {"_trophy", folder / "_trophy"}};
}

// Copies the files under each root of from into the same root of to, keeping
// their times, through "<to>.partial" so a half-copied folder is never used.
void CopyRoots(const Roots& from, const Roots& to, JobControl& control) {
    for (std::size_t i = 0; i < from.size(); ++i) {
        const auto& source = from[i].second;
        const auto& target = to[i].second;
        auto partial = target;
        partial += ".partial";
        std::error_code error;
        std::filesystem::remove_all(partial, error);
        if (std::filesystem::is_directory(source, error)) {
            for (std::filesystem::recursive_directory_iterator it(source, error), end; !error && it != end; it.increment(error)) {
                ThrowIfCancelled(control);
                const auto destination = partial / it->path().lexically_relative(source);
                if (it->is_directory()) {
                    std::filesystem::create_directories(destination);
                } else if (it->is_regular_file()) {
                    std::filesystem::create_directories(destination.parent_path());
                    std::filesystem::copy_file(it->path(), destination, std::filesystem::copy_options::overwrite_existing);
                    std::filesystem::last_write_time(destination, it->last_write_time());
                    control.Advance(it->file_size());
                }
            }
            if (error) throw std::system_error(error, "cannot read " + ToUtf8(source));
        }
        std::filesystem::remove_all(target, error);
        if (error) throw std::system_error(error, "cannot replace " + ToUtf8(target));
        if (std::filesystem::exists(partial)) {
            std::filesystem::create_directories(target.parent_path());
            std::filesystem::rename(partial, target);
        }
    }
}

}

std::filesystem::path SyncFolderFor(const std::filesystem::path& syncRoot, const Layout& layout, std::string_view serial) {
    return syncRoot / FromUtf8(layout.profile) / std::string(serial);
}

SyncResult PullSaves(const Layout& layout, std::string_view serial, const std::filesystem::path& syncRoot, JobControl& control) {
    SyncResult result;
    const auto remote = RemoteRoots(SyncFolderFor(syncRoot, layout, serial));
    const auto local = SaveRoots(layout, serial);
    const auto remoteTime = Newest(remote);
    if (!remoteTime) return result;  // nothing synced yet
    const auto localTime = Newest(local);
    if (localTime && *remoteTime <= *localTime + Tolerance) return result;
    if (localTime) result.backup = ZipFolders(local, SaveBackupFolder(layout, serial), "-before-sync", control);
    CopyRoots(remote, local, control);
    result.action = SyncResult::Action::Pulled;
    return result;
}

SyncResult PushSaves(const Layout& layout, std::string_view serial, const std::filesystem::path& syncRoot, JobControl& control) {
    SyncResult result;
    const auto remote = RemoteRoots(SyncFolderFor(syncRoot, layout, serial));
    const auto local = SaveRoots(layout, serial);
    const auto localTime = Newest(local);
    if (!localTime) return result;  // nothing to share
    const auto remoteTime = Newest(remote);
    if (remoteTime && *localTime <= *remoteTime + Tolerance) return result;
    if (remoteTime) result.backup = ZipFolders(remote, SaveBackupFolder(layout, serial), "-sync-folder", control);
    CopyRoots(local, remote, control);
    result.action = SyncResult::Action::Pushed;
    return result;
}

}
