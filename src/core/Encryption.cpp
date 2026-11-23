#include "core/Encryption.hpp"

#include <algorithm>
#include <cstdio>

#include "core/SelfUnwrap.hpp"

namespace launcher {

namespace {

constexpr std::uint32_t PtLoad = 1;
constexpr std::uint64_t HeaderSize = 64;
constexpr std::uint64_t EntrySize = 56;
constexpr std::uint16_t MaxEntries = 512;

std::uint64_t U64(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    std::uint64_t value = 0;
    for (int index = 7; index >= 0; --index) value = (value << 8) | bytes[offset + static_cast<std::size_t>(index)];
    return value;
}

std::uint32_t U32(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    return static_cast<std::uint32_t>(bytes[offset]) | (static_cast<std::uint32_t>(bytes[offset + 1]) << 8) |
           (static_cast<std::uint32_t>(bytes[offset + 2]) << 16) | (static_cast<std::uint32_t>(bytes[offset + 3]) << 24);
}

std::uint16_t U16(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    return static_cast<std::uint16_t>(bytes[offset] | (bytes[offset + 1] << 8));
}

std::string Hex(std::uint64_t value) {
    char text[24];
    std::snprintf(text, sizeof(text), "0x%llx", static_cast<unsigned long long>(value));
    return text;
}

}

std::optional<std::string> ElfEncryption(std::uint64_t fileSize, const RangeReader& read) {
    if (fileSize < HeaderSize) return std::nullopt;
    const auto header = read(0, HeaderSize);
    if (header.size() < HeaderSize || header[0] != 0x7F || header[1] != 'E' || header[2] != 'L' || header[3] != 'F' || header[4] != 2 || header[5] != 1) {
        return std::nullopt;
    }
    const auto tableOffset = U64(header, 0x20);
    const auto entrySize = U16(header, 0x36);
    const auto count = U16(header, 0x38);
    if (entrySize < EntrySize || count == 0 || count > MaxEntries || tableOffset > fileSize || std::uint64_t{entrySize} * count > fileSize - tableOffset) {
        return std::nullopt;
    }
    const auto table = read(tableOffset, std::uint64_t{entrySize} * count);
    if (table.size() < std::uint64_t{entrySize} * count) return std::nullopt;

    struct Encrypted {
        int index;
        std::uint64_t offset;
        double value;
    };
    std::vector<Encrypted> encrypted;
    std::uint64_t encryptedLarge = 0;
    std::uint64_t total = 0;
    std::size_t loads = 0;
    for (int index = 0; index < count; ++index) {
        const std::size_t entry = static_cast<std::size_t>(index) * entrySize;
        if (U32(table, entry) != PtLoad) continue;
        const auto offset = U64(table, entry + 0x08);
        auto size = U64(table, entry + 0x20);
        if (size == 0 || offset >= fileSize) continue;
        size = std::min(size, fileSize - offset);
        ++loads;
        total += size;
        double lowest = 0.0;
        if (size <= EncryptedEntropySample) {
            lowest = ByteEntropy(read(offset, size));
        } else {
            const std::uint64_t starts[] = {0, (size - EncryptedEntropySample) / 2, size - EncryptedEntropySample};
            for (int sample = 0; sample < 3; ++sample) {
                const double value = ByteEntropy(read(offset + starts[sample], EncryptedEntropySample));
                if (sample == 0 || value < lowest) lowest = value;
            }
        }
        if (lowest >= EncryptedEntropyLimit) {
            encrypted.push_back({index, offset, lowest});
            if (size >= EncryptedLargeSegment) encryptedLarge += size;
        }
    }
    const bool every = loads > 0 && encrypted.size() == loads;
    if (encrypted.empty() || !(every || encryptedLarge * 2 > total)) return std::nullopt;
    std::string listed;
    for (const auto& item : encrypted) {
        char value[32];
        std::snprintf(value, sizeof(value), "%.2f", item.value);
        if (!listed.empty()) listed += ", ";
        listed += "segment " + std::to_string(item.index) + " at " + Hex(item.offset) + " (" + value + " bits per byte)";
    }
    return std::string(every ? "every load segment looks encrypted: " : "most load bytes look encrypted: ") + listed;
}

std::optional<std::string> ElfEncryption(std::span<const std::uint8_t> file) {
    return ElfEncryption(file.size(), [file](std::uint64_t offset, std::uint64_t length) {
        if (offset >= file.size()) return std::vector<std::uint8_t>{};
        const auto end = std::min<std::uint64_t>(file.size(), offset + length);
        return std::vector<std::uint8_t>(file.begin() + static_cast<std::ptrdiff_t>(offset), file.begin() + static_cast<std::ptrdiff_t>(end));
    });
}

}
