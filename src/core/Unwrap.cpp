#include "core/Unwrap.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <sstream>
#include <span>
#include <stdexcept>
#include <system_error>
#include <vector>

#include "core/Executable.hpp"
#include "core/FileOps.hpp"
#include "core/Modules.hpp"
#include "core/SelfUnwrap.hpp"
#include "core/Utf8.hpp"
#include "core/ZipStore.hpp"

namespace launcher {

namespace {

std::vector<std::uint8_t> ReadWholeFile(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) throw std::runtime_error("cannot open " + ToUtf8(path));
    std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    if (file.bad()) throw std::runtime_error("cannot read " + ToUtf8(path));
    return bytes;
}

std::vector<std::uint8_t> ReadHead(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    std::vector<std::uint8_t> head(4);
    file.read(reinterpret_cast<char*>(head.data()), 4);
    head.resize(static_cast<std::size_t>(file.gcount()));
    return head;
}

void WriteNew(const std::filesystem::path& file, const std::vector<std::uint8_t>& bytes) {
    std::ofstream stream(file, std::ios::binary | std::ios::trunc);
    stream.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    stream.close();
    if (!stream) throw std::runtime_error("cannot write " + ToUtf8(file));
}

void RemoveFile(const std::filesystem::path& file) {
    std::error_code error;
    std::filesystem::remove(file, error);
    if (error) throw std::system_error(error, "cannot delete " + ToUtf8(file));
}

// "sce_module/libc.prx" -> path under the game folder; refuses anything that could leave it.
std::filesystem::path Inside(const std::filesystem::path& game, const std::string& name) {
    if (name.empty() || name.front() == '/' || name.find('\\') != std::string::npos || name.find(':') != std::string::npos) {
        throw std::runtime_error("unsafe name in the originals zip: " + name);
    }
    auto path = game;
    std::size_t begin = 0;
    while (begin <= name.size()) {
        const auto end = std::min(name.find('/', begin), name.size());
        const auto part = name.substr(begin, end - begin);
        if (part.empty() || part == "." || part == "..") throw std::runtime_error("unsafe name in the originals zip: " + name);
        path /= FromUtf8(part);
        begin = end + 1;
    }
    return path;
}

std::map<std::string, std::string> ReadChecksums(ZipReader& reader) {
    std::map<std::string, std::string> sums;
    const auto* entry = reader.Find(OriginalsChecksums);
    if (entry == nullptr) return sums;
    const auto bytes = reader.ReadAll(*entry);
    std::istringstream lines(std::string(bytes.begin(), bytes.end()));
    std::string line;
    while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.size() < 35 || line[32] != ' ') continue;
        auto name = line.substr(34);
        sums[name] = line.substr(0, 32);
    }
    return sums;
}

struct Original {
    std::string name;             // path in the game folder, forward slashes
    std::filesystem::path source; // the file whose bytes go into the zip
    std::filesystem::path target; // where the ELF goes (the same file, or the module of a .self backup)
    std::filesystem::path elf;    // <target>.elf.tmp; empty for a .self backup (the ELF is already in place)
    std::uint64_t size = 0;
};

}

std::filesystem::path UnwrappedEbootFor(const GameEntry& game) {
    return game.location / UnwrappedEbootName;
}

void UnwrapSelf(const std::filesystem::path& input, const std::filesystem::path& output) {
    std::error_code error;
    if (std::filesystem::exists(output, error) && std::filesystem::equivalent(input, output, error)) {
        throw UnwrapRefused("output aliases the input container");
    }
    const auto image = UnwrapSelfImage(ReadWholeFile(input));
    const auto info = ClassifyExecutable(std::span(image).first(std::min<std::size_t>(image.size(), ExecutableHeadSize)));
    if (info.kind != ExecutableKind::Elf) {
        throw std::runtime_error("the unwrapped file isn't a supported ELF: " + std::string(ToString(info.kind)) + (info.detail.empty() ? "" : " (" + info.detail + ")"));
    }
    WriteFileAtomically(output, std::string_view(reinterpret_cast<const char*>(image.data()), image.size()));
}

