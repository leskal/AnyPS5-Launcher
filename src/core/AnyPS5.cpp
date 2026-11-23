#include "core/AnyPS5.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <system_error>

#if defined(__x86_64__) || defined(__i386__)
#include <cpuid.h>
#endif

#include <nlohmann/json.hpp>

#include "core/Metadata.hpp"
#include "core/Utf8.hpp"

namespace launcher {

namespace {

#ifdef _WIN32
constexpr const char* RelinkerName = "relinker.exe";
constexpr std::initializer_list<const char*> AuxiliaryRuntimes = {"mimalloc.dll", "mimalloc-redirect.dll"};
constexpr std::initializer_list<const char*> MinGWRuntimes = {"libgcc_s_seh-1.dll", "libstdc++-6.dll", "libwinpthread-1.dll"};
#else
constexpr const char* RelinkerName = "relinker";
constexpr std::initializer_list<const char*> AuxiliaryRuntimes = {"libmimalloc.so"};
constexpr std::initializer_list<const char*> MinGWRuntimes = {};
#endif

bool IsFile(const std::filesystem::path& path) {
    std::error_code error;
    return std::filesystem::is_regular_file(path, error);
}

std::vector<std::filesystem::path> FilesWithExtension(const std::filesystem::path& folder, const char* extension) {
    std::vector<std::filesystem::path> files;
    std::error_code error;
    for (std::filesystem::directory_iterator it(folder, error), end; !error && it != end; it.increment(error)) {
        if (it->is_regular_file(error) && ToUtf8(it->path().extension()) == extension) files.push_back(it->path());
    }
    std::sort(files.begin(), files.end());
    return files;
}

std::string ReadText(const std::filesystem::path& file) {
    std::ifstream stream(file, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

std::optional<std::filesystem::path> SearchPath(const std::string& program) {
    const char* path = std::getenv("PATH");
    if (path == nullptr) return std::nullopt;
#ifdef _WIN32
    constexpr char separator = ';';
#else
    constexpr char separator = ':';
#endif
    std::stringstream entries(path);
    std::string entry;
    while (std::getline(entries, entry, separator)) {
        if (entry.empty()) continue;
        for (const auto& name : {program, program + ".exe"}) {
            const auto candidate = FromUtf8(entry) / FromUtf8(name);
            if (IsFile(candidate)) return candidate;
        }
    }
    return std::nullopt;
}

// windows_runtime() of AnyPS5's tools/package_release.py: the MinGW DLLs come
// from the bin folder of the compiler recorded in the build's CMakeCache.txt.
std::optional<std::filesystem::path> CompilerFolder(const std::filesystem::path& build) {
    std::istringstream cache(ReadText(build / "CMakeCache.txt"));
    std::string line;
    while (std::getline(cache, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.rfind("CMAKE_CXX_COMPILER:", 0) != 0) continue;
        const auto compiler = line.substr(line.find('=') + 1);
        if (compiler.empty() || compiler.ends_with("-NOTFOUND")) return std::nullopt;
        const auto path = FromUtf8(compiler);
        if (!path.has_parent_path()) {
            const auto found = SearchPath(compiler);
            return found ? std::optional(found->parent_path()) : std::nullopt;
        }
        return path.parent_path();
    }
    return std::nullopt;
}

AnyPS5Probe ProbeBuild(const std::filesystem::path& root, const std::filesystem::path& build) {
    AnyPS5Probe probe;
    AnyPS5Runtime runtime;
    runtime.root = root;
    runtime.layout = "build";
    runtime.relinker = build / "core" / "relinker" / RelinkerName;
    const auto libs = build / "core" / "libs" / "libs";
    runtime.libsFolder = libs;
    runtime.libraries = FilesWithExtension(libs, ".prx");
    if (runtime.libraries.empty()) {
        probe.problem = ToUtf8(libs) + " has no .prx libraries: build all of AnyPS5, not only the relinker";
        return probe;
    }
    for (const char* name : AuxiliaryRuntimes) {
        if (IsFile(libs / name)) runtime.libraries.push_back(libs / name);
    }
    if (MinGWRuntimes.size() != 0) {
        const auto compiler = CompilerFolder(build);
        for (const char* name : MinGWRuntimes) {
            if (IsFile(libs / name)) {
                runtime.libraries.push_back(libs / name);
            } else if (compiler && IsFile(*compiler / name)) {
                runtime.libraries.push_back(*compiler / name);
            } else {
                probe.problem = std::string(name) + " not found in " + ToUtf8(libs) + (compiler ? " or " + ToUtf8(*compiler) : " and CMakeCache.txt names no compiler");
                return probe;
            }
        }
    }
    if (IsFile(root / "tools" / "titles.json")) runtime.titleCatalog = root / "tools" / "titles.json";
    probe.runtime = std::move(runtime);
    return probe;
}

AnyPS5Probe ProbeRelease(const std::filesystem::path& root) {
    AnyPS5Probe probe;
    AnyPS5Runtime runtime;
    runtime.root = root;
    runtime.layout = "release";
    std::vector<std::filesystem::path> relinkers;
    std::error_code error;
    for (std::filesystem::directory_iterator it(root, error), end; !error && it != end; it.increment(error)) {
        const auto name = ToUtf8(it->path().filename());
#ifdef _WIN32
        const bool match = name == "relinker.exe" || (name.starts_with("relinker-") && name.ends_with(".exe"));
#else
        const bool match = name == "relinker" || (name.starts_with("relinker-") && !name.ends_with(".zip") && !name.ends_with(".exe") && !name.ends_with(".txt"));
#endif
        if (match && it->is_regular_file(error)) relinkers.push_back(it->path());
    }
    if (relinkers.size() != 1) {
        probe.problem = relinkers.empty() ? "no relinker found in " + ToUtf8(root) : "more than one relinker in " + ToUtf8(root) + "; keep one";
        return probe;
    }
    runtime.relinker = relinkers.front();
    const auto libs = root / "libs";
    runtime.libsFolder = libs;
    runtime.libraries = FilesWithExtension(libs, ".prx");
    if (runtime.libraries.empty()) {
        probe.problem = ToUtf8(libs) + " has no .prx libraries: extract the prx archive of the same release here";
        return probe;
    }
    for (const auto& extra : FilesWithExtension(libs, TargetIsWindows ? ".dll" : ".so")) runtime.libraries.push_back(extra);
    for (const char* name : MinGWRuntimes) {
        if (!IsFile(libs / name)) {
            probe.problem = std::string(name) + " is missing from " + ToUtf8(libs);
            return probe;
        }
    }
    if (IsFile(root / "tools" / "titles.json")) runtime.titleCatalog = root / "tools" / "titles.json";
    probe.runtime = std::move(runtime);
    return probe;
}

std::vector<std::string> StringList(const nlohmann::json& value, const std::string& where) {
    if (!value.is_array()) throw std::runtime_error(where + " must be a list");
    std::vector<std::string> items;
    for (const auto& item : value) {
        if (!item.is_string()) throw std::runtime_error(where + " must hold strings");
        items.push_back(item.get<std::string>());
    }
    return items;
}

}

AnyPS5Probe ProbeAnyPS5(const std::string& folder) {
    if (folder.empty()) return {std::nullopt, "no AnyPS5 version chosen; download or add one with \"AnyPS5 versions\""};
    const auto root = FromUtf8(folder);
    std::error_code error;
    if (!std::filesystem::is_directory(root, error)) return {std::nullopt, folder + " is not a folder"};
    if (IsFile(root / "build" / "core" / "relinker" / RelinkerName)) return ProbeBuild(root, root / "build");
    if (IsFile(root / "core" / "relinker" / RelinkerName)) return ProbeBuild(root, root);
    auto release = ProbeRelease(root);
    if (release.runtime) return release;
    if (IsFile(root / "CMakeLists.txt")) return {std::nullopt, "no build found: build AnyPS5 into " + ToUtf8(root / "build") + ", or choose an extracted release"};
    return release;
}

std::map<std::string, TitleEntry> LoadTitleCatalog(const std::filesystem::path& file) {
    const auto text = ReadText(file);
    const auto document = nlohmann::json::parse(text.starts_with("\xEF\xBB\xBF") ? text.substr(3) : text, nullptr, false);
    const auto name = ToUtf8(file);
    if (document.is_discarded() || !document.is_object()) throw std::runtime_error(name + " is not a JSON object");
    std::map<std::string, TitleEntry> catalog;
    for (const auto& [titleId, value] : document.items()) {
        if (!IsTitleId(titleId)) throw std::runtime_error(name + ": " + titleId + " is not a title ID");
        if (!value.is_object()) throw std::runtime_error(name + ": " + titleId + " must be an object");
        TitleEntry entry;
        const auto where = name + ": " + titleId + ": ";
        if (const auto field = value.find("relink"); field != value.end()) entry.relink = StringList(*field, where + "relink");
        if (const auto field = value.find("executables"); field != value.end()) entry.executables = StringList(*field, where + "executables");
        if (const auto field = value.find("fixed_base"); field != value.end()) {
            if (!field->is_string()) throw std::runtime_error(where + "fixed_base must be a string");
            entry.fixedBase = field->get<std::string>();
        }
        if (const auto field = value.find("env"); field != value.end()) {
            if (!field->is_object()) throw std::runtime_error(where + "env must be an object");
            for (const auto& [key, item] : field->items()) {
                if (!item.is_string()) throw std::runtime_error(where + "env must hold strings");
                entry.env[key] = item.get<std::string>();
            }
        }
        catalog.emplace(titleId, std::move(entry));
    }
    return catalog;
}

std::vector<std::string> ModuleDirectories(const TitleEntry& entry) {
    std::vector<std::string> directories;
    for (std::size_t index = 0; index + 1 < entry.relink.size(); ++index) {
        if (entry.relink[index] == "--module-dir") directories.push_back(entry.relink[index + 1]);
    }
    return directories;
}

std::vector<std::string> RelinkArguments(const TitleEntry& entry, bool windows, bool intelHost) {
    std::vector<std::string> arguments;
    if (windows) arguments.push_back("--windows");
    arguments.insert(arguments.end(), entry.relink.begin(), entry.relink.end());
    const auto has = [&](const char* flag) { return std::find(arguments.begin(), arguments.end(), flag) != arguments.end(); };
    if (intelHost && !has("--to-intel") && !has("--to-rosetta") && !has("--to-avx")) arguments.push_back("--to-intel");
    if (entry.fixedBase) {
        arguments.push_back(windows ? "--windows-fixed-base" : "--linux-fixed-base");
        arguments.push_back(*entry.fixedBase);
    }
    for (const auto& executable : entry.executables) {
        arguments.push_back("--executable");
        arguments.push_back(executable);
    }
    return arguments;
}

bool HostIsIntel() {
#if defined(__x86_64__) || defined(__i386__)
    unsigned int eax = 0, ebx = 0, ecx = 0, edx = 0;
    if (__get_cpuid(0, &eax, &ebx, &ecx, &edx) == 0) return false;
    char vendor[13] = {};
    std::memcpy(vendor, &ebx, 4);
    std::memcpy(vendor + 4, &edx, 4);
    std::memcpy(vendor + 8, &ecx, 4);
    return std::strcmp(vendor, "GenuineIntel") == 0;
#else
    return false;
#endif
}

}
