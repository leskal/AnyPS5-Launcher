#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

class ZArchiveReader;

namespace launcher {

class PackageReader {
public:
    virtual ~PackageReader() = default;

    virtual std::optional<std::uint64_t> FileSize(std::string_view relative) = 0;
    virtual std::vector<std::uint8_t> ReadRange(std::string_view relative, std::uint64_t offset, std::uint64_t length) = 0;
    virtual bool IsDirectory(std::string_view relative) = 0;
    // Names of the regular files directly inside a folder, sorted.
    virtual std::vector<std::string> ListFiles(std::string_view directory) = 0;
    // Paths, relative to the package root, of every file under a folder, sorted.
    virtual std::vector<std::string> ListTree(std::string_view directory) = 0;

    std::optional<std::vector<std::uint8_t>> ReadAll(std::string_view relative, std::uint64_t maxSize);
    std::optional<std::vector<std::uint8_t>> ReadHead(std::string_view relative, std::uint64_t count);
};

class FolderPackage final : public PackageReader {
public:
    explicit FolderPackage(std::filesystem::path root);

    std::optional<std::uint64_t> FileSize(std::string_view relative) override;
    std::vector<std::uint8_t> ReadRange(std::string_view relative, std::uint64_t offset, std::uint64_t length) override;
    bool IsDirectory(std::string_view relative) override;
    std::vector<std::string> ListFiles(std::string_view directory) override;
    std::vector<std::string> ListTree(std::string_view directory) override;

private:
    std::filesystem::path root_;
};

struct ZarEntry {
    std::string name;
    bool isDirectory = false;
    std::uint64_t size = 0;
};

class ZarArchive {
public:
    static std::shared_ptr<ZarArchive> Open(const std::filesystem::path& path);
    ~ZarArchive();

    ZarArchive(const ZarArchive&) = delete;
    ZarArchive& operator=(const ZarArchive&) = delete;

    std::optional<std::uint64_t> FileSize(std::string_view path);
    bool IsDirectory(std::string_view path);
    std::vector<std::uint8_t> ReadRange(std::string_view path, std::uint64_t offset, std::uint64_t length);
    std::vector<std::string> RootDirectories();
    std::vector<ZarEntry> List(std::string_view directory);

private:
    explicit ZarArchive(ZArchiveReader* reader);
    ZArchiveReader* reader_;
};

class ZarPackage final : public PackageReader {
public:
    ZarPackage(std::shared_ptr<ZarArchive> archive, std::string prefix);

    std::optional<std::uint64_t> FileSize(std::string_view relative) override;
    std::vector<std::uint8_t> ReadRange(std::string_view relative, std::uint64_t offset, std::uint64_t length) override;
    bool IsDirectory(std::string_view relative) override;
    std::vector<std::string> ListFiles(std::string_view directory) override;
    std::vector<std::string> ListTree(std::string_view directory) override;

private:
    std::shared_ptr<ZarArchive> archive_;
    std::string prefix_;
};

}