std::string OriginalsZipName(const GameEntry& game) {
    const auto base = !game.home.empty() ? game.home : !game.app0Parent.empty() ? game.app0Parent : game.location;
    // Named from param.json, so folder names don't matter: PPSA21567-01.018.000.originals.zip.
    const auto safe = [](std::string text) {
        for (auto& c : text) {
            if (!std::isalnum(static_cast<unsigned char>(c)) && c != '.' && c != '-' && c != '_') c = '_';
        }
        return text;
    };
    auto name = safe(game.metadata.titleId.empty() ? ToUtf8(base.filename()) : game.metadata.titleId);
    if (!game.metadata.contentVersion.empty()) name += "-" + safe(game.metadata.contentVersion);
    return name + ".originals.zip";
}

std::filesystem::path OriginalsZipFor(const GameEntry& game, const std::filesystem::path& library) {
    return library / "originals" / FromUtf8(OriginalsZipName(game));
}

SelfConversion ConvertGameSelfFiles(const std::filesystem::path& game, const std::filesystem::path& zip, const std::vector<std::string>& moduleFolders,
                                    const std::vector<std::string>& executables, JobControl& control) {
    SelfConversion result;
    std::error_code error;
    std::vector<std::pair<std::string, std::filesystem::path>> candidates;  // name, file
    if (ModuleKindOf(ReadHead(game / "eboot.bin")) == ModuleKind::Self) candidates.push_back({"eboot.bin", game / "eboot.bin"});
    std::vector<std::pair<std::string, std::filesystem::path>> legacy;      // module name, its .self backup
    for (const auto& folder : moduleFolders) {
        const auto directory = game / FromUtf8(folder);
        if (!std::filesystem::is_directory(directory, error)) continue;
        std::vector<std::filesystem::path> files;
        for (std::filesystem::directory_iterator it(directory, error), end; !error && it != end; it.increment(error)) {
            if (!it->is_symlink(error) && it->is_regular_file(error)) files.push_back(it->path());
        }
        std::sort(files.begin(), files.end());
        for (const auto& file : files) {
            const auto name = ToUtf8(file.filename());
            if (IsGuestModule(name) || name.ends_with(".elf.tmp")) continue;
            if (IsSelfBackup(name)) {
                const auto module = name.substr(0, name.size() - SelfBackupSuffix.size());
                if (std::filesystem::is_regular_file(directory / FromUtf8(module), error)) legacy.push_back({folder + "/" + module, file});
                continue;
            }
            if (ModuleKindOf(ReadHead(file)) == ModuleKind::Self) candidates.push_back({folder + "/" + name, file});
        }
    }
    for (const auto& executable : executables) {
        const auto file = Inside(game, executable);
        if (std::filesystem::is_regular_file(file, error) && ModuleKindOf(ReadHead(file)) == ModuleKind::Self) candidates.push_back({executable, file});
    }

    std::optional<ZipReader> existing;
    std::map<std::string, std::string> sums;
    if (std::filesystem::exists(zip, error)) {
        existing.emplace(zip);
        sums = ReadChecksums(*existing);
    }

    // 1. Unwrap into temporary files; nothing in the game changes yet.
    std::vector<Original> originals;
    const auto cleanup = [&] {
        for (const auto& original : originals) {
            if (!original.elf.empty()) std::filesystem::remove(original.elf, error);
        }
    };
    control.SetTotal(1);
    try {
        for (const auto& [name, file] : candidates) {
            ThrowIfCancelled(control);
            if (sums.count(name)) {
                result.refused.push_back(name + ": " + ToUtf8(zip.filename()) + " already holds an original of it; restore the originals first");
                continue;
            }
            auto elf = file;
            elf += ".elf.tmp";
            try {
                const auto image = UnwrapSelfImage(ReadWholeFile(file));
                if (ModuleKindOf(std::span(image).first(std::min<std::size_t>(image.size(), 4))) != ModuleKind::Elf) throw std::runtime_error("the unwrapped file isn't an ELF");
                WriteNew(elf, image);
            } catch (const Cancelled&) {
                throw;
            } catch (const std::exception& failure) {
                std::filesystem::remove(elf, error);
                result.refused.push_back(name + ": " + failure.what());
                continue;
            }
            originals.push_back({name, file, file, elf, std::filesystem::file_size(file, error)});
        }
        for (const auto& [name, backup] : legacy) {
            if (sums.count(name)) continue;
            originals.push_back({name, backup, Inside(game, name), {}, std::filesystem::file_size(backup, error)});
        }
        if (originals.empty()) return result;

        // 2. The zip: the old entries, then the new originals, then MD5SUMS; checked before it replaces the old one.
        std::uint64_t total = 0;
        for (const auto& original : originals) total += original.size * 2;
        if (existing) {
            for (const auto& entry : existing->Entries()) total += entry.size * 2;
        }
        control.SetTotal(total + 1);
        std::filesystem::create_directories(zip.parent_path(), error);
        auto partial = zip;
        partial += ".partial";
        std::filesystem::remove(partial, error);
        try {
            {
                ZipWriter writer(partial);
                if (existing) {
                    for (const auto& entry : existing->Entries()) {
                        if (entry.name == OriginalsChecksums) continue;
                        const auto temporary = partial.parent_path() / FromUtf8(ToUtf8(partial.filename()) + ".entry");
                        const auto md5 = existing->Extract(entry, temporary, control);
                        if (sums.count(entry.name) && sums[entry.name] != md5) {
                            std::filesystem::remove(temporary, error);
                            throw std::runtime_error(entry.name + " in " + ToUtf8(zip) + " doesn't match its MD5; the zip is left as it is");
                        }
                        writer.AddFile(entry.name, temporary, control);
                        std::filesystem::remove(temporary, error);
                    }
                }
                for (auto& original : originals) sums[original.name] = writer.AddFile(original.name, original.source, control);
                std::string list;
                for (const auto& [name, md5] : sums) list += md5 + "  " + name + "\n";
                writer.AddBytes(OriginalsChecksums, std::span(reinterpret_cast<const std::uint8_t*>(list.data()), list.size()));
                writer.Finish();
            }
            {
                // Closed before the rename: Windows can't rename a file that is open.
                ZipReader check(partial);
                for (const auto& original : originals) {
                    const auto* entry = check.Find(original.name);
                    if (entry == nullptr || check.Md5Of(*entry, control) != sums[original.name] || FileMd5(original.source, &control) != sums[original.name]) {
                        throw std::runtime_error("the backup of " + original.name + " doesn't match the original; nothing was converted");
                    }
                }
            }
            existing.reset();
            // One step: the old zip stays until the new one replaces it.
            if (std::filesystem::exists(zip, error)) ReplaceExistingFile(partial, zip);
            else RenameNew(partial, zip);
        } catch (...) {
            std::filesystem::remove(partial, error);
            throw;
        }
    } catch (...) {
        cleanup();
        throw;
    }

    // 3. Now the originals are safe: put the ELFs in place.
    for (auto& original : originals) {
        if (original.elf.empty()) {
            RemoveFile(original.source);
            ++result.migrated;
            continue;
        }
        ReplaceExistingFile(original.elf, original.target);
        original.elf.clear();
        if (original.name == "eboot.bin") {
            result.eboot = true;
            std::filesystem::remove(game / UnwrappedEbootName, error);
        } else {
            ++result.modules;
        }
    }
    control.Advance(control.Total() - std::min(control.Total(), control.Done()));
    return result;
}

