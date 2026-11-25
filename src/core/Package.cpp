#include "core/Package.hpp"

#include <algorithm>
#include <fstream>
#include <stdexcept>
#include <system_error>

#include <zarchive/zarchivereader.h>

#include "core/Utf8.hpp"

namespace launcher {

std::optional<std::vector<std::uint8_t>> PackageReader::ReadAll(std::string_view relative, std::uint64_t maxSize) {
    const auto size = FileSize(relative);
    if (!size) return std::nullopt;
    if (*size > maxSize) throw std::runtime_error(std::string(relative) + " is " + std::to_string(*size) + " bytes, more than the " + std::to_string(maxSize) + " allowed");
    return ReadRange(relative, 0, *size);
}

std::optional<std::vector<std::uint8_t>> PackageReader::ReadHead(std::string_view relative, std::uint64_t count) {
    const auto size = FileSize(relative);
    if (!size) return std::nullopt;
    return ReadRange(relative, 0, *size < count ? *size : count);
}

FolderPackage::FolderPackage(std::filesystem::path root) : root_(std::move(root)) {}

std::optional<std::uint64_t> FolderPackage::FileSize(std::string_view relative) {
    std::error_code error;
    const auto path = root_ / FromUtf8(relative);
    if (!std::filesystem::is_regular_file(path, error)) return std::nullopt;
    const auto size = std::filesystem::file_size(path, error);
    if (error) throw std::system_error(error, ToUtf8(path));
    return size;
}

std::vector<std::uint8_t> FolderPackage::ReadRange(std::string_view relative, std::uint64_t offset, std::uint64_t length) {
    const auto path = root_ / FromUtf8(relative);
    std::ifstream file(path, std::ios::binary);
    if (!file) throw std::runtime_error("cannot open " + ToUtf8(path));
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(length));
    file.seekg(static_cast<std::streamoff>(offset));
    file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(length));
    if (static_cast<std::uint64_t>(file.gcount()) != length) throw std::runtime_error("cannot read " + std::to_string(length) + " bytes from " + ToUtf8(path));
    return bytes;
}

bool FolderPackage::IsDirectory(std::string_view relative) {
    std::error_code error;
    return std::filesystem::is_directory(root_ / FromUtf8(relative), error);
}

std::vector<std::string> FolderPackage::ListFiles(std::string_view directory) {
    std::vector<std::string> names;
    std::error_code error;
    for (std::filesystem::directory_iterator it(root_ / FromUtf8(directory), error), end; !error && it != end; it.increment(error)) {
        if (!it->is_symlink(error) && it->is_regular_file(error)) names.push_back(ToUtf8(it->path().filename()));
    }
    if (error) throw std::system_error(error, "cannot read " + ToUtf8(root_ / FromUtf8(directory)));
    std::sort(names.begin(), names.end());
    return names;
}

std::vector<std::string> FolderPackage::ListDirectories(std::string_view directory) {
    std::vector<std::string> names;
    std::error_code error;
    for (std::filesystem::directory_iterator it(root_ / FromUtf8(directory), error), end; !error && it != end; it.increment(error)) {
        if (!it->is_symlink(error) && it->is_directory(error)) names.push_back(ToUtf8(it->path().filename()));
    }
    std::sort(names.begin(), names.end());
    return names;
}

void FolderPackage::ListChildren(std::string_view directory, std::vector<std::string>& files, std::vector<std::string>& folders) {
    files.clear();
    folders.clear();
    std::error_code error;
    for (std::filesystem::directory_iterator it(root_ / FromUtf8(directory), error), end; !error && it != end; it.increment(error)) {
        // The entry's type comes with the listing on Windows: no extra look per file.
        if (it->is_symlink(error)) continue;
        if (it->is_directory(error)) folders.push_back(ToUtf8(it->path().filename()));
        else if (it->is_regular_file(error)) files.push_back(ToUtf8(it->path().filename()));
    }
    std::sort(files.begin(), files.end());
    std::sort(folders.begin(), folders.end());
}

std::vector<std::string> FolderPackage::ListTree(std::string_view directory) {
    std::vector<std::string> paths;
    std::error_code error;
    const auto start = root_ / FromUtf8(directory);
    if (!std::filesystem::is_directory(start, error)) return paths;
    for (std::filesystem::recursive_directory_iterator it(start, error), end; !error && it != end; it.increment(error)) {
        if (!it->is_symlink(error) && it->is_regular_file(error)) paths.push_back(ToUtf8(it->path().lexically_relative(root_).generic_u8string()));
    }
    if (error) throw std::system_error(error, "cannot read " + ToUtf8(start));
    std::sort(paths.begin(), paths.end());
    return paths;
}

