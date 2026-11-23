#include "core/ZipStore.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <stdexcept>

#include "core/Utf8.hpp"

namespace launcher {

namespace {

constexpr std::uint32_t LocalSignature = 0x04034b50;
constexpr std::uint32_t CentralSignature = 0x02014b50;
constexpr std::uint32_t EndSignature = 0x06054b50;
constexpr std::uint32_t End64Signature = 0x06064b50;
constexpr std::uint32_t Locator64Signature = 0x07064b50;
constexpr std::uint32_t Limit32 = 0xFFFFFFFFu;
constexpr std::uint16_t Utf8Names = 0x0800;
constexpr std::size_t Chunk = 1u << 20;

std::array<std::uint32_t, 256> MakeCrcTable() {
    std::array<std::uint32_t, 256> table{};
    for (std::uint32_t index = 0; index < 256; ++index) {
        std::uint32_t value = index;
        for (int bit = 0; bit < 8; ++bit) value = (value & 1) ? 0xEDB88320u ^ (value >> 1) : value >> 1;
        table[index] = value;
    }
    return table;
}

std::uint32_t Crc32(std::uint32_t crc, std::span<const std::uint8_t> data) {
    static const auto table = MakeCrcTable();
    crc = ~crc;
    for (const auto byte : data) crc = table[(crc ^ byte) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

void Put16(std::vector<std::uint8_t>& out, std::uint16_t value) {
    out.push_back(static_cast<std::uint8_t>(value));
    out.push_back(static_cast<std::uint8_t>(value >> 8));
}

void Put32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8) out.push_back(static_cast<std::uint8_t>(value >> shift));
}

void Put64(std::vector<std::uint8_t>& out, std::uint64_t value) {
    for (int shift = 0; shift < 64; shift += 8) out.push_back(static_cast<std::uint8_t>(value >> shift));
}

std::uint16_t Get16(const std::uint8_t* data) {
    return static_cast<std::uint16_t>(data[0] | (data[1] << 8));
}

std::uint32_t Get32(const std::uint8_t* data) {
    return static_cast<std::uint32_t>(data[0]) | (static_cast<std::uint32_t>(data[1]) << 8) | (static_cast<std::uint32_t>(data[2]) << 16) |
           (static_cast<std::uint32_t>(data[3]) << 24);
}

std::uint64_t Get64(const std::uint8_t* data) {
    return static_cast<std::uint64_t>(Get32(data)) | (static_cast<std::uint64_t>(Get32(data + 4)) << 32);
}

// 1980-01-01 00:00, the zip epoch: the backup's contents, not its time, matter.
constexpr std::uint16_t DosTime = 0;
constexpr std::uint16_t DosDate = (0 << 9) | (1 << 5) | 1;

std::uint32_t Rotate(std::uint32_t value, int count) {
    return (value << count) | (value >> (32 - count));
}

}

Md5::Md5() : state_{0x67452301u, 0xefcdab89u, 0x98badcfeu, 0x10325476u} {}

void Md5::Block(const std::uint8_t* block) {
    static constexpr std::uint32_t K[64] = {
        0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501, 0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122,
        0xfd987193, 0xa679438e, 0x49b40821, 0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8, 0x21e1cde6, 0xc33707d6,
        0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a, 0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60,
        0xbebfbc70, 0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665, 0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039,
        0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1, 0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};
    static constexpr int S[64] = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20,
                                  4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};
    std::uint32_t words[16];
    for (int index = 0; index < 16; ++index) words[index] = Get32(block + index * 4);
    std::uint32_t a = state_[0], b = state_[1], c = state_[2], d = state_[3];
    for (int index = 0; index < 64; ++index) {
        std::uint32_t f;
        int g;
        if (index < 16) {
            f = (b & c) | (~b & d);
            g = index;
        } else if (index < 32) {
            f = (d & b) | (~d & c);
            g = (5 * index + 1) % 16;
        } else if (index < 48) {
            f = b ^ c ^ d;
            g = (3 * index + 5) % 16;
        } else {
            f = c ^ (b | ~d);
            g = (7 * index) % 16;
        }
        const std::uint32_t next = d;
        d = c;
        c = b;
        b = b + Rotate(a + f + K[index] + words[g], S[index]);
        a = next;
    }
    state_[0] += a;
    state_[1] += b;
    state_[2] += c;
    state_[3] += d;
}

