#include "core/Install.hpp"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <fstream>
#include <iterator>
#include <memory>
#include <set>
#include <span>
#include <sstream>
#include <stdexcept>
#include <system_error>

#include <nlohmann/json.hpp>

#include "core/Encryption.hpp"
#include "core/FileOps.hpp"
#include "core/Links.hpp"
#include "core/Modules.hpp"
#include "core/Package.hpp"
#include "core/SelfUnwrap.hpp"
#include "core/Session.hpp"
#include "core/Utf8.hpp"

namespace launcher {

namespace {

constexpr std::uint64_t ProgressTotal = 1000;
constexpr std::uint64_t MaxExtractedFile = std::uint64_t{4} << 30;
constexpr const char* App0 = "app0";
constexpr const char* App0Zar = "app0.zar";

std::string Now() {
    const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    char text[32];
    std::strftime(text, sizeof(text), "%Y-%m-%d %H:%M:%S", &local);
    return text;
}

std::string ReadText(const std::filesystem::path& file) {
    std::ifstream stream(file, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

ModuleKind KindOf(const std::filesystem::path& file) {
    std::ifstream stream(file, std::ios::binary);
    std::uint8_t head[4]{};
    stream.read(reinterpret_cast<char*>(head), 4);
    return ModuleKindOf(std::span<const std::uint8_t>(head, static_cast<std::size_t>(stream.gcount())));
}

// The reason a file on disk is an ELF that is still encrypted, if it is.
std::optional<std::string> EncryptedOnDisk(const std::filesystem::path& file) {
    std::error_code error;
    const auto size = std::filesystem::file_size(file, error);
    if (error) return std::nullopt;
    std::ifstream stream(file, std::ios::binary);
    return ElfEncryption(size, [&](std::uint64_t offset, std::uint64_t length) {
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(length));
        stream.clear();
        stream.seekg(static_cast<std::streamoff>(offset));
        stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(length));
        bytes.resize(static_cast<std::size_t>(stream.gcount()));
        return bytes;
    });
}

std::string EncryptedMessage(const std::vector<std::string>& files) {
    std::string list;
    for (std::size_t index = 0; index < std::min<std::size_t>(files.size(), 3); ++index) list += (index ? "; " : "") + files[index];
    if (files.size() > 3) list += "; and " + std::to_string(files.size() - 3) + " more";
    return "still encrypted: " + list + ". The launcher can't decrypt; use a decrypted dump of the game";
}

bool Exists(const std::filesystem::path& path) {
    std::error_code error;
    return std::filesystem::exists(std::filesystem::symlink_status(path, error));
}

std::vector<std::string> TopLevelNames(const std::filesystem::path& folder) {
    std::vector<std::string> names;
    std::error_code error;
    for (std::filesystem::directory_iterator it(folder, error), end; !error && it != end; it.increment(error)) names.push_back(ToUtf8(it->path().filename()));
    if (error) throw std::system_error(error, "cannot read " + ToUtf8(folder));
    std::sort(names.begin(), names.end());
    return names;
}

void WriteBytes(const std::filesystem::path& file, std::span<const std::uint8_t> bytes) {
    std::error_code error;
    std::filesystem::create_directories(file.parent_path(), error);
    std::ofstream stream(file, std::ios::binary | std::ios::trunc);
    stream.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    stream.close();
    if (!stream) throw std::runtime_error("cannot write " + ToUtf8(file));
}

// A path from inside an archive, refused if it could leave the target folder.
std::filesystem::path SafeRelative(const std::string& relative) {
    if (relative.empty() || relative.front() == '/' || relative.find('\\') != std::string::npos || relative.find(':') != std::string::npos) {
        throw std::runtime_error("unsafe path in the archive: " + relative);
    }
    std::filesystem::path result;
    std::size_t begin = 0;
    while (begin <= relative.size()) {
        const auto end = std::min(relative.find('/', begin), relative.size());
        const auto part = relative.substr(begin, end - begin);
        if (part.empty() || part == "." || part == "..") throw std::runtime_error("unsafe path in the archive: " + relative);
        result /= FromUtf8(part);
        begin = end + 1;
    }
    return result;
}

// Writes a file from the archive, unwrapping it when it's a SELF. Returns true when unwrapped.
bool ExtractExecutable(PackageReader& package, const std::string& relative, const std::filesystem::path& target) {
    auto bytes = package.ReadAll(relative, MaxExtractedFile);
    if (!bytes) throw std::runtime_error(relative + " is missing from the archive");
    const auto kind = ModuleKindOf(std::span<const std::uint8_t>(*bytes).first(std::min<std::size_t>(bytes->size(), 4)));
    if (kind != ModuleKind::Self) {
        if (kind == ModuleKind::Elf) {
            if (auto reason = ElfEncryption(*bytes)) throw std::runtime_error(EncryptedMessage({relative + " (" + *reason + ")"}));
        }
        WriteBytes(target, *bytes);
        return false;
    }
    try {
        WriteBytes(target, UnwrapSelfImage(*bytes));
    } catch (const UnwrapRefused& refused) {
        throw std::runtime_error(relative + " can't be unwrapped from SELF: " + refused.what());
    }
    return true;
}

struct Extracted {
    int modules = 0;
    int unwrapped = 0;
};

// Only what the relinker and AnyPS5's start-up read from disk; the game reads
// everything else from app0.zar.
Extracted ExtractEssentials(PackageReader& package, const std::filesystem::path& app0, const std::vector<std::string>& moduleFolders,
                            const std::vector<std::string>& executables, JobControl& control) {
    Extracted result;
    if (!package.FileSize("eboot.bin")) throw std::runtime_error("eboot.bin is missing from the archive");
    if (ExtractExecutable(package, "eboot.bin", app0 / "eboot.elf")) ++result.unwrapped;
    for (const auto& file : package.ListTree("sce_sys")) {
        ThrowIfCancelled(control);
        auto bytes = package.ReadAll(file, MaxExtractedFile);
        if (bytes) WriteBytes(app0 / SafeRelative(file), *bytes);
    }
    const std::set<std::string> extra(executables.begin(), executables.end());
    for (const auto& folder : moduleFolders) {
        if (!package.IsDirectory(folder)) continue;
        std::error_code error;
        std::filesystem::create_directories(app0 / SafeRelative(folder), error);
        for (const auto& name : package.ListFiles(folder)) {
            ThrowIfCancelled(control);
            const auto relative = folder + "/" + name;
            if (extra.count(relative) || IsSelfBackup(name) || IsGuestModule(name)) continue;
            const auto head = package.ReadHead(relative, 4);
            if (!head || ModuleKindOf(*head) == ModuleKind::NotModule) continue;
            ++result.modules;
            if (ExtractExecutable(package, relative, app0 / SafeRelative(relative))) ++result.unwrapped;
        }
    }
    for (const auto& executable : executables) {
        ThrowIfCancelled(control);
        if (ExtractExecutable(package, executable, app0 / SafeRelative(executable))) ++result.unwrapped;
    }
    return result;
}

// The relinker reads a folder game as it is, so everything it converts must already be ELF.
int RequireElf(const std::filesystem::path& folder, const std::vector<std::string>& moduleFolders, const std::vector<std::string>& executables) {
    std::vector<std::string> self;
    std::vector<std::string> encrypted;
    const auto checkEncrypted = [&](const std::filesystem::path& file, const std::string& name) {
        if (auto reason = EncryptedOnDisk(file)) encrypted.push_back(name + " (" + *reason + ")");
    };
    if (KindOf(folder / "eboot.elf") == ModuleKind::Elf) checkEncrypted(folder / "eboot.elf", "eboot.elf");
    else if (KindOf(folder / "eboot.bin") == ModuleKind::Elf) checkEncrypted(folder / "eboot.bin", "eboot.bin");
    else self.push_back("eboot.bin");
    int modules = 0;
    std::error_code error;
    for (const auto& directory : moduleFolders) {
        const auto path = folder / FromUtf8(directory);
        if (!std::filesystem::is_directory(path, error)) continue;
        for (std::filesystem::directory_iterator it(path, error), end; !error && it != end; it.increment(error)) {
            const auto name = ToUtf8(it->path().filename());
            if (it->is_symlink(error) || !it->is_regular_file(error) || IsSelfBackup(name) || IsGuestModule(name)) continue;
            const auto kind = KindOf(it->path());
            if (kind == ModuleKind::Elf) {
                ++modules;
                checkEncrypted(it->path(), directory + "/" + name);
            } else if (kind == ModuleKind::Self) {
                self.push_back(directory + "/" + name);
            }
        }
    }
    for (const auto& executable : executables) {
        if (KindOf(folder / FromUtf8(executable)) != ModuleKind::Elf) self.push_back(executable);
        else checkEncrypted(folder / FromUtf8(executable), executable);
    }
    if (!encrypted.empty()) throw std::runtime_error(EncryptedMessage(encrypted));
    if (!self.empty()) {
        std::string list;
        for (std::size_t index = 0; index < std::min<std::size_t>(self.size(), 3); ++index) list += (index ? ", " : "") + self[index];
        if (self.size() > 3) list += " and " + std::to_string(self.size() - 3) + " more";
        throw std::runtime_error("not ELF yet: " + list + ". Right-click the game and choose Convert SELF to ELF first");
    }
    return modules;
}

// Every library AnyPS5 needs must be in its libs folder, because the exe loads
// them from there (--rpath). With an AnyPS5 build the MinGW DLLs sit beside the
// compiler, so they are copied into the build's libs folder once.
std::vector<std::string> CompleteLibsFolder(const AnyPS5Runtime& anyps5) {
    std::vector<std::string> added;
    std::error_code error;
    for (const auto& library : anyps5.libraries) {
        if (library.parent_path() == anyps5.libsFolder) continue;
        const auto target = anyps5.libsFolder / library.filename();
        const auto size = std::filesystem::file_size(library, error);
        if (!error && std::filesystem::is_regular_file(target) && std::filesystem::file_size(target, error) == size && !error) continue;
        error.clear();
        std::filesystem::copy_file(library, target, std::filesystem::copy_options::overwrite_existing, error);
        if (error) throw std::system_error(error, "cannot copy " + ToUtf8(library) + " into " + ToUtf8(anyps5.libsFolder));
        added.push_back(ToUtf8(library.filename()));
    }
    return added;
}

std::string RunPath(const AnyPS5Runtime& anyps5) {
    const auto text = ToUtf8(std::filesystem::absolute(anyps5.libsFolder).lexically_normal().make_preferred());
    if (std::any_of(text.begin(), text.end(), [](unsigned char c) { return c >= 128; })) {
        throw std::runtime_error("the AnyPS5 folder's path has non-ASCII characters (" + text +
                                 "), and the relinker's --rpath takes only ASCII on Windows. Move AnyPS5 to a plain path such as C:\\dev\\AnyPS5");
    }
    return text;
}

void WriteManifest(const std::filesystem::path& file, const InstallManifest& manifest) {
    const nlohmann::json document = {
        {"version", 2},
        {"titleId", manifest.titleId},
        {"name", manifest.name},
        {"contentVersion", manifest.contentVersion},
        {"format", manifest.format},
        {"originalName", manifest.originalName},
        {"keptApp0", manifest.keptApp0},
        {"complete", manifest.complete},
        {"installedAt", manifest.installedAt},
        {"anyps5Root", manifest.anyps5Root},
        {"anyps5Layout", manifest.anyps5Layout},
        {"relinkerVersion", manifest.relinkerVersion},
        {"arguments", manifest.arguments},
        {"outputs", manifest.outputs},
        {"modules", manifest.modules},
        {"unwrapped", manifest.unwrapped},
    };
    WriteFileAtomically(file, document.dump(4) + "\n");
}

std::vector<std::string> Strings(const nlohmann::json& document, const char* key) {
    std::vector<std::string> values;
    if (const auto found = document.find(key); found != document.end() && found->is_array()) {
        for (const auto& item : *found) {
            if (item.is_string()) values.push_back(item.get<std::string>());
        }
    }
    return values;
}

std::vector<std::string> ModuleDirArguments(const std::vector<std::string>& arguments) {
    std::vector<std::string> folders;
    for (std::size_t index = 0; index + 1 < arguments.size(); ++index) {
        if (arguments[index] == "--module-dir") folders.push_back(arguments[index + 1]);
    }
    return folders;
}

// Deletes the run files left in a folder.
void DropRunFiles(const std::filesystem::path& folder) {
    std::error_code error;
    for (const char* name : {RunReportName, RunRecipeName}) std::filesystem::remove(folder / name, error);
}

void RemoveFile(const std::filesystem::path& file) {
    std::error_code error;
    std::filesystem::remove(file, error);
    if (error) throw std::system_error(error, "cannot delete " + ToUtf8(file));
}

}

bool IsRunFile(const std::string& name) {
    return name == RunReportName || name == RunRecipeName;
}

std::filesystem::path ExecutableIn(const std::filesystem::path& home, std::string_view titleId) {
    return home / (std::string(titleId) + (TargetIsWindows ? ".exe" : ""));
}

std::filesystem::path HomeFor(const GameEntry& game) {
    if (!game.home.empty()) return game.home;
    if (game.format == PackageFormat::Folder) return game.app0Parent.empty() ? game.location : game.app0Parent;
    return game.location.parent_path() / game.location.stem();
}

std::optional<InstallManifest> ReadHomeManifest(const std::filesystem::path& home) {
    const auto document = nlohmann::json::parse(ReadText(home / InstallManifestName), nullptr, false);
    if (document.is_discarded() || !document.is_object()) return std::nullopt;
    InstallManifest manifest;
    const auto text = [&](const char* key) {
        const auto found = document.find(key);
        return found != document.end() && found->is_string() ? found->get<std::string>() : std::string{};
    };
    manifest.titleId = text("titleId");
    manifest.name = text("name");
    manifest.contentVersion = text("contentVersion");
    manifest.format = text("format");
    manifest.originalName = text("originalName");
    manifest.installedAt = text("installedAt");
    manifest.anyps5Root = text("anyps5Root");
    manifest.anyps5Layout = text("anyps5Layout");
    manifest.relinkerVersion = text("relinkerVersion");
    manifest.arguments = Strings(document, "arguments");
    manifest.outputs = Strings(document, "outputs");
    const auto number = [&](const char* key) {
        const auto found = document.find(key);
        return found != document.end() && found->is_number_integer() ? found->get<int>() : 0;
    };
    manifest.modules = number("modules");
    manifest.unwrapped = number("unwrapped");
    const auto kept = document.find("keptApp0");
    manifest.keptApp0 = kept != document.end() && kept->is_boolean() && kept->get<bool>();
    const auto complete = document.find("complete");
    manifest.complete = complete != document.end() && complete->is_boolean() && complete->get<bool>();
    return manifest;
}

std::optional<InstallManifest> ReadInstallManifest(const std::filesystem::path& home) {
    auto manifest = ReadHomeManifest(home);
    if (!manifest || !manifest->complete || manifest->titleId.empty()) return std::nullopt;
    std::error_code error;
    if (!std::filesystem::is_regular_file(ExecutableIn(home, manifest->titleId), error)) return std::nullopt;
    return manifest;
}

std::string RelinkerFailure(const std::filesystem::path& log) {
    std::istringstream lines(ReadText(log));
    std::string line;
    std::string last;
    while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.rfind("FAIL: ", 0) == 0) return line.substr(6);
        if (!line.empty()) last = line;
    }
    if (last.size() > 400) last = last.substr(0, 400) + "...";
    return last;
}

