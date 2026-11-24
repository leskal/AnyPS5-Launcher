#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "core/Job.hpp"
#include "core/Scanner.hpp"

namespace launcher {

inline constexpr const char* UnwrappedEbootName = "eboot.elf";

std::filesystem::path UnwrappedEbootFor(const GameEntry& game);

void UnwrapSelf(const std::filesystem::path& input, const std::filesystem::path& output);

// Convert SELF to ELF keeps every original it replaces in one zip, named from
// param.json (PPSA21567-01.018.000.originals.zip), with an MD5SUMS list (md5sum
// format), in <library>\originals\.
inline constexpr const char* OriginalsChecksums = "MD5SUMS";

// The zip's name, from the game's title ID and version.
std::string OriginalsZipName(const GameEntry& game);
// <library>\originals\<name>.
std::filesystem::path OriginalsZipFor(const GameEntry& game, const std::filesystem::path& library);

struct SelfConversion {
    bool eboot = false;
    int modules = 0;                   // modules and extra executables converted now
    int migrated = 0;                  // older <name>.self backups moved into the zip
    std::vector<std::string> refused;  // left as they are, with the reason
};

// Converts, in place, the SELF files of a game folder that the relinker reads:
// eboot.bin, every file directly inside the module folders, and the title's
// extra executables. Order, so an original is never lost:
//   1. each SELF is unwrapped to <name>.elf.tmp; one that can't be (encrypted)
//      is left alone and listed;
//   2. the originals are added to the zip (written as .partial, then each entry's
//      MD5 is checked against the original before the zip replaces the old one);
//   3. only then each original is replaced by its ELF.
// <name>.self files from the launcher's earlier converter are moved into the zip
// the same way, and an eboot.elf beside a converted eboot.bin is deleted.
SelfConversion ConvertGameSelfFiles(const std::filesystem::path& game, const std::filesystem::path& zip, const std::vector<std::string>& moduleFolders,
                                    const std::vector<std::string>& executables, JobControl& control);

// Puts every original in the zip back (each checked against MD5SUMS first),
// then deletes the zip. Returns how many files were restored.
int RestoreOriginals(const std::filesystem::path& game, const std::filesystem::path& zip, JobControl& control);

}