void Md5::Update(std::span<const std::uint8_t> data) {
    length_ += data.size();
    std::size_t offset = 0;
    if (used_ > 0) {
        const auto take = std::min(data.size(), buffer_.size() - used_);
        std::memcpy(buffer_.data() + used_, data.data(), take);
        used_ += take;
        offset = take;
        if (used_ < buffer_.size()) return;
        Block(buffer_.data());
        used_ = 0;
    }
    for (; offset + 64 <= data.size(); offset += 64) Block(data.data() + offset);
    used_ = data.size() - offset;
    if (used_ > 0) std::memcpy(buffer_.data(), data.data() + offset, used_);
}

std::string Md5::HexDigest() {
    const std::uint64_t bits = length_ * 8;
    std::uint8_t padding[72] = {0x80};
    const std::size_t pad = used_ < 56 ? 56 - used_ : 120 - used_;
    Update(std::span<const std::uint8_t>(padding, pad));
    std::uint8_t tail[8];
    for (int index = 0; index < 8; ++index) tail[index] = static_cast<std::uint8_t>(bits >> (8 * index));
    Update(std::span<const std::uint8_t>(tail, 8));
    std::string text;
    for (const auto word : state_) {
        for (int index = 0; index < 4; ++index) {
            char pair[3];
            std::snprintf(pair, sizeof(pair), "%02x", static_cast<unsigned>((word >> (8 * index)) & 0xFF));
            text += pair;
        }
    }
    return text;
}

std::string FileMd5(const std::filesystem::path& file, JobControl* control) {
    std::ifstream stream(file, std::ios::binary);
    if (!stream) throw std::runtime_error("cannot open " + ToUtf8(file));
    Md5 md5;
    std::vector<std::uint8_t> buffer(Chunk);
    while (stream) {
        if (control != nullptr) ThrowIfCancelled(*control);
        stream.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
        const auto read = static_cast<std::size_t>(stream.gcount());
        if (read == 0) break;
        md5.Update(std::span<const std::uint8_t>(buffer.data(), read));
    }
    if (stream.bad()) throw std::runtime_error("cannot read " + ToUtf8(file));
    return md5.HexDigest();
}

ZipWriter::ZipWriter(const std::filesystem::path& file) : file_(file), stream_(file, std::ios::binary | std::ios::trunc) {
    if (!stream_) throw std::runtime_error("cannot create " + ToUtf8(file));
}

ZipWriter::~ZipWriter() = default;

void ZipWriter::Begin(const std::string& name, std::uint64_t size, std::uint32_t crc32) {
    if (finished_) throw std::logic_error("zip already finished");
    const auto offset = static_cast<std::uint64_t>(stream_.tellp());
    const bool zip64 = size >= Limit32;
    std::vector<std::uint8_t> header;
    Put32(header, LocalSignature);
    Put16(header, zip64 ? 45 : 20);
    Put16(header, Utf8Names);
    Put16(header, 0);
    Put16(header, DosTime);
    Put16(header, DosDate);
    Put32(header, crc32);
    Put32(header, zip64 ? Limit32 : static_cast<std::uint32_t>(size));
    Put32(header, zip64 ? Limit32 : static_cast<std::uint32_t>(size));
    Put16(header, static_cast<std::uint16_t>(name.size()));
    Put16(header, zip64 ? 20 : 0);
    header.insert(header.end(), name.begin(), name.end());
    if (zip64) {
        Put16(header, 1);
        Put16(header, 16);
        Put64(header, size);
        Put64(header, size);
    }
    stream_.write(reinterpret_cast<const char*>(header.data()), static_cast<std::streamsize>(header.size()));
    entries_.push_back({name, size, crc32, offset});
}

