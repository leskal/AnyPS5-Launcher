#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <vector>

#include "core/Job.hpp"

namespace launcher {

// MD5 (RFC 1321), fed in pieces.
class Md5 {
public:
    Md5();
    void Update(std::span<const std::uint8_t> data);
    std::string HexDigest();  // finishes; call once

private:
    void Block(const std::uint8_t* block);

    std::array<std::uint32_t, 4> state_;
    std::uint64_t length_ = 0;
    std::array<std::uint8_t, 64> buffer_{};
    std::size_t used_ = 0;
};

std::string FileMd5(const std::filesystem::path& file, JobControl* control = nullptr);

struct ZipEntry {
    std::string name;  // forward slashes
    std::uint64_t size = 0;
    std::uint32_t crc32 = 0;
    std::uint64_t dataOffset = 0;
};

// Uncompressed ("stored") zip files that Windows Explorer and every zip tool
// open, with ZIP64 records when a file or the archive passes 4 GiB.
class ZipWriter {
public:
    explicit ZipWriter(const std::filesystem::path& file);
    ~ZipWriter();

    // Copies a file in; returns its MD5.
    std::string AddFile(const std::string& name, const std::filesystem::path& source, JobControl& control);
    void AddBytes(const std::string& name, std::span<const std::uint8_t> bytes);
    void Finish();

private:
    struct Written {
        std::string name;
        std::uint64_t size = 0;
        std::uint32_t crc32 = 0;
        std::uint64_t offset = 0;
    };
    void Begin(const std::string& name, std::uint64_t size, std::uint32_t crc32);

    std::filesystem::path file_;
    std::ofstream stream_;
    std::vector<Written> entries_;
    bool finished_ = false;
};

// Reads zip files ZipWriter made (stored entries only).
class ZipReader {
public:
    explicit ZipReader(const std::filesystem::path& file);

    const std::vector<ZipEntry>& Entries() const { return entries_; }
    const ZipEntry* Find(const std::string& name) const;
    std::vector<std::uint8_t> ReadAll(const ZipEntry& entry);
    // Writes an entry to a file, checking its CRC-32; returns its MD5.
    std::string Extract(const ZipEntry& entry, const std::filesystem::path& target, JobControl& control);
    // Reads an entry through, checking its CRC-32; returns its MD5.
    std::string Md5Of(const ZipEntry& entry, JobControl& control);

private:
    std::string Stream(const ZipEntry& entry, std::ofstream* output, JobControl& control);

    std::filesystem::path file_;
    std::ifstream stream_;
    std::vector<ZipEntry> entries_;
};

}
