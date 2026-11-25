#include "core/Playtime.hpp"

#include "core/Utf8.hpp"

#include <nlohmann/json.hpp>

#include <fstream>
#include <iterator>
#include <stdexcept>
#include <algorithm>
#include <system_error>

namespace launcher {

Playtimes LoadPlaytimes(const std::filesystem::path& file) {
    Playtimes playtimes;
    std::ifstream in(file, std::ios::binary);
    if (!in) return playtimes;
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const auto document = nlohmann::json::parse(text, nullptr, false);
    if (!document.is_object()) return playtimes;
    const auto games = document.find("games");
    if (games == document.end() || !games->is_object()) return playtimes;
    for (const auto& [serial, entry] : games->items()) {
        if (serial.empty() || !entry.is_object()) continue;
        PlayRecord record;
        const auto seconds = entry.find("seconds");
        if (seconds == entry.end() || !seconds->is_number_unsigned()) continue;
        record.seconds = seconds->get<std::uint64_t>();
        if (const auto sessions = entry.find("sessions"); sessions != entry.end() && sessions->is_number_integer()) {
            record.sessions = std::max(0, sessions->get<int>());
        }
        if (const auto last = entry.find("lastPlayed"); last != entry.end() && last->is_number_integer()) {
            record.lastPlayed = last->get<std::int64_t>();
        }
        playtimes[serial] = record;
    }
    return playtimes;
}

void SavePlaytimes(const std::filesystem::path& file, const Playtimes& playtimes) {
    nlohmann::json games = nlohmann::json::object();
    for (const auto& [serial, record] : playtimes) {
        games[serial] = {{"seconds", record.seconds}, {"sessions", record.sessions}, {"lastPlayed", record.lastPlayed}};
    }
    const nlohmann::json document = {{"version", 1}, {"games", games}};
    std::filesystem::create_directories(file.parent_path());
    // Written beside and renamed over, so a crash mid-write leaves the old file.
    auto partial = file;
    partial += ".partial";
    {
        std::ofstream out(partial, std::ios::binary | std::ios::trunc);
        out << document.dump(2) << '\n';
        if (!out) throw std::runtime_error("cannot write " + ToUtf8(partial));
    }
    std::filesystem::rename(partial, file);
}

PlayRecord AddPlaytime(const std::filesystem::path& file, const std::string& serial, std::uint64_t seconds, bool newSession, std::int64_t now) {
    auto playtimes = LoadPlaytimes(file);
    auto& record = playtimes[serial];
    record.seconds += seconds;
    if (newSession) ++record.sessions;
    record.lastPlayed = now;
    SavePlaytimes(file, playtimes);
    return record;
}

std::string FormatPlaytime(std::uint64_t seconds) {
    const auto minutes = seconds / 60;
    if (minutes == 0) return "Under a minute";
    if (minutes < 60) return std::to_string(minutes) + " min";
    const auto hours = minutes / 60;
    if (hours >= 100) return std::to_string(hours) + " h";
    const auto rest = minutes % 60;
    return std::to_string(hours) + " h" + (rest == 0 ? std::string() : " " + std::to_string(rest) + " min");
}

}