std::shared_ptr<ZarArchive> ZarArchive::Open(const std::filesystem::path& path) {
    auto* reader = ZArchiveReader::OpenFromFile(path);
    if (reader == nullptr) throw std::runtime_error("not a readable ZArchive");
    return std::shared_ptr<ZarArchive>(new ZarArchive(reader));
}

ZarArchive::ZarArchive(ZArchiveReader* reader) : reader_(reader) {}

ZarArchive::~ZarArchive() { delete reader_; }

std::optional<std::uint64_t> ZarArchive::FileSize(std::string_view path) {
    const auto node = reader_->LookUp(path);
    if (node == ZARCHIVE_INVALID_NODE || !reader_->IsFile(node)) return std::nullopt;
    return reader_->GetFileSize(node);
}

bool ZarArchive::IsDirectory(std::string_view path) {
    const auto node = reader_->LookUp(path);
    return node != ZARCHIVE_INVALID_NODE && reader_->IsDirectory(node);
}

std::vector<std::uint8_t> ZarArchive::ReadRange(std::string_view path, std::uint64_t offset, std::uint64_t length) {
    const auto node = reader_->LookUp(path);
    if (node == ZARCHIVE_INVALID_NODE || !reader_->IsFile(node)) throw std::runtime_error(std::string(path) + " is not a file in the archive");
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(length));
    const auto read = reader_->ReadFromFile(node, offset, length, bytes.data());
    if (read != length) throw std::runtime_error("archive is damaged: " + std::string(path) + " returned " + std::to_string(read) + " of " + std::to_string(length) + " bytes");
    return bytes;
}

std::vector<std::string> ZarArchive::RootDirectories() {
    std::vector<std::string> names;
    for (auto& entry : List("")) {
        if (entry.isDirectory) names.push_back(std::move(entry.name));
    }
    return names;
}

std::vector<ZarEntry> ZarArchive::List(std::string_view directory) {
    std::vector<ZarEntry> entries;
    const auto node = reader_->LookUp(directory);
    if (node == ZARCHIVE_INVALID_NODE || !reader_->IsDirectory(node)) throw std::runtime_error(std::string(directory) + " is not a folder in the archive");
    const auto count = reader_->GetDirEntryCount(node);
    entries.reserve(count);
    for (std::uint32_t index = 0; index < count; ++index) {
        ZArchiveReader::DirEntry entry;
        if (!reader_->GetDirEntry(node, index, entry)) throw std::runtime_error("archive is damaged: entry " + std::to_string(index) + " of \"" + std::string(directory) + "\" cannot be read");
        entries.push_back({std::string(entry.name), entry.isDirectory, entry.size});
    }
    return entries;
}

ZarPackage::ZarPackage(std::shared_ptr<ZarArchive> archive, std::string prefix) : archive_(std::move(archive)), prefix_(std::move(prefix)) {}

std::optional<std::uint64_t> ZarPackage::FileSize(std::string_view relative) {
    return archive_->FileSize(prefix_ + std::string(relative));
}

std::vector<std::uint8_t> ZarPackage::ReadRange(std::string_view relative, std::uint64_t offset, std::uint64_t length) {
    return archive_->ReadRange(prefix_ + std::string(relative), offset, length);
}

namespace {

std::string Join(const std::string& prefix, std::string_view relative) {
    auto path = prefix + std::string(relative);
    while (!path.empty() && path.back() == '/') path.pop_back();
    return path;
}

}

bool ZarPackage::IsDirectory(std::string_view relative) {
    return archive_->IsDirectory(Join(prefix_, relative));
}

std::vector<std::string> ZarPackage::ListFiles(std::string_view directory) {
    std::vector<std::string> names;
    for (auto& entry : archive_->List(Join(prefix_, directory))) {
        if (!entry.isDirectory) names.push_back(std::move(entry.name));
    }
    std::sort(names.begin(), names.end());
    return names;
}

std::vector<std::string> ZarPackage::ListDirectories(std::string_view directory) {
    std::vector<std::string> names;
    if (!IsDirectory(directory)) return names;
    for (auto& entry : archive_->List(Join(prefix_, directory))) {
        if (entry.isDirectory) names.push_back(std::move(entry.name));
    }
    std::sort(names.begin(), names.end());
    return names;
}

std::vector<std::string> ZarPackage::ListTree(std::string_view directory) {
    std::vector<std::string> paths;
    if (!IsDirectory(directory)) return paths;
    std::vector<std::string> pending{std::string(directory)};
    while (!pending.empty()) {
        auto current = std::move(pending.back());
        pending.pop_back();
        for (auto& entry : archive_->List(Join(prefix_, current))) {
            auto path = current.empty() ? entry.name : current + "/" + entry.name;
            if (entry.isDirectory) pending.push_back(std::move(path));
            else paths.push_back(std::move(path));
        }
    }
    std::sort(paths.begin(), paths.end());
    return paths;
}

}
