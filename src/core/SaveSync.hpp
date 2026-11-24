#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include "core/Job.hpp"
#include "core/Profiles.hpp"

namespace launcher {

// Keeps a game's saves and trophies the same on several PCs through a folder
// they all see (OneDrive, Dropbox, a USB stick, a network share):
//   <sync folder>/<profile>/<serial>/_sd, _sd_mem, _trophy
// The newest side wins, judged by the newest file in each. Whatever is about
// to be replaced is zipped into the profile's save backups first, so nothing
// is lost when two PCs played without syncing in between.
struct SyncResult {
    enum class Action { Nothing, Pulled, Pushed };
    Action action = Action::Nothing;
    std::optional<std::filesystem::path> backup;  // what was replaced, zipped
};

std::filesystem::path SyncFolderFor(const std::filesystem::path& syncRoot, const Layout& layout, std::string_view serial);

// Before a game starts: newer saves in the sync folder replace this PC's.
SyncResult PullSaves(const Layout& layout, std::string_view serial, const std::filesystem::path& syncRoot, JobControl& control);

// After a game ends: this PC's saves replace older ones in the sync folder.
SyncResult PushSaves(const Layout& layout, std::string_view serial, const std::filesystem::path& syncRoot, JobControl& control);

}