std::string ZipWriter::AddFile(const std::string& name, const std::filesystem::path& source, JobControl& control) {
    // CRC-32 goes in the header before the data, so the file is read twice: once
    // for the sums, once to copy. Originals are at most a few hundred MiB.
    std::uint32_t crc = 0;
    Md5 md5;
    std::uint64_t size = 0;
    std::vector<std::uint8_t> buffer(Chunk);
    {
        std::ifstream input(source, std::ios::binary);
        if (!input) throw std::runtime_error("cannot open " + ToUtf8(source));
        while (input) {
            ThrowIfCancelled(control);
            input.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
            const auto read = static_cast<std::size_t>(input.gcount());
            if (read == 0) break;
            const std::span<const std::uint8_t> piece(buffer.data(), read);
            crc = Crc32(crc, piece);
            md5.Update(piece);
            size += read;
        }
        if (input.bad()) throw std::runtime_error("cannot read " + ToUtf8(source));
    }
    Begin(name, size, crc);
    std::ifstream input(source, std::ios::binary);
    std::uint64_t copied = 0;
    while (copied < size) {
        ThrowIfCancelled(control);
        input.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(std::min<std::uint64_t>(buffer.size(), size - copied)));
        const auto read = static_cast<std::size_t>(input.gcount());
        if (read == 0) throw std::runtime_error(ToUtf8(source) + " changed while it was being backed up");
        stream_.write(reinterpret_cast<const char*>(buffer.data()), static_cast<std::streamsize>(read));
        copied += read;
        control.Advance(read);
    }
    if (!stream_) throw std::runtime_error("cannot write " + ToUtf8(file_));
    return md5.HexDigest();
}

void ZipWriter::AddBytes(const std::string& name, std::span<const std::uint8_t> bytes) {
    Begin(name, bytes.size(), Crc32(0, bytes));
    stream_.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!stream_) throw std::runtime_error("cannot write " + ToUtf8(file_));
}

void ZipWriter::Finish() {
    if (finished_) return;
    const auto directoryOffset = static_cast<std::uint64_t>(stream_.tellp());
    std::vector<std::uint8_t> directory;
    for (const auto& entry : entries_) {
        const bool bigSize = entry.size >= Limit32;
        const bool bigOffset = entry.offset >= Limit32;
        std::vector<std::uint8_t> extra;
        if (bigSize || bigOffset) {
            Put16(extra, 1);
            Put16(extra, static_cast<std::uint16_t>((bigSize ? 16 : 0) + (bigOffset ? 8 : 0)));
            if (bigSize) {
                Put64(extra, entry.size);
                Put64(extra, entry.size);
            }
            if (bigOffset) Put64(extra, entry.offset);
        }
        Put32(directory, CentralSignature);
        Put16(directory, 45);
        Put16(directory, bigSize || bigOffset ? 45 : 20);
        Put16(directory, Utf8Names);
        Put16(directory, 0);
        Put16(directory, DosTime);
        Put16(directory, DosDate);
        Put32(directory, entry.crc32);
        Put32(directory, bigSize ? Limit32 : static_cast<std::uint32_t>(entry.size));
        Put32(directory, bigSize ? Limit32 : static_cast<std::uint32_t>(entry.size));
        Put16(directory, static_cast<std::uint16_t>(entry.name.size()));
        Put16(directory, static_cast<std::uint16_t>(extra.size()));
        Put16(directory, 0);
        Put16(directory, 0);
        Put16(directory, 0);
        Put32(directory, 0);
        Put32(directory, bigOffset ? Limit32 : static_cast<std::uint32_t>(entry.offset));
        directory.insert(directory.end(), entry.name.begin(), entry.name.end());
        directory.insert(directory.end(), extra.begin(), extra.end());
    }
    stream_.write(reinterpret_cast<const char*>(directory.data()), static_cast<std::streamsize>(directory.size()));
    const auto directoryEnd = static_cast<std::uint64_t>(stream_.tellp());
    const bool zip64 = entries_.size() >= 0xFFFF || directoryOffset >= Limit32 || directory.size() >= Limit32;
    std::vector<std::uint8_t> end;
    if (zip64) {
        Put32(end, End64Signature);
        Put64(end, 44);
        Put16(end, 45);
        Put16(end, 45);
        Put32(end, 0);
        Put32(end, 0);
        Put64(end, entries_.size());
        Put64(end, entries_.size());
        Put64(end, directory.size());
        Put64(end, directoryOffset);
        Put32(end, Locator64Signature);
        Put32(end, 0);
        Put64(end, directoryEnd);
        Put32(end, 1);
    }
    Put32(end, EndSignature);
    Put16(end, 0);
    Put16(end, 0);
    Put16(end, static_cast<std::uint16_t>(std::min<std::size_t>(entries_.size(), 0xFFFF)));
    Put16(end, static_cast<std::uint16_t>(std::min<std::size_t>(entries_.size(), 0xFFFF)));
    Put32(end, zip64 ? Limit32 : static_cast<std::uint32_t>(directory.size()));
    Put32(end, zip64 ? Limit32 : static_cast<std::uint32_t>(directoryOffset));
    Put16(end, 0);
    stream_.write(reinterpret_cast<const char*>(end.data()), static_cast<std::streamsize>(end.size()));
    stream_.close();
    if (!stream_) throw std::runtime_error("cannot write " + ToUtf8(file_));
    finished_ = true;
}