int RestoreOriginals(const std::filesystem::path& game, const std::filesystem::path& zip, JobControl& control) {
    std::vector<std::pair<std::filesystem::path, std::filesystem::path>> ready;  // temporary, target
    std::error_code error;
    try {
        // Closed before the zip is deleted: Windows can't delete a file that is open.
        ZipReader reader(zip);
        const auto sums = ReadChecksums(reader);
        std::uint64_t total = 0;
        for (const auto& entry : reader.Entries()) total += entry.size;
        control.SetTotal(total + 1);
        for (const auto& entry : reader.Entries()) {
            if (entry.name == OriginalsChecksums) continue;
            const auto found = sums.find(entry.name);
            if (found == sums.end()) throw std::runtime_error(entry.name + " has no MD5 in " + std::string(OriginalsChecksums) + "; nothing was restored");
            const auto target = Inside(game, entry.name);
            auto temporary = target;
            temporary += ".restore.tmp";
            std::filesystem::create_directories(target.parent_path(), error);
            ready.push_back({temporary, target});
            if (reader.Extract(entry, temporary, control) != found->second) throw std::runtime_error(entry.name + " doesn't match its MD5; nothing was restored");
        }
    } catch (...) {
        for (const auto& [temporary, target] : ready) std::filesystem::remove(temporary, error);
        throw;
    }
    for (const auto& [temporary, target] : ready) ReplaceExistingFile(temporary, target);
    std::filesystem::remove(zip, error);
    if (error) throw std::runtime_error("restored " + std::to_string(ready.size()) + " files, but cannot delete " + ToUtf8(zip) + ": " + ReadableError(error));
    control.Advance(control.Total() - std::min(control.Total(), control.Done()));
    return static_cast<int>(ready.size());
}

}
