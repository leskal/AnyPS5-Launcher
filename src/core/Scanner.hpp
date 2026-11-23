#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "core/Executable.hpp"
#include "core/Metadata.hpp"

namespace launcher {

enum class PackageFormat {
    Folder,
    Zar,
};

enum class ZarLayout {
    NotApplicable,
    Root,
    Nested,
};

// Files directly inside sce_module, sce_modules and prx, by first bytes.
// Launcher backups (*.self) are not counted.
struct ModuleSummary {
    int elf = 0;
    int self = 0;
};

// A game folder the launcher has set up for AnyPS5 holds this file. Beside it:
//   <serial>.exe   the relinker's output
//   app0\          the game's files (folder game), or only the files the
//                  relinker and start-up need (.zar game)
//   app0.zar       the archive, moved here unchanged (.zar game)
inline constexpr const char* GameHomeMarker = "anyps5-launcher-install.json";

struct GameEntry {
    std::filesystem::path location;  // the game folder, its app0\ or the .zar
    std::filesystem::path home;      // the set-up folder; empty when not set up
    PackageFormat format = PackageFormat::Folder;
    ZarLayout layout = ZarLayout::NotApplicable;
    std::string innerPrefix;
    GameMetadata metadata;
    ExecutableInfo executable;
    std::optional<ExecutableInfo> unwrapped;
    ModuleSummary modules;
    std::vector<std::uint8_t> icon;
};

struct ScanProblem {
    std::filesystem::path location;
    std::string message;
};

struct ScanOptions {
    std::vector<std::filesystem::path> roots;
    std::vector<std::filesystem::path> excluded;
    std::string preferredLocale;
    std::uint64_t maxMetadataSize = 1u << 20;
    std::uint64_t maxIconSize = 8u << 20;
};

struct ScanResult {
    std::vector<GameEntry> games;
    std::vector<ScanProblem> problems;
};

ScanResult Scan(const ScanOptions& options);

std::map<std::string, std::vector<std::size_t>> GroupByTitleId(const std::vector<GameEntry>& games);

std::string_view ToString(PackageFormat format);
std::string_view ToString(ZarLayout layout);

}
