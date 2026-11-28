#include "core/Archive.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <map>
#include <memory>
#include <stdexcept>
#include <system_error>
#include <vector>

#include <zarchive/zarchivewriter.h>

#include "core/FileOps.hpp"
#include "core/Package.hpp"
#include "core/Utf8.hpp"

namespace launcher {

namespace {

constexpr std::size_t ChunkSize = 4u << 20;

std::filesystem::path WithSuffix(const std::filesystem::path& path, const char* suffix) {
    auto result = path;
    result += suffix;
    return result;
}

std::string Lower(std::string_view text) {
    std::string result(text);
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return result;
}

void RequireAbsent(const std::filesystem::path& path, const char* what) {
    std::error_code error;
    if (std::filesystem::exists(std::filesystem::symlink_status(path, error))) {
        throw std::runtime_error(ToUtf8(path) + " already exists" + what);
    }
}

void RequireAbsentOutputs(const std::filesystem::path& output, const std::filesystem::path& partial) {
    RequireAbsent(output, "");
    RequireAbsent(partial, "; it is left over from an interrupted conversion, delete it and try again");
}

struct PackItem {
    std::string relative;
    std::filesystem::path source;
    bool isDirectory = false;
    std::uint64_t size = 0;
};

std::vector<PackItem> CollectFolder(const std::filesystem::path& folder) {
    std::vector<PackItem> items;
    std::error_code error;
    std::filesystem::recursive_directory_iterator walker(folder, error);
    if (error) throw std::system_error(error, "cannot read " + ToUtf8(folder));
    const std::filesystem::recursive_directory_iterator end;
    for (; walker != end; walker.increment(error)) {
        if (error) throw std::system_error(error, "cannot read " + ToUtf8(folder));
        const auto& item = *walker;
        const auto relative = ToUtf8(item.path().lexically_relative(folder).generic_u8string());
        if (item.is_symlink(error)) throw std::runtime_error(relative + " is a link; packing links is not supported");
        PackItem entry{relative, item.path(), false, 0};
        for (const auto& component : FromUtf8(relative)) {
            if (!IsSafeArchiveName(ToUtf8(component))) throw std::runtime_error("\"" + relative + "\" has a name that can't be packed (only printable ASCII names that Windows accepts are supported)");
        }
        if (item.is_directory(error)) {
            entry.isDirectory = true;
        } else if (item.is_regular_file(error)) {
            entry.size = item.file_size(error);
            if (error) throw std::system_error(error, "cannot read the size of " + relative);
        } else {
            throw std::runtime_error(relative + " is not a regular file or folder");
        }
        items.push_back(std::move(entry));
    }
    std::sort(items.begin(), items.end(), [](const PackItem& left, const PackItem& right) { return left.relative < right.relative; });
    std::map<std::string, std::string> seen;
    for (const auto& item : items) {
        const auto [found, inserted] = seen.emplace(Lower(item.relative), item.relative);
        if (!inserted) throw std::runtime_error("\"" + found->second + "\" and \"" + item.relative + "\" differ only in letter case; a .zar can't hold both");
    }
    return items;
}

struct WriterContext {
    std::filesystem::path path;
    std::ofstream stream;
    bool failed = false;
};

void VerifyArchive(const std::filesystem::path& file, const std::vector<PackItem>& items) {
    const auto archive = ZarArchive::Open(file);
    for (const auto& item : items) {
        if (item.isDirectory) {
            if (!archive->IsDirectory(item.relative)) throw std::runtime_error("verification failed: folder " + item.relative + " is missing from the archive");
            continue;
        }
        const auto size = archive->FileSize(item.relative);
        if (!size) throw std::runtime_error("verification failed: " + item.relative + " is missing from the archive");
        if (*size != item.size) throw std::runtime_error("verification failed: " + item.relative + " has " + std::to_string(*size) + " bytes instead of " + std::to_string(item.size));
    }
}

struct ExtractItem {
    std::string archivePath;
    std::filesystem::path relative;
    bool isDirectory = false;
    std::uint64_t size = 0;
};

void CollectArchive(ZarArchive& archive, const std::string& directory, const std::filesystem::path& relative, std::vector<ExtractItem>& items, int depth) {
    if (depth > 256) throw std::runtime_error("archive folders are nested too deeply");
    std::map<std::string, std::string> seen;
    for (const auto& entry : archive.List(directory)) {
        const auto shown = directory.empty() ? entry.name : directory + "/" + entry.name;
        if (!IsSafeArchiveName(entry.name)) throw std::runtime_error("\"" + shown + "\" has a name that can't be created on Windows");
        if (const auto [found, inserted] = seen.emplace(Lower(entry.name), entry.name); !inserted) {
            throw std::runtime_error("\"" + found->second + "\" and \"" + entry.name + "\" in \"" + directory + "\" differ only in letter case");
        }
        const auto childRelative = relative / FromUtf8(entry.name);
        items.push_back({shown, childRelative, entry.isDirectory, entry.size});
        if (entry.isDirectory) CollectArchive(archive, shown, childRelative, items, depth + 1);
    }
}

}

std::filesystem::path ZarOutputFor(const GameEntry& game) {
    return WithSuffix(game.location, ".zar");
}

std::filesystem::path FolderOutputFor(const GameEntry& game) {
    return game.location.parent_path() / game.location.stem();
}

bool IsSafeArchiveName(std::string_view name) {
    if (name.empty() || name == "." || name == ".." || name.size() > 255) return false;
    for (const unsigned char c : name) {
        if (c < 0x20 || c > 0x7E) return false;
        if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|') return false;
    }
    if (name.back() == '.' || name.back() == ' ') return false;
    auto base = Lower(name.substr(0, name.find('.')));
    while (!base.empty() && base.back() == ' ') base.pop_back();
    static constexpr std::array<std::string_view, 4> reserved{"con", "prn", "aux", "nul"};
    if (std::find(reserved.begin(), reserved.end(), base) != reserved.end()) return false;
    if (base.size() == 4 && (base.starts_with("com") || base.starts_with("lpt")) && base[3] >= '0' && base[3] <= '9') return false;
    return true;
}

void PackFolderToZar(const std::filesystem::path& folder, const std::filesystem::path& output, JobControl& control) {
    const auto partial = WithSuffix(output, ".partial");
    RequireAbsentOutputs(output, partial);
    const auto items = CollectFolder(folder);
    std::uint64_t total = 0;
    for (const auto& item : items) total += item.size;
    control.SetTotal(total);
    RequireFreeSpace(output.parent_path(), total);

    WriterContext context{partial, {}, false};
    try {
        {
            ZArchiveWriter writer(
                [](const int32_t, void* raw) {
                    auto* ctx = static_cast<WriterContext*>(raw);
                    ctx->stream = std::ofstream(ctx->path, std::ios::binary | std::ios::trunc);
                    if (!ctx->stream) ctx->failed = true;
                },
                [](const void* data, size_t length, void* raw) {
                    auto* ctx = static_cast<WriterContext*>(raw);
                    if (ctx->failed) return;
                    ctx->stream.write(static_cast<const char*>(data), static_cast<std::streamsize>(length));
                    if (!ctx->stream) ctx->failed = true;
                },
                &context);
            const auto check = [&] {
                if (context.failed) throw std::runtime_error("cannot write " + ToUtf8(partial));
                ThrowIfCancelled(control);
            };
            check();
            std::vector<char> buffer(ChunkSize);
            for (const auto& item : items) {
                if (item.isDirectory) {
                    if (!writer.MakeDir(item.relative.c_str(), false)) throw std::runtime_error("cannot add folder " + item.relative);
                    continue;
                }
                if (!writer.StartNewFile(item.relative.c_str())) throw std::runtime_error("cannot add " + item.relative);
                std::ifstream source(item.source, std::ios::binary);
                if (!source) throw std::runtime_error("cannot open " + ToUtf8(item.source));
                std::uint64_t remaining = item.size;
                while (remaining > 0) {
                    const auto amount = static_cast<std::size_t>(std::min<std::uint64_t>(remaining, buffer.size()));
                    source.read(buffer.data(), static_cast<std::streamsize>(amount));
                    if (static_cast<std::size_t>(source.gcount()) != amount) throw std::runtime_error(item.relative + " changed or could not be read while packing");
                    writer.AppendData(buffer.data(), amount);
                    remaining -= amount;
                    control.Advance(amount);
                    check();
                }
            }
            writer.Finalize();
            check();
        }
        context.stream.close();
        if (!context.stream) throw std::runtime_error("cannot finish writing " + ToUtf8(partial));
        VerifyArchive(partial, items);
        RenameNew(partial, output);
    } catch (...) {
        context.stream.close();
        std::error_code ignored;
        std::filesystem::remove(partial, ignored);
        throw;
    }
}

void ExtractZarToFolder(const std::filesystem::path& archivePath, std::string_view innerPrefix, const std::filesystem::path& output, JobControl& control) {
    RequireAbsentOutputs(output, WithSuffix(output, ".partial"));  // .partial: left by older versions
    const auto archive = ZarArchive::Open(archivePath);
    std::string root(innerPrefix);
    while (!root.empty() && root.back() == '/') root.pop_back();
    std::vector<ExtractItem> items;
    CollectArchive(*archive, root, {}, items, 0);
    std::uint64_t total = 0;
    for (const auto& item : items) total += item.size;
    control.SetTotal(total);
    RequireFreeSpace(output.parent_path(), total);

    // Written straight into the final folder, like ZArchive does: renaming a just-written
    // folder fails while the antivirus is still scanning its files, which takes long for
    // a big game. A marker file inside says it isn't finished, so the scan skips it if
    // the conversion is cut off; it is deleted last.
    const auto marker = output / UnfinishedExtractMarker;
    bool created = false;
    try {
        std::error_code error;
        if (!std::filesystem::create_directory(output, error)) {
            throw std::system_error(error ? error : std::make_error_code(std::errc::file_exists), "cannot create " + ToUtf8(output));
        }
        created = true;
        {
            std::ofstream note(marker, std::ios::binary | std::ios::trunc);
            note << "AnyPS5 Launcher is extracting " << ToUtf8(archivePath.filename()) << " here. If this file stays, the conversion was cut off: delete the folder.\n";
            if (!note) throw std::runtime_error("cannot create " + ToUtf8(marker));
        }
        std::vector<std::uint8_t> buffer;
        for (const auto& item : items) {
            ThrowIfCancelled(control);
            const auto target = output / item.relative;
            if (item.isDirectory) {
                std::filesystem::create_directory(target);
                continue;
            }
            std::ofstream file(target, std::ios::binary | std::ios::trunc);
            if (!file) throw std::runtime_error("cannot create " + ToUtf8(target));
            std::uint64_t offset = 0;
            while (offset < item.size) {
                const auto amount = std::min<std::uint64_t>(item.size - offset, ChunkSize);
                buffer = archive->ReadRange(item.archivePath, offset, amount);
                file.write(reinterpret_cast<const char*>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
                if (!file) throw std::runtime_error("cannot write " + ToUtf8(target));
                offset += amount;
                control.Advance(amount);
                ThrowIfCancelled(control);
            }
            file.close();
            if (!file) throw std::runtime_error("cannot write " + ToUtf8(target));
        }
        RemoveTree(marker);  // waits for the antivirus if it is reading it
    } catch (...) {
        if (created) {
            try {
                RemoveTree(output);
            } catch (...) {
            }
        }
        throw;
    }
}

}