std::string QueryRelinkerVersion(const AnyPS5Runtime& anyps5, const ProcessRunner& run, const std::filesystem::path& scratchFile) {
    JobControl control;
    std::error_code error;
    std::filesystem::create_directories(scratchFile.parent_path(), error);
    const int code = run({ToUtf8(anyps5.relinker), "--version"}, anyps5.relinker.parent_path(), scratchFile, control);
    auto text = ReadText(scratchFile);
    std::filesystem::remove(scratchFile, error);
    if (code != 0) throw std::runtime_error("relinker --version exited with code " + std::to_string(code));
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ')) text.pop_back();
    if (const auto newline = text.find('\n'); newline != std::string::npos) text = text.substr(0, newline);
    if (text.rfind("relinker ", 0) == 0) text = text.substr(9);
    return text;
}

std::string ReinstallReason(const GameEntry& game, const InstallManifest& manifest, const std::string& relinkerVersion) {
    if (manifest.contentVersion != game.metadata.contentVersion) return "installed version " + manifest.contentVersion;
    if (!relinkerVersion.empty() && manifest.relinkerVersion != relinkerVersion) return "converted with AnyPS5 " + manifest.relinkerVersion;
    const auto converted = ModuleDirArguments(manifest.arguments);
    for (const auto& folder : game.moduleDirs) {
        if (std::find(converted.begin(), converted.end(), folder) == converted.end()) return "the modules in " + folder + " weren't converted";
    }
    return {};
}

