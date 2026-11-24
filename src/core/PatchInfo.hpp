#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/Http.hpp"

namespace launcher {

// What PROSPEROPatches.com knows about a title: the details on its page and
// the list of updates Sony has published. Changelogs are not included: the
// site puts them behind a CAPTCHA, so the launcher links to the page instead.
inline constexpr const char* PatchSite = "https://prosperopatches.com";

struct PatchRelease {
    std::string contentVersion;  // "01.015.000"
    std::string size;            // as the site writes it: "250.1 MB"
    std::string requiredFirmware;
    std::string importDate;      // "2025-06-19 11:55:04"
    bool latest = false;
};

struct TitleOnlineInfo {
    std::string titleId;
    bool found = false;  // false when the site has no page for the title
    std::string name;
    std::string contentId;
    std::string publisher;
    std::string region;  // as the site writes it: "EU", "US", ...
    std::vector<PatchRelease> patches;  // newest first
    std::int64_t fetchedAt = 0;         // seconds since 1970, UTC
};

std::string PatchPageUrl(std::string_view titleId);

struct TitlePage {
    std::string name;
    std::string contentId;
    std::string publisher;
    std::string region;
    std::string key;  // the page's key for its update list
};

TitlePage ParseTitlePage(std::string_view html);

// The answer of the update list request. Throws when it reports a failure.
std::vector<PatchRelease> ParsePatchList(std::string_view json);

// The newest update, or nullptr when there is none.
const PatchRelease* NewestPatch(const TitleOnlineInfo& info);

// Loads the title page, then its update list. Throws on network errors; a
// title the site doesn't know comes back with found == false.
TitleOnlineInfo FetchTitleOnline(std::string_view titleId, const TextFetcher& get, const TextPoster& post, std::int64_t now);

// One JSON file per title in folder. A cached answer older than maxAge seconds is ignored.
std::optional<TitleOnlineInfo> LoadCachedTitle(const std::filesystem::path& folder, std::string_view titleId, std::int64_t now, std::int64_t maxAge);
void SaveCachedTitle(const std::filesystem::path& folder, const TitleOnlineInfo& info);

std::int64_t UnixNow();

}
