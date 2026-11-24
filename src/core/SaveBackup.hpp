#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/Job.hpp"
#include "core/Profiles.hpp"

namespace launcher {

// A game's saves and trophies in one profile, kept as zip files in
// <userdata>/backups/<profile>/<serial>/<yyyy-mm-dd_hh-mm-ss>.zip with the
// entries _sd/..., _sd_mem/... and _trophy/... (the folders AnyPS5 uses).
struct SaveBackup {
    std::filesystem::path file;
    std::string created;  // "2026-11-24 19:42:10", from the file name
    std::uint64_t size = 0;
};

std::filesystem::path SaveBackupFolder(const Layout& layout, std::string_view serial);

// The folders a backup holds, by their name in the zip: _sd, _sd_mem, _trophy.
std::vector<std::pair<std::string, std::filesystem::path>> SaveRoots(const Layout& layout, std::string_view serial);

// Zips the files under roots (name -> folder) into folder/<stamp><suffix>.zip,
// read back before it's kept. Throws when there is nothing to zip.
std::filesystem::path ZipFolders(const std::vector<std::pair<std::string, std::filesystem::path>>& roots, const std::filesystem::path& folder,
                                 const std::string& suffix, JobControl& control);

// True when the profile has anything saved for the game.
bool HasSaves(const Layout& layout, std::string_view serial);

// Zips the game's saves and trophies; returns the new zip. Throws when there is nothing to back up.
std::filesystem::path BackUpSaves(const Layout& layout, std::string_view serial, JobControl& control);

// Newest first.
std::vector<SaveBackup> ListSaveBackups(const Layout& layout, std::string_view serial);

// Puts a backup's saves and trophies in place of the current ones, after backing
// those up first (returned, or nullopt when there was nothing to keep). Every
// file is checked against its CRC-32 before anything current is replaced.
std::optional<std::filesystem::path> RestoreSaves(const Layout& layout, std::string_view serial, const std::filesystem::path& backup, JobControl& control);

}