InstallManifest InstallGame(const InstallRequest& request, const ProcessRunner& run, JobControl& control) {
    const auto& game = request.game;
    const auto& serial = game.metadata.titleId;
    control.SetTotal(ProgressTotal);
    if (game.format == PackageFormat::Zar && game.layout == ZarLayout::Nested) {
        throw std::runtime_error("this .zar keeps the game inside a folder, so it can't be used as app0.zar; convert it to a folder first");
    }
    const auto home = HomeFor(game);
    const auto app0 = home / App0;
    if (!game.home.empty() && HasSession(home)) throw std::runtime_error("the game's folder still holds a profile's saves from the last run; restart the launcher to move them back");
    const auto titleModuleFolders = request.title != nullptr ? ModuleDirectories(*request.title) : std::vector<std::string>{};
    const auto executables = request.title != nullptr ? request.title->executables : std::vector<std::string>{};
    // The title's own --module-dir folders, and every folder the game keeps modules in
    // (looked for here, as a full look is too slow for every scan).
    auto searched = game.moduleDirs;
    try {
        if (auto package = OpenGamePackage(game)) {
            for (auto& folder : FindModuleDirectories(*package)) searched.push_back(std::move(folder));
        }
    } catch (const std::exception&) {
    }
    std::sort(searched.begin(), searched.end());
    searched.erase(std::unique(searched.begin(), searched.end()), searched.end());
    std::vector<std::string> foundModuleFolders;
    for (const auto& folder : searched) {
        if (std::find(titleModuleFolders.begin(), titleModuleFolders.end(), folder) == titleModuleFolders.end()) foundModuleFolders.push_back(folder);
    }
    auto extraModuleFolders = titleModuleFolders;
    extraModuleFolders.insert(extraModuleFolders.end(), foundModuleFolders.begin(), foundModuleFolders.end());
    const auto moduleFolders = ModuleFolders(extraModuleFolders);
    const auto rpath = RunPath(request.anyps5);

    InstallManifest manifest;
    if (!game.home.empty()) {
        if (auto previous = ReadHomeManifest(home)) manifest = std::move(*previous);
    }
    manifest.titleId = serial;
    manifest.name = game.metadata.name;
    manifest.contentVersion = game.metadata.contentVersion;
    manifest.format = std::string(ToString(game.format));
    manifest.anyps5Root = ToUtf8(request.anyps5.root);
    manifest.anyps5Layout = request.anyps5.layout;
    manifest.relinkerVersion = request.relinkerVersion;
    manifest.complete = false;
    manifest.installedAt.clear();
    std::error_code error;

    // 1. Set up the folder.
    if (game.format == PackageFormat::Folder) {
        manifest.modules = RequireElf(game.location, moduleFolders, executables);
        if (game.home.empty() && !game.app0Parent.empty()) {
            // Already <folder>\app0 with nothing else beside it: only the manifest is added.
            manifest.keptApp0 = true;
            manifest.originalName = ToUtf8(home.filename());
            WriteManifest(home / InstallManifestName, manifest);
        } else if (game.home.empty()) {
            DropRunFiles(home);  // from a run before an earlier removal; not the game's
            const auto names = TopLevelNames(home);
            if (std::find(names.begin(), names.end(), App0) != names.end()) throw std::runtime_error("the game folder already has an app0 entry of its own");
            std::filesystem::create_directory(app0, error);
            if (error) throw std::system_error(error, "cannot create " + ToUtf8(app0));
            std::vector<std::string> moved;
            try {
                for (const auto& name : names) {
                    RenameNew(home / FromUtf8(name), app0 / FromUtf8(name));
                    moved.push_back(name);
                }
            } catch (...) {
                for (const auto& name : moved) {
                    try {
                        RenameNew(app0 / FromUtf8(name), home / FromUtf8(name));
                    } catch (...) {
                    }
                }
                std::filesystem::remove(app0, error);
                throw;
            }
            manifest.originalName = ToUtf8(home.filename());
            WriteManifest(home / InstallManifestName, manifest);
        }
    } else {
        auto package = std::make_unique<ZarPackage>(ZarArchive::Open(game.location), "");
        const bool fresh = game.home.empty();
        if (fresh) {
            if (Exists(home)) throw std::runtime_error(ToUtf8(home) + " already exists; move it away or rename the .zar");
            std::filesystem::create_directory(home, error);
            if (error) throw std::system_error(error, "cannot create " + ToUtf8(home));
        } else {
            RemoveOwnTree(app0);
        }
        try {
            std::filesystem::create_directory(app0, error);
            const auto extracted = ExtractEssentials(*package, app0, moduleFolders, executables, control);
            manifest.modules = extracted.modules;
            manifest.unwrapped = extracted.unwrapped;
            package.reset();
            if (fresh) {
                manifest.originalName = ToUtf8(game.location.filename());
                MoveTree(game.location, home / App0Zar);
            }
        } catch (...) {
            package.reset();
            if (fresh) {
                try {
                    RemoveOwnTree(home);
                } catch (...) {
                }
            }
            throw;
        }
        WriteManifest(home / InstallManifestName, manifest);
    }
    control.Advance(200);

    // 2. Relink.
    manifest.addedLibraries = CompleteLibsFolder(request.anyps5);
    for (const auto& output : manifest.outputs) std::filesystem::remove(home / FromUtf8(output), error);
    manifest.outputs.clear();
    const auto executable = ExecutableIn(home, serial);
    std::filesystem::remove(executable, error);
    const auto before = TopLevelNames(home);

    manifest.arguments = RelinkArguments(request.title != nullptr ? *request.title : TitleEntry{}, request.windows, request.intelHost);
    for (const auto& folder : foundModuleFolders) {
        manifest.arguments.push_back("--module-dir");
        manifest.arguments.push_back(folder);
    }
    manifest.arguments.push_back("--rpath");
    manifest.arguments.push_back(rpath);
    const bool anyModuleFolder = std::any_of(moduleFolders.begin(), moduleFolders.end(), [&](const std::string& folder) { return std::filesystem::is_directory(app0 / FromUtf8(folder), error); });
    if (!anyModuleFolder && executables.empty()) manifest.arguments.push_back("--skip-sce-module");
    const auto input = KindOf(app0 / "eboot.elf") == ModuleKind::Elf ? app0 / "eboot.elf" : app0 / "eboot.bin";
    std::vector<std::string> command{ToUtf8(request.anyps5.relinker)};
    command.insert(command.end(), manifest.arguments.begin(), manifest.arguments.end());
    command.push_back(ToUtf8(input));
    command.push_back(ToUtf8(executable));
    const auto logs = request.layout.Logs(serial);
    std::filesystem::create_directories(logs, error);
    const auto log = logs / "install.log";
    const int code = run(command, home, log, control);

    for (const auto& name : TopLevelNames(home)) {
        if (!std::binary_search(before.begin(), before.end(), name)) manifest.outputs.push_back(name);
    }
    if (code != 0 || !std::filesystem::is_regular_file(executable, error)) {
        for (const auto& output : manifest.outputs) std::filesystem::remove(home / FromUtf8(output), error);
        manifest.outputs.clear();
        WriteManifest(home / InstallManifestName, manifest);
        if (code != 0) throw std::runtime_error("the relinker failed (exit code " + std::to_string(code) + "): " + RelinkerFailure(log));
        throw std::runtime_error("the relinker reported success but wrote no " + ToUtf8(executable.filename()));
    }
    manifest.complete = true;
    manifest.installedAt = Now();
    WriteManifest(home / InstallManifestName, manifest);
    control.Advance(ProgressTotal - control.Done());
    return manifest;
}

