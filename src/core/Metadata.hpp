#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace launcher {

struct GameMetadata {
    std::string titleId;
    std::string name;
    std::string contentVersion;
    std::string contentId;  // e.g. EP9000-PPSA21567_00-0000000000000000; may be empty
    std::string source;
};

// The store region a content ID belongs to, from its two-letter prefix:
// "Europe" for EP, "USA" for UP, "Japan" for JP, "Asia" for HP, ... Empty when unknown.
std::string RegionOf(std::string_view contentId);

// Decodes &amp; &lt; &gt; &quot; &apos; and numeric references; leaves anything else as written.
std::string DecodeXmlEntities(std::string_view text);

// One entry of the game's own sce_sys/changeinfo/changeinfo.xml.
struct ChangeNote {
    std::string version;  // its app_ver, as written
    std::string text;
};

// Parses changeinfo.xml: <changeinfo><changes app_ver="01.02">text</changes>...</changeinfo>.
// The text may be CDATA; XML entities are decoded and lines trimmed. Newest first.
std::vector<ChangeNote> ParseChangeInfo(std::string_view xml);

// The note for a content version ("01.018.000" matches app_ver "01.018.000" or "01.18"), if any.
const ChangeNote* ChangeNoteFor(const std::vector<ChangeNote>& notes, std::string_view contentVersion);

// Compares dotted version numbers part by part as numbers: -1, 0 or 1.
int CompareVersions(std::string_view a, std::string_view b);

bool IsTitleId(std::string_view text);

GameMetadata ParseParamJson(std::span<const std::uint8_t> bytes, std::string_view preferredLocale = {});

GameMetadata ParseParamSfo(std::span<const std::uint8_t> bytes);

}