ZipReader::ZipReader(const std::filesystem::path& file) : file_(file), stream_(file, std::ios::binary) {
    if (!stream_) throw std::runtime_error("cannot open " + ToUtf8(file));
    stream_.seekg(0, std::ios::end);
    const auto fileSize = static_cast<std::uint64_t>(stream_.tellg());
    const auto tailSize = std::min<std::uint64_t>(fileSize, 0x10000 + 22);
    std::vector<std::uint8_t> tail(static_cast<std::size_t>(tailSize));
    stream_.seekg(static_cast<std::streamoff>(fileSize - tailSize));
    stream_.read(reinterpret_cast<char*>(tail.data()), static_cast<std::streamsize>(tail.size()));
    std::size_t endAt = std::string::npos;
    for (std::size_t index = tail.size() >= 22 ? tail.size() - 22 : 0; tail.size() >= 22; --index) {
        if (Get32(tail.data() + index) == EndSignature) {
            endAt = index;
            break;
        }
        if (index == 0) break;
    }
    const auto bad = [&](const std::string& why) { return std::runtime_error(ToUtf8(file) + " isn't a readable zip: " + why); };
    if (endAt == std::string::npos) throw bad("no end record");
    std::uint64_t count = Get16(tail.data() + endAt + 10);
    std::uint64_t directorySize = Get32(tail.data() + endAt + 12);
    std::uint64_t directoryOffset = Get32(tail.data() + endAt + 16);
    if (endAt >= 20 && Get32(tail.data() + endAt - 20) == Locator64Signature) {
        const auto record = Get64(tail.data() + endAt - 20 + 8);
        std::uint8_t end64[56];
        stream_.seekg(static_cast<std::streamoff>(record));
        stream_.read(reinterpret_cast<char*>(end64), sizeof(end64));
        if (!stream_ || Get32(end64) != End64Signature) throw bad("broken ZIP64 end record");
        count = Get64(end64 + 32);
        directorySize = Get64(end64 + 40);
        directoryOffset = Get64(end64 + 48);
    }
    if (directoryOffset + directorySize > fileSize || directorySize > (64u << 20)) throw bad("central directory out of range");
    std::vector<std::uint8_t> directory(static_cast<std::size_t>(directorySize));
    stream_.seekg(static_cast<std::streamoff>(directoryOffset));
    stream_.read(reinterpret_cast<char*>(directory.data()), static_cast<std::streamsize>(directory.size()));
    if (!stream_) throw bad("central directory unreadable");
    std::size_t at = 0;
    for (std::uint64_t index = 0; index < count; ++index) {
        if (at + 46 > directory.size() || Get32(directory.data() + at) != CentralSignature) throw bad("broken central directory");
        const auto* entry = directory.data() + at;
        const auto method = Get16(entry + 10);
        std::uint64_t compressed = Get32(entry + 20);
        std::uint64_t size = Get32(entry + 24);
        const auto nameLength = Get16(entry + 28);
        const auto extraLength = Get16(entry + 30);
        const auto commentLength = Get16(entry + 32);
        std::uint64_t offset = Get32(entry + 42);
        if (at + 46 + nameLength + extraLength + commentLength > directory.size()) throw bad("broken central directory");
        ZipEntry item;
        item.name.assign(reinterpret_cast<const char*>(entry + 46), nameLength);
        item.crc32 = Get32(entry + 16);
        const auto* extra = entry + 46 + nameLength;
        for (std::size_t field = 0; field + 4 <= extraLength;) {
            const auto id = Get16(extra + field);
            const auto length = Get16(extra + field + 2);
            if (id == 1) {
                std::size_t next = field + 4;
                if (size == Limit32 && next + 8 <= field + 4 + length) size = Get64(extra + next), next += 8;
                if (compressed == Limit32 && next + 8 <= field + 4 + length) compressed = Get64(extra + next), next += 8;
                if (offset == Limit32 && next + 8 <= field + 4 + length) offset = Get64(extra + next), next += 8;
            }
            field += 4 + length;
        }
        if (method != 0 || compressed != size) throw bad(item.name + " is compressed; only stored entries are read");
        std::uint8_t local[30];
        stream_.seekg(static_cast<std::streamoff>(offset));
        stream_.read(reinterpret_cast<char*>(local), sizeof(local));
        if (!stream_ || Get32(local) != LocalSignature) throw bad("broken local header of " + item.name);
        item.size = size;
        item.dataOffset = offset + 30 + Get16(local + 26) + Get16(local + 28);
        if (item.dataOffset + size > fileSize) throw bad(item.name + " runs past the end");
        entries_.push_back(std::move(item));
        at += 46 + nameLength + extraLength + commentLength;
    }
}