RemovedInstall RemoveInstall(const GameEntry& game) {
    if (game.home.empty()) throw std::runtime_error("the game isn't set up for AnyPS5");
    const auto home = game.home;
    const auto app0 = home / App0;
    if (HasSession(home)) throw std::runtime_error("the game's folder still holds a profile's saves from the last run; restart the launcher to move them back");
    const auto manifest = ReadHomeManifest(home);
    if (!manifest) throw std::runtime_error(ToUtf8(home / InstallManifestName) + " can't be read");

    std::set<std::string> ours{InstallManifestName, App0, App0Zar, RunReportName, RunRecipeName};
    for (const auto& output : manifest->outputs) ours.insert(output);
    if (!manifest->titleId.empty()) ours.insert(ToUtf8(ExecutableIn(home, manifest->titleId).filename()));
    RemovedInstall result;
    for (const auto& name : TopLevelNames(home)) {
        if (!ours.count(name)) result.leftovers.push_back(name);
    }

    if (game.format == PackageFormat::Zar) {
        const auto original = home.parent_path() / FromUtf8(manifest->originalName.empty() ? ToUtf8(home.filename()) + ".zar" : manifest->originalName);
        if (Exists(original)) throw std::runtime_error(ToUtf8(original) + " already exists; move it away first");
        MoveTree(home / App0Zar, original);
        result.restored = original;
        RemoveOwnTree(app0);
    } else {
        std::vector<std::string> moduleFolders = ModuleFolders(ModuleDirArguments(manifest->arguments));
        for (const auto& folder : moduleFolders) {
            const auto directory = app0 / FromUtf8(folder);
            std::error_code error;
            if (!std::filesystem::is_directory(directory, error)) continue;
            for (const auto& name : TopLevelNames(directory)) {
                if (IsGuestModule(name)) RemoveFile(directory / FromUtf8(name));
            }
        }
        if (manifest->keptApp0) {
            // It was already <folder>\app0: the files stay where they were.
            result.restored = app0;
        } else {
            DropRunFiles(app0);  // left there by an older set-up that moved them in with the game
            DropRunFiles(home);
            const auto names = TopLevelNames(app0);
            for (const auto& name : names) {
                if (Exists(home / FromUtf8(name)) && name != App0) throw std::runtime_error(ToUtf8(home / FromUtf8(name)) + " is in the way of the game's own " + name);
            }
            // The game's own files are moved out first; app0 itself last, in case the game has an app0 entry.
            for (const auto& name : names) {
                if (name != App0) RenameNew(app0 / FromUtf8(name), home / FromUtf8(name));
            }
            result.restored = home;
        }
    }
    for (const auto& name : ours) {
        if (name == App0 || name == App0Zar) continue;
        std::error_code error;
        if (std::filesystem::is_regular_file(home / FromUtf8(name), error)) RemoveFile(home / FromUtf8(name));
        else if (Exists(home / FromUtf8(name))) RemoveOwnTree(home / FromUtf8(name));
    }
    std::error_code error;
    if (game.format == PackageFormat::Folder && !manifest->keptApp0) std::filesystem::remove(app0, error);
    if (game.format == PackageFormat::Zar && result.leftovers.empty()) std::filesystem::remove(home, error);
    return result;
}

}
