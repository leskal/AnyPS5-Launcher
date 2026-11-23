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

struct SelfConversion {
    bool eboot = false;
    int modules = 0;
    std::vector<std::string> refused;
};

// Converts a game folder's SELF files: eboot.bin into eboot.elf beside it (the
// original stays, the runtime needs it), and every SELF module directly inside
// the module folders in place, keeping the original as <name>.self. A module
// that can't be unwrapped is left as it is and listed in refused.
// The title's extra executables (titles.json) are converted in place the same way.
SelfConversion ConvertGameSelfFiles(const std::filesystem::path& game, const std::vector<std::string>& moduleFolders,
                                    const std::vector<std::string>& executables, JobControl& control);

}