const ZipEntry* ZipReader::Find(const std::string& name) const {
    for (const auto& entry : entries_) {
        if (entry.name == name) return &entry;
    }
    return nullptr;
}

std::vector<std::uint8_t> ZipReader::ReadAll(const ZipEntry& entry) {
    if (entry.size > (256u << 20)) throw std::runtime_error(entry.name + " is too large to read into memory");
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(entry.size));
    stream_.clear();
    stream_.seekg(static_cast<std::streamoff>(entry.dataOffset));
    stream_.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!stream_ || Crc32(0, bytes) != entry.crc32) throw std::runtime_error(entry.name + " in " + ToUtf8(file_) + " is damaged");
    return bytes;
}

std::string ZipReader::Md5Of(const ZipEntry& entry, JobControl& control) {
    return Stream(entry, nullptr, control);
}

std::string ZipReader::Extract(const ZipEntry& entry, const std::filesystem::path& target, JobControl& control) {
    std::ofstream output(target, std::ios::binary | std::ios::trunc);
    if (!output) throw std::runtime_error("cannot write " + ToUtf8(target));
    const auto md5 = Stream(entry, &output, control);
    output.close();
    if (!output) throw std::runtime_error("cannot write " + ToUtf8(target));
    return md5;
}

std::string ZipReader::Stream(const ZipEntry& entry, std::ofstream* output, JobControl& control) {
    stream_.clear();
    stream_.seekg(static_cast<std::streamoff>(entry.dataOffset));
    std::vector<std::uint8_t> buffer(Chunk);
    std::uint32_t crc = 0;
    Md5 md5;
    std::uint64_t left = entry.size;
    while (left > 0) {
        ThrowIfCancelled(control);
        stream_.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(std::min<std::uint64_t>(buffer.size(), left)));
        const auto read = static_cast<std::size_t>(stream_.gcount());
        if (read == 0) throw std::runtime_error(entry.name + " in " + ToUtf8(file_) + " is cut short");
        const std::span<const std::uint8_t> piece(buffer.data(), read);
        crc = Crc32(crc, piece);
        md5.Update(piece);
        if (output != nullptr) output->write(reinterpret_cast<const char*>(buffer.data()), static_cast<std::streamsize>(read));
        left -= read;
        control.Advance(read);
    }
    if (crc != entry.crc32) throw std::runtime_error(entry.name + " in " + ToUtf8(file_) + " is damaged (CRC-32 mismatch)");
    return md5.HexDigest();
}

}
