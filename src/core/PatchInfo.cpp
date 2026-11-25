#include "core/PatchInfo.hpp"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <system_error>

#include <nlohmann/json.hpp>

#include "core/FileOps.hpp"
#include "core/Metadata.hpp"
#include "core/Utf8.hpp"

namespace launcher {

namespace {

constexpr int CacheVersion = 3;  // 1 could hold an empty update list (wrong key); 2 asked PROSPEROPatches for PS4 titles

bool IsSpace(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

// The text of an HTML fragment: tags dropped, entities decoded, runs of space collapsed.
std::string PlainText(std::string_view html) {
    std::string text;
    bool inTag = false;
    for (const char c : html) {
        if (c == '<') inTag = true;
        else if (c == '>') inTag = false;
        else if (!inTag) text += c;
    }
    // HTML names XML doesn't have; titles carry ® and ™ often.
    for (const auto& [name, utf8] : {std::pair<std::string_view, std::string_view>{"&reg;", "\xC2\xAE"}, {"&trade;", "\xE2\x84\xA2"}, {"&copy;", "\xC2\xA9"}, {"&nbsp;", " "}}) {
        for (auto at = text.find(name); at != std::string::npos; at = text.find(name, at + utf8.size())) text.replace(at, name.size(), utf8);
    }
    text = DecodeXmlEntities(text);
    std::string out;
    bool space = false;
    for (const char c : text) {
        if (IsSpace(c)) {
            space = !out.empty();
        } else {
            if (space) out += ' ';
            space = false;
            out += c;
        }
    }
    return out;
}

// Removes every <small>...</small>: the page puts "View" and "Switch" links there.
std::string WithoutSmall(std::string_view html) {
    std::string out;
    std::size_t at = 0;
    for (;;) {
        const auto open = html.find("<small", at);
        if (open == std::string_view::npos) break;
        const auto close = html.find("</small>", open);
        if (close == std::string_view::npos) break;
        out += html.substr(at, open - at);
        at = close + 8;
    }
    out += html.substr(at);
    return out;
}

std::string Attribute(std::string_view tag, std::string_view name) {
    const auto key = std::string(name) + "=\"";
    const auto at = tag.find(key);
    if (at == std::string_view::npos) return {};
    const auto end = tag.find('"', at + key.size());
    if (end == std::string_view::npos) return {};
    return DecodeXmlEntities(tag.substr(at + key.size(), end - at - key.size()));
}

std::string Text(const nlohmann::json& object, const char* key) {
    const auto found = object.find(key);
    return found != object.end() && found->is_string() ? found->get<std::string>() : std::string();
}

bool IsAsciiAlnum(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
}

}

std::string PatchSiteFor(std::string_view titleId) {
    return PlatformOf(titleId) == "PS4" ? Ps4PatchSite : Ps5PatchSite;
}

std::string PatchSiteName(std::string_view titleId) {
    return PlatformOf(titleId) == "PS4" ? "ORBISPatches" : "PROSPEROPatches";
}

std::string PatchPageUrl(std::string_view titleId) {
    return PatchSiteFor(titleId) + "/" + std::string(titleId);
}

TitlePage ParseTitlePage(std::string_view html) {
    TitlePage page;
    if (const auto title = html.find("<title>"); title != std::string_view::npos) {
        const auto end = html.find("</title>", title);
        auto text = PlainText(html.substr(title + 7, end == std::string_view::npos ? 0 : end - title - 7));
        if (const auto colon = text.find(": "); colon != std::string::npos) text = text.substr(colon + 2);
        if (const auto bar = text.rfind(" | "); bar != std::string::npos) text = text.substr(0, bar);  // "... | ORBISPatches.com"
        page.name = text;
    }
    // Each detail is <li><strong class="bd-links-heading ...">Label</strong> value</li>.
    std::size_t at = 0;
    while ((at = html.find("bd-links-heading", at)) != std::string_view::npos) {
        const auto open = html.find('>', at);
        const auto close = html.find("</strong>", at);
        const auto item = html.find("</li>", at);
        if (open == std::string_view::npos || close == std::string_view::npos || item == std::string_view::npos || close > item) {
            ++at;
            continue;
        }
        const auto label = PlainText(WithoutSmall(html.substr(open + 1, close - open - 1)));
        const auto value = PlainText(html.substr(close + 9, item - close - 9));
        if (label == "Content ID") page.contentId = value;
        else if (label == "Publisher") page.publisher = value;
        else if (label == "Region") page.region = value;
        at = item;
    }
    // The update list's element: <div ... id="dynpatch" data-key="...">. The page's
    // "Game Updates" link carries data-id="dynpatch" too, so match id= alone.
    for (auto list = html.find("id=\"dynpatch\""); list != std::string_view::npos && page.key.empty(); list = html.find("id=\"dynpatch\"", list + 1)) {
        if (list > 0 && html[list - 1] != ' ' && html[list - 1] != '\t' && html[list - 1] != '\n') continue;
        const auto start = html.rfind('<', list);
        const auto end = html.find('>', list);
        if (start == std::string_view::npos || end == std::string_view::npos) continue;
        const auto tag = html.substr(start, end - start);
        page.key = Attribute(tag, "data-key");
        // ORBISPatches: data-loadparams="{ 'titleid': 'CUSA01715', 'key': '...' }".
        if (page.key.empty()) {
            const auto params = Attribute(tag, "data-loadparams");
            if (const auto at = params.find("'key'"); at != std::string::npos) {
                const auto open = params.find('\'', params.find(':', at));
                const auto close = open == std::string::npos ? std::string::npos : params.find('\'', open + 1);
                if (close != std::string::npos) page.key = params.substr(open + 1, close - open - 1);
            }
        }
    }
    return page;
}

std::vector<PatchRelease> ParsePatchList(std::string_view json) {
    const auto document = nlohmann::json::parse(json.begin(), json.end(), nullptr, false);
    if (document.is_discarded() || !document.is_object()) throw std::runtime_error("the update list from the patch site is not valid JSON");
    const auto success = document.find("success");
    if (success == document.end() || !success->is_boolean() || !success->get<bool>()) {
        const auto message = Text(document, "message");
        throw std::runtime_error("the patch site refused the update list" + (message.empty() ? std::string() : ": " + message));
    }
    std::vector<PatchRelease> patches;
    const auto list = document.find("patches");
    if (list == document.end() || !list->is_array()) return patches;
    for (const auto& item : *list) {
        if (!item.is_object()) continue;
        PatchRelease patch;
        patch.contentVersion = Text(item, "content_ver");
        if (patch.contentVersion.empty()) patch.contentVersion = Text(item, "version");  // ORBISPatches
        if (patch.contentVersion.empty()) continue;
        patch.size = Text(item, "filesize");
        patch.requiredFirmware = Text(item, "required_firmware");
        patch.importDate = Text(item, "import_date");
        if (patch.importDate.empty()) patch.importDate = Text(item, "creation_date");
        patch.notes = Text(item, "changelog_preview");
        if (const auto length = item.find("changelog_charcount"); length != item.end() && length->is_number_integer()) patch.notesLength = length->get<int>();
        if (const auto latest = item.find("is_latest"); latest != item.end() && latest->is_boolean()) patch.latest = latest->get<bool>();
        patches.push_back(std::move(patch));
    }
    std::stable_sort(patches.begin(), patches.end(), [](const PatchRelease& a, const PatchRelease& b) { return CompareVersions(a.contentVersion, b.contentVersion) > 0; });
    return patches;
}

const PatchRelease* NewestPatch(const TitleOnlineInfo& info) {
    for (const auto& patch : info.patches) {
        if (patch.latest) return &patch;
    }
    return info.patches.empty() ? nullptr : &info.patches.front();
}

TitleOnlineInfo FetchTitleOnline(std::string_view titleId, const TextFetcher& get, const TextPoster& post, std::int64_t now) {
    if (!IsTitleId(titleId)) throw std::runtime_error("\"" + std::string(titleId) + "\" is not a title ID");
    for (const char c : titleId) {
        if (!IsAsciiAlnum(c)) throw std::runtime_error("\"" + std::string(titleId) + "\" is not a title ID");
    }
    TitleOnlineInfo info;
    info.titleId = std::string(titleId);
    info.fetchedAt = now;
    std::string html;
    try {
        html = get(PatchPageUrl(titleId));
    } catch (const std::exception& failure) {
        const std::string what = failure.what();
        // WinHTTP reports "HTTP 404 for ..."; curl exits with status 22.
        if (what.find("HTTP 404") != std::string::npos || what.find("exit status 22") != std::string::npos || what.find("exit status 5632") != std::string::npos) return info;
        throw;
    }
    const auto page = ParseTitlePage(html);
    info.found = true;
    info.name = page.name;
    info.contentId = page.contentId;
    info.publisher = page.publisher;
    info.region = page.region;
    if (!page.key.empty()) {
        const nlohmann::json request = {{"titleid", info.titleId}, {"key", page.key}};
        info.patches = ParsePatchList(post(PatchSiteFor(titleId) + "/api/internal/loadpatches", request.dump(), "application/x-www-form-urlencoded; charset=UTF-8"));
    }
    return info;
}

std::optional<TitleOnlineInfo> LoadCachedTitle(const std::filesystem::path& folder, std::string_view titleId, std::int64_t now, std::int64_t maxAge) {
    std::ifstream stream(folder / FromUtf8(std::string(titleId) + ".json"), std::ios::binary);
    if (!stream) return std::nullopt;
    const std::string text{std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
    const auto document = nlohmann::json::parse(text, nullptr, false);
    if (document.is_discarded() || !document.is_object()) return std::nullopt;
    const auto version = document.find("version");
    if (version == document.end() || !version->is_number_integer() || version->get<int>() != CacheVersion) return std::nullopt;
    TitleOnlineInfo info;
    info.titleId = Text(document, "titleId");
    if (info.titleId != titleId) return std::nullopt;
    const auto fetched = document.find("fetchedAt");
    if (fetched == document.end() || !fetched->is_number_integer()) return std::nullopt;
    info.fetchedAt = fetched->get<std::int64_t>();
    if (now - info.fetchedAt > maxAge || info.fetchedAt > now + 300) return std::nullopt;
    if (const auto found = document.find("found"); found != document.end() && found->is_boolean()) info.found = found->get<bool>();
    info.name = Text(document, "name");
    info.contentId = Text(document, "contentId");
    info.publisher = Text(document, "publisher");
    info.region = Text(document, "region");
    if (const auto list = document.find("patches"); list != document.end() && list->is_array()) {
        for (const auto& item : *list) {
            if (!item.is_object()) continue;
            PatchRelease patch;
            patch.contentVersion = Text(item, "contentVersion");
            patch.size = Text(item, "size");
            patch.requiredFirmware = Text(item, "requiredFirmware");
            patch.importDate = Text(item, "importDate");
            patch.notes = Text(item, "notes");
            if (const auto length = item.find("notesLength"); length != item.end() && length->is_number_integer()) patch.notesLength = length->get<int>();
            if (const auto latest = item.find("latest"); latest != item.end() && latest->is_boolean()) patch.latest = latest->get<bool>();
            if (!patch.contentVersion.empty()) info.patches.push_back(std::move(patch));
        }
    }
    return info;
}

void SaveCachedTitle(const std::filesystem::path& folder, const TitleOnlineInfo& info) {
    nlohmann::json patches = nlohmann::json::array();
    for (const auto& patch : info.patches) {
        patches.push_back({{"contentVersion", patch.contentVersion},
                           {"size", patch.size},
                           {"requiredFirmware", patch.requiredFirmware},
                           {"importDate", patch.importDate},
                           {"notes", patch.notes},
                           {"notesLength", patch.notesLength},
                           {"latest", patch.latest}});
    }
    const nlohmann::json document = {{"version", CacheVersion},   {"titleId", info.titleId}, {"fetchedAt", info.fetchedAt},
                                     {"found", info.found},       {"name", info.name},       {"contentId", info.contentId},
                                     {"publisher", info.publisher}, {"region", info.region}, {"patches", patches}};
    std::error_code error;
    std::filesystem::create_directories(folder, error);
    WriteFileAtomically(folder / FromUtf8(info.titleId + ".json"), document.dump(2) + "\n");
}

std::int64_t UnixNow() {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

}
