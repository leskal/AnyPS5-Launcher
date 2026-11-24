#pragma once

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace launcher {

// The pieces of an AnyPS5 build or release the launcher drives.
struct AnyPS5Runtime {
    std::filesystem::path root;
    std::string layout;                              // "build" or "release"
    std::filesystem::path relinker;
    std::filesystem::path libsFolder;                // AnyPS5's own libs folder; a game's libs/ links here
    std::vector<std::filesystem::path> libraries;    // everything a game's libs/ must hold
    std::optional<std::filesystem::path> titleCatalog; // tools/titles.json
};

struct AnyPS5Probe {
    std::optional<AnyPS5Runtime> runtime;
    std::string problem;
};

AnyPS5Probe ProbeAnyPS5(const std::string& folder);

// One entry of AnyPS5's tools/titles.json. Only the fields that affect relinking
// and launching are kept.
struct TitleEntry {
    std::vector<std::string> relink;
    std::optional<std::string> fixedBase;
    std::vector<std::string> executables;
    std::map<std::string, std::string> env;
};

std::map<std::string, TitleEntry> LoadTitleCatalog(const std::filesystem::path& file);

std::vector<std::string> ModuleDirectories(const TitleEntry& entry);

// relink_args() of AnyPS5's tools/title_run.py.
std::vector<std::string> RelinkArguments(const TitleEntry& entry, bool windows, bool intelHost);

bool HostIsIntel();

// The CPU's model name as it reports it, e.g. "Intel(R) Core(TM) i7-12700K"; empty if unknown.
std::string HostCpuName();

inline constexpr bool TargetIsWindows =
#ifdef _WIN32
    true;
#else
    false;
#endif

}
