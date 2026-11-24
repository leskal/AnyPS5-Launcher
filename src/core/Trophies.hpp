#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace launcher {

class PackageReader;

// A game's trophies: the list, texts and icons from the trophy packages in the
// dump (sce_sys/trophy2/trophyNN.ucp), and what AnyPS5 recorded as earned in the
// profile's _trophy folder (<user>_<label>.txt: "<trophy id> <tick>" lines, and
// <user>_<label>.stats.txt: "<stat id> <value>" lines for progress).

enum class TrophyGrade { Platinum, Gold, Silver, Bronze };

const char* TrophyGradeName(TrophyGrade grade);

struct Trophy {
    std::uint32_t label = 0;  // the package's service label (the NN of trophyNN.ucp)
    std::int32_t id = 0;
    TrophyGrade grade = TrophyGrade::Bronze;
    bool hidden = false;
    std::string name;
    std::string detail;
    std::vector<std::uint8_t> icon;  // PNG, may be empty
    std::optional<std::int64_t> earnedAt;  // seconds since 1970, UTC
    std::optional<std::uint64_t> statId;   // progressive trophies: the counted stat
    std::uint64_t target = 0;              // and the value that earns it
    std::uint64_t progress = 0;
};

struct TrophyCount {
    int earned = 0;
    int total = 0;
};

struct TrophySet {
    std::string title;
    std::vector<std::uint8_t> icon;
    std::vector<Trophy> trophies;

    TrophyCount Count() const;
    TrophyCount Count(TrophyGrade grade) const;
    // Weighted as on the console: gold 90, silver 30, bronze 15; the platinum doesn't count.
    int Percent() const;
};

// The files of a .ucp package by name. Empty when the data isn't a valid package.
std::map<std::string, std::vector<std::uint8_t>> ReadUcp(std::span<const std::uint8_t> data);

// One package's trophies in the first of languages it has (then en-US, en-GB, its default).
std::optional<TrophySet> ParseTrophyPackage(const std::map<std::string, std::vector<std::uint8_t>>& files, std::uint32_t label,
                                            const std::vector<std::string>& languages);

struct TrophyRecords {
    std::map<std::pair<std::uint32_t, std::int32_t>, std::uint64_t> unlocks;  // (label, trophy) -> tick
    std::map<std::pair<std::uint32_t, std::uint64_t>, std::uint64_t> stats;   // (label, stat) -> value
};

// Every user's records in the folder; damaged lines are skipped.
TrophyRecords ReadTrophyRecords(const std::filesystem::path& folder);

// Marks earned trophies and fills in progress.
void ApplyTrophyRecords(TrophySet& set, const TrophyRecords& records);

// Reads every sce_sys/trophy2/trophyNN.ucp of the game. nullopt when it has none.
std::optional<TrophySet> LoadTrophies(PackageReader& package, const std::vector<std::string>& languages);

// The trophy text language for a profile's console language ("", "auto" or "0".."30").
std::vector<std::string> TrophyLanguages(std::string_view consoleLanguage);

}
