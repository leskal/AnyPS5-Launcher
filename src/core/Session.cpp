#include "core/Session.hpp"

#include <fstream>
#include <iterator>
#include <stdexcept>
#include <system_error>

#include <nlohmann/json.hpp>

#include "core/FileOps.hpp"
#include "core/Utf8.hpp"

namespace launcher {

namespace {

bool Exists(const std::filesystem::path& path) {
    std::error_code error;
    return std::filesystem::exists(std::filesystem::symlink_status(path, error));
}

// Removes an empty folder; refuses anything else.
void ClearPlace(const std::filesystem::path& path, const std::string& why) {
    if (!Exists(path)) return;
    std::error_code error;
    if (std::filesystem::is_directory(std::filesystem::symlink_status(path, error)) && std::filesystem::is_empty(path, error) && !error) {
        std::filesystem::remove(path, error);
        if (!error) return;
    }
    throw std::runtime_error(ToUtf8(path) + " already holds files " + why + "; move them away first");
}

std::vector<SessionItem> ReadMarker(const std::filesystem::path& home) {
    std::ifstream stream(home / SessionMarker, std::ios::binary);
    const std::string text{std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
    const auto document = nlohmann::json::parse(text, nullptr, false);
    if (document.is_discarded() || !document.is_object() || !document.contains("items") || !document["items"].is_array()) {
        throw std::runtime_error(ToUtf8(home / SessionMarker) + " can't be read; move the save folders beside the exe back by hand");
    }
    std::vector<SessionItem> items;
    for (const auto& item : document["items"]) {
        if (!item.is_object() || !item.contains("name") || !item.contains("origin") || !item["name"].is_string() || !item["origin"].is_string()) continue;
        const auto name = item["name"].get<std::string>();
        if (name.empty() || name.find_first_of("/\\:") != std::string::npos || name == "." || name == "..") continue;
        items.push_back({name, FromUtf8(item["origin"].get<std::string>())});
    }
    return items;
}

void WriteMarker(const std::filesystem::path& home, const std::vector<SessionItem>& items) {
    nlohmann::json list = nlohmann::json::array();
    for (const auto& item : items) list.push_back({{"name", item.name}, {"origin", ToUtf8(item.origin)}});
    WriteFileAtomically(home / SessionMarker, nlohmann::json{{"version", 1}, {"items", list}}.dump(4) + "\n");
}

void MoveBack(const std::filesystem::path& home, const SessionItem& item) {
    const auto here = home / FromUtf8(item.name);
    std::error_code error;
    std::filesystem::create_directories(item.origin.parent_path(), error);
    if (!Exists(here)) {
        std::filesystem::create_directories(item.origin, error);
        return;
    }
    ClearPlace(item.origin, "while the game's copy is still beside the exe");
    MoveTree(here, item.origin);
}

}

bool HasSession(const std::filesystem::path& home) {
    return Exists(home / SessionMarker);
}

void MoveInSession(const std::filesystem::path& home, const std::vector<SessionItem>& items) {
    if (HasSession(home)) throw std::runtime_error("the saves of an earlier run are still beside the exe; restart the launcher to move them back");
    for (const auto& item : items) ClearPlace(home / FromUtf8(item.name), "that don't belong to a profile");
    std::error_code error;
    for (const auto& item : items) {
        std::filesystem::create_directories(item.origin, error);
        if (error) throw std::system_error(error, "cannot create " + ToUtf8(item.origin));
    }
    WriteMarker(home, items);
    std::vector<SessionItem> moved;
    try {
        for (const auto& item : items) {
            MoveTree(item.origin, home / FromUtf8(item.name));
            moved.push_back(item);
        }
    } catch (...) {
        for (const auto& item : moved) {
            try {
                MoveBack(home, item);
            } catch (...) {
                WriteMarker(home, moved);
                throw;
            }
        }
        std::filesystem::remove(home / SessionMarker, error);
        throw;
    }
}

std::vector<std::string> MoveOutSession(const std::filesystem::path& home) {
    if (!HasSession(home)) return {};
    auto items = ReadMarker(home);
    std::vector<std::string> names;
    std::vector<SessionItem> left;
    std::string failure;
    for (const auto& item : items) {
        try {
            MoveBack(home, item);
            names.push_back(item.name);
        } catch (const std::exception& error) {
            left.push_back(item);
            if (failure.empty()) failure = error.what();
        }
    }
    if (!left.empty()) {
        WriteMarker(home, left);
        throw std::runtime_error(failure);
    }
    std::error_code error;
    std::filesystem::remove(home / SessionMarker, error);
    return names;
}

}
