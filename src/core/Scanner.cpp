#include "core/Scanner.hpp"

#include <algorithm>
#include <cctype>
#include <exception>
#include <stdexcept>
#include <system_error>

#include "core/Encryption.hpp"
#include "core/Modules.hpp"
#include "core/Package.hpp"
#include "core/Utf8.hpp"

namespace launcher {

namespace {

bool HoldsModule(PackageReader& package, const std::string& folder, const std::vector<std::string>& files) {
    for (const auto& name : files) {
        if (IsSelfBackup(name) || IsGuestModule(name)) continue;
        std::string low = name;
        std::transform(low.begin(), low.end(), low.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (!low.ends_with(".prx") && !low.ends_with(".sprx") && !low.ends_with(".suprx")) continue;
        const auto head = package.ReadHead(folder + "/" + name, 4);
        if (head && ModuleKindOf(*head) != ModuleKind::NotModule) return true;
    }
    return false;
}

}

std::vector<std::string> KnownModuleDirectories(PackageReader& package) {
    std::vector<std::string> found;
    for (const char* folder : {"Media/Modules", "Media/Plugins"}) {
        try {
            if (package.IsDirectory(folder) && HoldsModule(package, folder, package.ListFiles(folder))) found.emplace_back(folder);
        } catch (const std::exception&) {
        }
    }
    return found;
}

std::vector<std::string> FindModuleDirectories(PackageReader& package) {
    constexpr int MaxDepth = 4;
    constexpr std::size_t MaxFolders = 4000;  // a big game's data folders aren't walked for ever
    const auto standard = ModuleFolders();
    std::vector<std::string> found;
    std::vector<std::pair<std::string, int>> pending{{std::string(), 0}};
    std::size_t visited = 0;
    std::vector<std::string> files;
    std::vector<std::string> children;
    while (!pending.empty() && visited < MaxFolders) {
        auto [folder, depth] = std::move(pending.back());
        pending.pop_back();
        ++visited;
        try {
            package.ListChildren(folder, files, children);
        } catch (const std::exception&) {
            continue;  // an unreadable folder holds nothing we can use
        }
        if (!folder.empty() && HoldsModule(package, folder, files)) found.push_back(folder);
        if (depth >= MaxDepth) continue;
        for (const auto& child : children) {
            std::string low = child;
            std::transform(low.begin(), low.end(), low.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (folder.empty() && (low == "sce_sys" || std::find(standard.begin(), standard.end(), low) != standard.end())) continue;
            pending.emplace_back(folder.empty() ? child : folder + "/" + child, depth + 1);
        }
    }
    std::sort(found.begin(), found.end());
    return found;
}

namespace {

constexpr std::string_view ParamJson = "sce_sys/param.json";
constexpr std::string_view ParamSfo = "sce_sys/param.sfo";
constexpr std::string_view Icon = "sce_sys/icon0.png";
constexpr std::string_view ChangeInfo = "sce_sys/changeinfo/changeinfo.xml";
constexpr std::string_view Eboot = "eboot.bin";
constexpr std::string_view UnwrappedEboot = "eboot.elf";

bool HasZarExtension(const std::filesystem::path& path) {
    auto extension = ToUtf8(path.extension());
    std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return extension == ".zar";
}

bool IsPartial(const std::filesystem::path& path) {
    return ToUtf8(path.extension()) == ".partial";
}

bool HasMetadata(PackageReader& package) {
    return package.FileSize(ParamJson).has_value() || package.FileSize(ParamSfo).has_value();
}

GameEntry ReadEntry(PackageReader& package, const ScanOptions& options) {
    GameEntry entry;
    if (auto json = package.ReadAll(ParamJson, options.maxMetadataSize)) {
        entry.metadata = ParseParamJson(*json, options.preferredLocale);
    } else if (auto sfo = package.ReadAll(ParamSfo, options.maxMetadataSize)) {
        entry.metadata = ParseParamSfo(*sfo);
    } else {
        throw std::runtime_error("no sce_sys/param.json or sce_sys/param.sfo");
    }
    const auto classify = [&](std::string_view file) {
        auto info = ClassifyExecutable(*package.ReadHead(file, ExecutableHeadSize));
        if (info.kind != ExecutableKind::Elf) return info;
        const auto size = package.FileSize(file);
        if (!size) return info;
        if (auto reason = ElfEncryption(*size, [&](std::uint64_t offset, std::uint64_t length) { return package.ReadRange(file, offset, length); })) {
            info = {ExecutableKind::ElfEncrypted, *reason};
        }
        return info;
    };
    if (package.ReadHead(Eboot, ExecutableHeadSize)) entry.executable = classify(Eboot);
    if (package.ReadHead(UnwrappedEboot, ExecutableHeadSize)) entry.unwrapped = classify(UnwrappedEboot);
    entry.moduleDirs = KnownModuleDirectories(package);
    for (const auto& folder : ModuleFolders(entry.moduleDirs)) {
        if (!package.IsDirectory(folder)) continue;
        for (const auto& name : package.ListFiles(folder)) {
            if (IsSelfBackup(name) || IsGuestModule(name)) continue;
            const auto head = package.ReadHead(folder + "/" + name, 4);
            if (!head) continue;
            const auto kind = ModuleKindOf(*head);
            if (kind == ModuleKind::Elf) ++entry.modules.elf;
            else if (kind == ModuleKind::Self) ++entry.modules.self;
        }
    }
    if (const auto size = package.FileSize(Icon); size && *size <= options.maxIconSize) {
        entry.icon = package.ReadRange(Icon, 0, *size);
    }
    try {  // only informative: a broken changeinfo never hides the game
        if (auto xml = package.ReadAll(ChangeInfo, options.maxMetadataSize)) {
            entry.changes = ParseChangeInfo(std::string_view(reinterpret_cast<const char*>(xml->data()), xml->size()));
        }
    } catch (const std::exception&) {
    }
    return entry;
}

// Windows drops these into folders on its own; they don't count as content.
bool IsShellClutter(const std::string& name) {
    std::string lower = name;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return lower == "desktop.ini" || lower == "thumbs.db";
}

// The parent of an app0 game folder, when app0 is all it holds.
std::filesystem::path App0Parent(const std::filesystem::path& folder, const std::filesystem::path& root) {
    if (ToUtf8(folder.filename()) != "app0") return {};
    const auto parent = folder.parent_path();
    std::error_code error;
    if (parent.empty() || std::filesystem::equivalent(parent, root, error)) return {};
    for (std::filesystem::directory_iterator it(parent, error), end; !error && it != end; it.increment(error)) {
        const auto name = ToUtf8(it->path().filename());
        if (name != "app0" && !IsShellClutter(name)) return {};
    }
    return error ? std::filesystem::path{} : parent;
}

bool IsExcluded(const std::filesystem::path& path, const std::vector<std::filesystem::path>& excluded) {
    std::error_code error;
    for (const auto& candidate : excluded) {
        if (std::filesystem::equivalent(path, candidate, error)) return true;
    }
    return false;
}

class Scanner {
public:
    explicit Scanner(const ScanOptions& options) : options_(options) {}

    ScanResult Run() {
        for (const auto& root : options_.roots) ScanRoot(root);
        std::sort(result_.games.begin(), result_.games.end(), [](const GameEntry& left, const GameEntry& right) {
            if (left.metadata.name != right.metadata.name) return left.metadata.name < right.metadata.name;
            return left.location < right.location;
        });
        return std::move(result_);
    }

private:
    void ScanRoot(const std::filesystem::path& root) {
        std::error_code error;
        if (!std::filesystem::is_directory(root, error)) {
            Problem(root, "games folder does not exist or is not a folder");
            return;
        }
        if (IsExcluded(root, options_.excluded)) return;
        root_ = root;
        if (TryFolder(root)) return;
        std::filesystem::recursive_directory_iterator walker(root, std::filesystem::directory_options::skip_permission_denied, error);
        if (error) {
            Problem(root, error.message());
            return;
        }
        const std::filesystem::recursive_directory_iterator end;
        while (walker != end) {
            const auto& item = *walker;
            if (item.is_symlink(error)) {
                walker.disable_recursion_pending();
            } else if (item.is_directory(error)) {
                if (IsPartial(item.path()) || IsExcluded(item.path(), options_.excluded) || TryFolder(item.path())) walker.disable_recursion_pending();
            } else if (item.is_regular_file(error) && HasZarExtension(item.path())) {
                TryZar(item.path());
            }
            const auto current = item.path();
            walker.increment(error);
            if (error) {
                Problem(current, "scan stopped here: " + error.message());
                return;
            }
        }
    }

    bool TryFolder(const std::filesystem::path& folder) {
        std::error_code error;
        if (std::filesystem::is_regular_file(folder / GameHomeMarker, error)) {
            if (std::filesystem::is_regular_file(folder / "app0.zar", error)) TryZar(folder / "app0.zar", folder);
            else if (!TryGameFolder(folder / "app0", folder)) Problem(folder, "set up for AnyPS5, but app0\\ holds no game");
            return true;
        }
        return TryGameFolder(folder, {});
    }

    bool TryGameFolder(const std::filesystem::path& folder, const std::filesystem::path& home) {
        FolderPackage package(folder);
        try {
            if (!HasMetadata(package) || !package.FileSize(Eboot)) return false;
            auto entry = ReadEntry(package, options_);
            entry.location = folder;
            entry.home = home;
            if (home.empty()) entry.app0Parent = App0Parent(folder, root_);
            entry.format = PackageFormat::Folder;
            result_.games.push_back(std::move(entry));
        } catch (const std::exception& failure) {
            Problem(folder, failure.what());
        }
        return true;
    }

    void TryZar(const std::filesystem::path& file, const std::filesystem::path& home = {}) {
        try {
            const auto archive = ZarArchive::Open(file);
            std::string prefix;
            auto layout = ZarLayout::Root;
            {
                ZarPackage rootPackage(archive, {});
                if (!HasMetadata(rootPackage)) {
                    std::vector<std::string> candidates;
                    for (const auto& name : archive->RootDirectories()) {
                        ZarPackage nested(archive, name + "/");
                        if (HasMetadata(nested)) candidates.push_back(name + "/");
                    }
                    if (candidates.empty()) throw std::runtime_error("no sce_sys/param.json at the archive root or one folder down");
                    if (candidates.size() > 1) throw std::runtime_error("more than one top-level folder holds a game");
                    prefix = candidates.front();
                    layout = ZarLayout::Nested;
                }
            }
            ZarPackage package(archive, prefix);
            auto entry = ReadEntry(package, options_);
            entry.location = file;
            entry.home = home;
            entry.format = PackageFormat::Zar;
            entry.layout = layout;
            entry.innerPrefix = prefix;
            result_.games.push_back(std::move(entry));
        } catch (const std::exception& failure) {
            Problem(file, failure.what());
        }
    }

    void Problem(const std::filesystem::path& location, std::string message) {
        result_.problems.push_back({location, std::move(message)});
    }

    const ScanOptions& options_;
    ScanResult result_;
    std::filesystem::path root_;
};

}

ScanResult Scan(const ScanOptions& options) {
    return Scanner(options).Run();
}

std::map<std::string, std::vector<std::size_t>> GroupByTitleId(const std::vector<GameEntry>& games) {
    std::map<std::string, std::vector<std::size_t>> groups;
    for (std::size_t index = 0; index < games.size(); ++index) groups[games[index].metadata.titleId].push_back(index);
    return groups;
}

std::string_view ToString(PackageFormat format) {
    return format == PackageFormat::Zar ? "zar" : "folder";
}

std::string_view ToString(ZarLayout layout) {
    switch (layout) {
    case ZarLayout::NotApplicable: return "";
    case ZarLayout::Root: return "app0 at root";
    case ZarLayout::Nested: return "nested folder";
    }
    return "";
}

std::unique_ptr<PackageReader> OpenGamePackage(const GameEntry& game) {
    if (game.format == PackageFormat::Zar) return std::make_unique<ZarPackage>(ZarArchive::Open(game.location), game.innerPrefix);
    return std::make_unique<FolderPackage>(game.location);
}

}
