#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace launcher {

// While a game runs, the folders AnyPS5 keeps beside the exe (_sd, _sd_mem,
// _trophy, data, download0) are the profile's own folders, moved there. This
// file beside the exe lists where each came from, so they can be moved back
// after the game exits, or the next time the launcher starts if it didn't.
inline constexpr const char* SessionMarker = "anyps5-launcher-session.json";

struct SessionItem {
    std::string name;               // folder name beside the exe
    std::filesystem::path origin;   // where it lives between runs
};

bool HasSession(const std::filesystem::path& home);

// Moves each origin to home/name. A folder already at home/name is removed when
// empty; one with files stops the start and is named. On failure everything
// moved so far is moved back.
void MoveInSession(const std::filesystem::path& home, const std::vector<SessionItem>& items);

// Moves everything listed in the marker back and deletes the marker. Returns the
// names it moved.
std::vector<std::string> MoveOutSession(const std::filesystem::path& home);

}
