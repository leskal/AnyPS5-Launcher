#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>

namespace launcher {

// How long a profile has played each game: the time from the game's window showing
// to its exit, added up over its sessions. Kept per profile and title ID, like saves,
// in <profile>/playtime.json.
struct PlayRecord {
    std::uint64_t seconds = 0;
    int sessions = 0;
    std::int64_t lastPlayed = 0;  // Unix time of the last minute counted; 0: never
};

using Playtimes = std::map<std::string, PlayRecord>;

// A missing or unreadable file reads as nothing played; bad entries are skipped.
Playtimes LoadPlaytimes(const std::filesystem::path& file);
void SavePlaytimes(const std::filesystem::path& file, const Playtimes& playtimes);

// Adds seconds to a game's time (and one session when newSession) and saves at once,
// so a crash keeps what was counted. Returns the updated record.
PlayRecord AddPlaytime(const std::filesystem::path& file, const std::string& serial, std::uint64_t seconds, bool newSession, std::int64_t now);

// "Under a minute", "45 min", "3 h 10 min", "120 h".
std::string FormatPlaytime(std::uint64_t seconds);

}
