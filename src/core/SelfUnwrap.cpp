#include "core/SelfUnwrap.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <utility>

namespace launcher {

namespace {

// Python integers never overflow, and the script adds and subtracts 64-bit fields
// freely, so every quantity here is a 128-bit signed integer.
__extension__ typedef __int128 Int;
__extension__ typedef unsigned __int128 UInt;

constexpr std::uint32_t SelfMagic = 0x1D3D154F;
constexpr std::uint32_t SelfMagicKernel = 0xEEF51454;
constexpr Int RecordOffset = 0x40;
constexpr Int RecordSize = 0x20;
constexpr Int RecordHasSegment = 0x800;
constexpr int RecordSegmentShift = 20;
constexpr Int FirstRegion = 0x400;
constexpr Int MaxAlignmentPadding = Int{64} * 1024 * 1024;
constexpr Int MaxOutputFactor = 4;
constexpr Int MinOutputLimit = Int{256} * 1024 * 1024;
constexpr std::uint32_t PtNull = 0;
constexpr std::uint32_t PtLoad = 1;
constexpr std::uint32_t PtDynamic = 2;
constexpr std::uint32_t PtInterp = 3;
constexpr std::uint32_t PtNote = 4;
constexpr std::uint32_t PtPhdr = 6;
constexpr std::uint32_t PtTls = 7;
constexpr double EntropyLimit = EncryptedEntropyLimit;
constexpr Int EntropySample = static_cast<Int>(EncryptedEntropySample);
constexpr Int EntropyLarge = static_cast<Int>(EncryptedLargeSegment);
constexpr Int EntrySize = 56;

std::string Decimal(Int value) {
    if (value == 0) return "0";
    const bool negative = value < 0;
    UInt magnitude = negative ? static_cast<UInt>(-(value + 1)) + 1 : static_cast<UInt>(value);
    std::string digits;
    while (magnitude > 0) {
        digits.push_back(static_cast<char>('0' + static_cast<int>(magnitude % 10)));
        magnitude /= 10;
    }
    if (negative) digits.push_back('-');
    std::reverse(digits.begin(), digits.end());
    return digits;
}

std::string Hex(Int value) {
    const bool negative = value < 0;
    UInt magnitude = negative ? static_cast<UInt>(-(value + 1)) + 1 : static_cast<UInt>(value);
    std::string digits;
    do {
        digits.push_back("0123456789abcdef"[static_cast<int>(magnitude % 16)]);
        magnitude /= 16;
    } while (magnitude > 0);
    digits += "x0";
    if (negative) digits.push_back('-');
    std::reverse(digits.begin(), digits.end());
    return digits;
}

[[noreturn]] void Refuse(const std::string& message) {
    throw UnwrapRefused(message);
}

Int PyMod(Int value, Int divisor) {
    Int result = value % divisor;
    if (result != 0 && ((result < 0) != (divisor < 0))) result += divisor;
    return result;
}

bool IsPowerOfTwo(Int value) {
    return value != 0 && (value & (value - 1)) == 0;
}

struct Bytes {
    std::span<const std::uint8_t> data;

    Int Size() const { return static_cast<Int>(data.size()); }

    std::uint64_t U64(Int offset) const {
        std::uint64_t value = 0;
        std::memcpy(&value, data.data() + static_cast<std::size_t>(offset), 8);
        return value;
    }
    std::uint32_t U32(Int offset) const {
        std::uint32_t value = 0;
        std::memcpy(&value, data.data() + static_cast<std::size_t>(offset), 4);
        return value;
    }
    std::uint16_t U16(Int offset) const {
        std::uint16_t value = 0;
        std::memcpy(&value, data.data() + static_cast<std::size_t>(offset), 2);
        return value;
    }

    // Python's data[start:end] for non-negative start and end.
    std::span<const std::uint8_t> Slice(Int start, Int end) const {
        start = std::clamp<Int>(start, 0, Size());
        end = std::clamp<Int>(end, 0, Size());
        if (end <= start) return {};
        return data.subspan(static_cast<std::size_t>(start), static_cast<std::size_t>(end - start));
    }
};

struct Record {
    Int flags;
    Int segment;
    Int size;
};

struct Header {
    int index = 0;
    std::uint32_t type = 0;
    Int flags = 0;
    Int offset = 0;
    Int vaddr = 0;
    Int size = 0;
    Int memory = 0;
    Int align = 0;
    Int source = 0;
    Header* host = nullptr;
    Header* container = nullptr;
    Int out = 0;
};

struct Region {
    Int old = 0;
    Int size = 0;
    Int vaddr = 0;
    Int align = 0;
    Int out = 0;
};

double Entropy(std::span<const std::uint8_t> data) {
    if (data.empty()) return 0.0;
    std::size_t counts[256] = {};
    std::uint8_t order[256];
    int distinct = 0;
    for (const auto byte : data) {
        if (counts[byte]++ == 0) order[distinct++] = byte;
    }
    const auto length = static_cast<double>(data.size());
    // CPython 3.12+ sums floats with Neumaier compensation; match it so the 7.5
    // threshold is decided exactly as the script decides it.
    double sum = 0.0;
    double compensation = 0.0;
    for (int index = 0; index < distinct; ++index) {
        const double share = static_cast<double>(counts[order[index]]) / length;
        const double term = share * std::log2(share);
        const double total = sum + term;
        if (std::fabs(sum) >= std::fabs(term)) compensation += (sum - total) + term;
        else compensation += (term - total) + sum;
        sum = total;
    }
    if (compensation != 0.0 && std::isfinite(compensation)) sum += compensation;
    return -sum;
}

std::vector<Record> ReadRecords(const Bytes& data) {
    std::vector<Record> records;
    Int offset = RecordOffset;
    while (offset + RecordSize <= FirstRegion) {
        const Int flags = data.U64(offset);
        const Int segment = data.U64(offset + 8);
        const Int size = data.U64(offset + 16);
        const Int sizeAgain = data.U64(offset + 24);
        if (size == 0 || size != sizeAgain) break;
        if (segment >= data.Size() || segment < 0x350 || segment % 0x10 != 0) break;
        if (size > data.Size()) break;
        records.push_back({flags, segment, size});
        offset += RecordSize;
    }
    return records;
}

struct ParsedElf {
    Int inner = 0;
    std::array<std::uint8_t, 0x40> header{};
    std::vector<std::unique_ptr<Header>> headers;
};

ParsedElf ParseElf(const Bytes& data) {
    static constexpr std::uint8_t magic[] = {0x7F, 'E', 'L', 'F'};
    const auto found = std::search(data.data.begin(), data.data.end(), std::begin(magic), std::end(magic));
    if (found == data.data.end()) Refuse("no inner ELF header");
    ParsedElf parsed;
    parsed.inner = found - data.data.begin();
    if (parsed.inner + 0x40 > data.Size()) Refuse("inner ELF header is truncated");
    std::memcpy(parsed.header.data(), data.data.data() + static_cast<std::size_t>(parsed.inner), 0x40);
    const Bytes header{parsed.header};
    if (parsed.header[4] != 2 || parsed.header[5] != 1) Refuse("inner ELF is not 64-bit little-endian");
    const auto version = header.U32(0x14);
    if (parsed.header[6] != 1 || version != 1) {
        Refuse("unsupported inner ELF version (identification " + Decimal(parsed.header[6]) + ", object " + Decimal(version) + ")");
    }
    const Int programOffset = header.U64(0x20);
    const auto machine = header.U16(0x12);
    if (machine != 62) Refuse("unsupported inner ELF machine " + Decimal(machine) + " (expected 62)");
    const auto headerSize = header.U16(0x34);
    if (headerSize != 64) Refuse("invalid inner ELF header size " + Decimal(headerSize) + " (expected 64)");
    const Int entrySize = header.U16(0x36);
    const Int count = header.U16(0x38);
    if (entrySize != 56 || count == 0) Refuse("program header table is " + Decimal(count) + " entries of " + Decimal(entrySize) + " bytes");
    if (programOffset < 0x40) Refuse("ELF program header table overlaps the ELF header");
    if (parsed.inner + programOffset + count * entrySize > data.Size()) Refuse("ELF program header table does not fit the file");
    for (int index = 0; index < static_cast<int>(count); ++index) {
        const Int offset = parsed.inner + programOffset + index * EntrySize;
        auto entry = std::make_unique<Header>();
        entry->index = index;
        entry->type = data.U32(offset);
        entry->flags = data.U32(offset + 4);
        entry->offset = data.U64(offset + 8);
        entry->vaddr = data.U64(offset + 16);
        entry->size = data.U64(offset + 32);
        entry->memory = data.U64(offset + 40);
        entry->align = data.U64(offset + 48);
        entry->source = entry->offset;
        const auto i = Decimal(index);
        if (entry->type != PtNull && entry->align != 0 && (entry->align & (entry->align - 1)) != 0) Refuse("segment " + i + " alignment is not a power of two");
        if ((entry->type == PtLoad || entry->type == PtTls) && entry->size > entry->memory) Refuse("segment " + i + " file size exceeds memory size");
        if (entry->type == PtTls) {
            if (entry->align > 8192) Refuse("segment " + i + " TLS alignment exceeds 8192");
            if (entry->memory > 0x7FFF0000) Refuse("segment " + i + " TLS memory size exceeds 0x7fff0000");
        }
        if ((entry->type == PtLoad || entry->type == PtTls) && entry->memory > (Int{1} << 64) - entry->vaddr) Refuse("segment " + i + " memory range exceeds ELF64");
        parsed.headers.push_back(std::move(entry));
    }
    return parsed;
}

template <typename Key>
Header* FirstMin(const std::vector<Header*>& items, Key key) {
    Header* best = nullptr;
    for (auto* item : items) {
        if (best == nullptr || key(item) < key(best)) best = item;
    }
    return best;
}

std::vector<std::uint8_t> PlaceSegments(const Bytes& data, std::vector<Header*>& headers, const std::vector<Record>& records) {
    for (auto* header : headers) {
        if (header->type == PtNull) {
            header->flags = header->offset = header->vaddr = header->size = header->memory = header->align = header->source = 0;
        }
    }
    std::vector<Header*> loads;
    for (auto* header : headers) {
        if (header->type == PtLoad) loads.push_back(header);
    }
    std::vector<std::uint8_t> zeroFilled;
    std::set<std::size_t> used;
    std::set<int> placed;
    const Int count = static_cast<Int>(headers.size());
    for (const auto& record : records) {
        const Int target = record.flags >> RecordSegmentShift;
        if ((record.flags & RecordHasSegment) != 0 && target < count && !placed.count(static_cast<int>(target)) &&
            headers[static_cast<std::size_t>(target)]->size == record.size) {
            headers[static_cast<std::size_t>(target)]->source = record.segment;
            placed.insert(static_cast<int>(target));
        }
    }
    std::vector<Header*> recorded;
    for (auto* header : headers) {
        if (placed.count(header->index) && header->vaddr == 0 && header->type != PtLoad) recorded.push_back(header);
    }
    for (auto* header : headers) {
        if (header->type != PtInterp || header->vaddr != 0 || header->size == 0 || placed.count(header->index)) continue;
        std::vector<Header*> containers;
        for (auto* load : loads) {
            if (load->offset <= header->offset && header->offset + header->size <= load->offset + load->size) containers.push_back(load);
        }
        if (!containers.empty()) {
            header->host = FirstMin(containers, [](Header* load) { return load->size; });
            placed.insert(header->index);
        }
    }
    for (auto* header : headers) {
        if (header->type != PtDynamic || header->vaddr != 0 || header->size == 0 || placed.count(header->index)) continue;
        std::vector<Header*> containers;
        for (auto* other : recorded) {
            if (other->offset <= header->offset && header->offset + header->size <= other->offset + other->size) containers.push_back(other);
        }
        if (!containers.empty()) {
            auto* container = FirstMin(containers, [](Header* other) { return other->size; });
            header->source = container->source + header->offset - container->offset;
            header->container = container;
            placed.insert(header->index);
        }
    }
    if (!recorded.empty()) {
        Header* last = nullptr;
        for (auto* header : recorded) {
            if (last == nullptr || header->offset > last->offset) last = header;
        }
        Int cursor = last->source + last->size;
        std::vector<Header*> trailing;
        for (auto* header : headers) {
            if (!placed.count(header->index) && header->vaddr == 0 && header->type != PtLoad && header->size != 0 && header->offset > last->offset) trailing.push_back(header);
        }
        std::stable_sort(trailing.begin(), trailing.end(), [](Header* left, Header* right) { return left->offset < right->offset; });
        for (auto* header : trailing) {
            placed.insert(header->index);
            if (header->type == PtNote) {
                header->source = 0;
                header->size = 0;
                continue;
            }
            header->source = cursor;
            cursor += header->size;
            if (cursor > data.Size()) {
                header->source = 0;
                header->size = 0;
            }
        }
    }
    for (auto* header : loads) {
        if (header->size == 0 || placed.count(header->index)) continue;
        for (std::size_t index = 0; index < records.size(); ++index) {
            const auto& record = records[index];
            if ((record.flags & RecordHasSegment) != 0 || used.count(index)) continue;
            if (record.size == header->size) {
                used.insert(index);
                header->source = record.segment;
                break;
            }
        }
    }
    for (auto* header : loads) {
        if (header->size != 0 && (header->source < 0 || header->source > data.Size() || header->size > data.Size() - header->source)) {
            Refuse("segment " + Decimal(header->index) + " does not fit the file");
        }
    }

    const auto loadFor = [&](Int vaddr, Int size) -> Header* {
        Header* found = nullptr;
        for (auto* header : loads) {
            const Int end = header->vaddr + header->memory;
            if (header->memory != 0 && header->vaddr <= vaddr && (vaddr < end || (size == 0 && vaddr == end))) found = header;
        }
        return found;
    };

    std::optional<std::pair<Int, Int>> tail;
    const Int outputLimit = std::max(MaxOutputFactor * data.Size(), MinOutputLimit);
    for (auto* header : headers) {
        if (header->type == PtLoad) continue;
        if (header->host != nullptr) {
            auto* host = header->host;
            header->source = host->source + header->offset - host->offset;
        }
        if (placed.count(header->index)) {
            if (auto* host = loadFor(header->vaddr, header->size)) {
                const Int offset = header->vaddr - host->vaddr;
                if (host->size != 0 && offset <= host->size && header->size <= host->size - offset && header->source == host->source + offset) header->host = host;
            }
            if (header->size != 0 && (!tail || header->offset > tail->first)) tail = std::make_pair(header->offset, header->source - header->offset);
            continue;
        }
        if (header->vaddr != 0 || header->type == PtPhdr || header->type == PtDynamic || header->type == PtTls) {
            auto* host = loadFor(header->vaddr, header->size);
            if (host == nullptr) {
                if (header->size == 0) {
                    header->source = 0;
                    continue;
                }
                Refuse("segment " + Decimal(header->index) + " vaddr " + Hex(header->vaddr) + " is in no PT_LOAD");
            }
            const Int offset = header->vaddr - host->vaddr;
            if (header->size != 0 && (offset > host->size || header->size > host->size - offset)) {
                if (header->type != PtTls || header->size > host->memory - offset) {
                    Refuse("segment " + Decimal(header->index) + " does not fit the file-backed range of PT_LOAD " + Decimal(host->index));
                }
                if (static_cast<Int>(zeroFilled.size()) + header->size > outputLimit) {
                    Refuse("segment " + Decimal(header->index) + " needs " + Decimal(header->size) + " bytes of output, more than the " + Decimal(outputLimit) +
                           " allowed for a " + Decimal(data.Size()) + " byte container");
                }
                const auto backed = offset < host->size ? data.Slice(host->source + offset, host->source + host->size) : std::span<const std::uint8_t>{};
                header->source = data.Size() + static_cast<Int>(zeroFilled.size());
                zeroFilled.insert(zeroFilled.end(), backed.begin(), backed.end());
                const Int zeros = header->size - static_cast<Int>(backed.size());
                if (zeros < 0) Refuse("segment " + Decimal(header->index) + " has more file bytes than its size");
                zeroFilled.resize(zeroFilled.size() + static_cast<std::size_t>(zeros), 0);
                continue;
            }
            if (host->size == 0 || offset > host->size) {
                header->source = 0;
                continue;
            }
            header->host = host;
            header->source = host->source + offset;
            if (!tail || header->offset > tail->first) tail = std::make_pair(header->offset, header->source - header->offset);
        } else if (header->size != 0) {
            header->source = header->offset + (tail ? tail->second : 0);
            if (header->source > data.Size()) header->source = data.Size();
            const Int keep = std::max<Int>(0, data.Size() - header->source);
            header->size = std::min(header->size, keep);
            header->memory = std::min(header->memory, keep);
        }
    }

    const Int end = data.Size() + static_cast<Int>(zeroFilled.size());
    for (auto* header : headers) {
        if (header->type == PtLoad && header->size == 0) continue;
        if (header->source < 0 || header->source > end || header->size > end - header->source) Refuse("segment " + Decimal(header->index) + " does not fit the file");
    }
    return zeroFilled;
}

double SegmentEntropy(const Bytes& data, const Header& header) {
    const Int start = header.source;
    const Int size = header.size;
    if (size <= EntropySample) return Entropy(data.Slice(start, start + size));
    const Int offsets[] = {0, (size - EntropySample) / 2, size - EntropySample};
    double lowest = 0.0;
    for (int index = 0; index < 3; ++index) {
        const double value = Entropy(data.Slice(start + offsets[index], start + offsets[index] + EntropySample));
        if (index == 0 || value < lowest) lowest = value;
    }
    return lowest;
}

void CheckDecrypted(const Bytes& data, const std::vector<Header*>& headers) {
    struct Encrypted {
        int index;
        Int source;
        double value;
    };
    std::vector<Encrypted> encrypted;
    Int encryptedLarge = 0;
    Int total = 0;
    std::size_t loads = 0;
    for (auto* header : headers) {
        if (header->type != PtLoad || header->size == 0) continue;
        ++loads;
        total += header->size;
        const double value = SegmentEntropy(data, *header);
        if (value >= EntropyLimit) {
            encrypted.push_back({header->index, header->source, value});
            if (header->size >= EntropyLarge) encryptedLarge += header->size;
        }
    }
    const bool every = encrypted.size() == loads;
    if (!encrypted.empty() && (every || encryptedLarge * 2 > total)) {
        std::string listed;
        for (const auto& item : encrypted) {
            char value[32];
            std::snprintf(value, sizeof(value), "%.2f", item.value);
            if (!listed.empty()) listed += ", ";
            listed += "segment " + Decimal(item.index) + " at " + Hex(item.source) + " (" + value + " bits per byte)";
        }
        if (every) Refuse("every load segment looks encrypted: " + listed);
        Refuse("most load bytes look encrypted: " + listed);
    }
}

void PutU64(std::vector<std::uint8_t>& out, Int offset, Int value, const char* what) {
    if (value < 0 || value > static_cast<Int>(UINT64_MAX)) Refuse(std::string("cannot store ") + what + " " + Decimal(value) + " in the output ELF");
    if (offset < 0 || offset + 8 > static_cast<Int>(out.size())) Refuse(std::string("cannot store ") + what + " outside the output");
    const auto raw = static_cast<std::uint64_t>(value);
    std::memcpy(out.data() + static_cast<std::size_t>(offset), &raw, 8);
}

void PutU32(std::vector<std::uint8_t>& out, Int offset, Int value) {
    const auto raw = static_cast<std::uint32_t>(value);
    std::memcpy(out.data() + static_cast<std::size_t>(offset), &raw, 4);
}

void PutU16(std::vector<std::uint8_t>& out, Int offset, std::uint16_t value) {
    std::memcpy(out.data() + static_cast<std::size_t>(offset), &value, 2);
}

}

std::vector<std::uint8_t> UnwrapSelfImage(std::span<const std::uint8_t> input) {
    const Bytes data{input};
    if (data.Size() < FirstRegion) Refuse("file is smaller than the SELF header area");
    const auto magic = data.U32(0);
    if (magic != SelfMagic && magic != SelfMagicKernel) Refuse("not a SELF container (magic " + Hex(magic) + ")");

    const auto records = ReadRecords(data);
    auto parsed = ParseElf(data);
    std::vector<Header*> headers;
    for (auto& header : parsed.headers) headers.push_back(header.get());
    const Int count = static_cast<Int>(headers.size());

    Header* phdr = nullptr;
    for (auto* header : headers) {
        if (header->type != PtPhdr) continue;
        if (phdr != nullptr) Refuse("more than one PT_PHDR segment");
        phdr = header;
    }
    if (phdr != nullptr) {
        if (phdr->size < count * EntrySize || phdr->memory < count * EntrySize) Refuse("PT_PHDR does not cover the program header table");
        for (auto* header : headers) {
            if (header->type == PtLoad && header->index < phdr->index) Refuse("PT_PHDR must precede the load segments");
        }
    }
    const auto zeroFilled = PlaceSegments(data, headers, records);
    const Bytes headerBytes{parsed.header};
    if (phdr != nullptr) {
        if (phdr->host == nullptr) Refuse("PT_PHDR is not in a file-backed load segment");
        const Int table = parsed.inner + headerBytes.U64(0x20);
        const Int tableSize = count * EntrySize;
        const auto mapped = data.Slice(phdr->source, phdr->source + tableSize);
        const auto actual = data.Slice(table, table + tableSize);
        if (!std::equal(mapped.begin(), mapped.end(), actual.begin(), actual.end())) Refuse("PT_PHDR does not map the program header table");
    }
    CheckDecrypted(data, headers);

    std::vector<std::shared_ptr<Region>> regions;
    std::map<int, std::shared_ptr<Region>> loadRegions;
    for (auto* header : headers) {
        if (header->type == PtLoad && header->size != 0) {
            auto region = std::make_shared<Region>(Region{header->source, header->size, header->vaddr, header->align, 0});
            regions.push_back(region);
            loadRegions[header->index] = region;
        }
    }
    const auto regionFor = [&](Int offset, Int size) -> std::shared_ptr<Region> {
        for (const auto& region : regions) {
            if (region->old <= offset && offset + size <= region->old + region->size) return region;
        }
        return nullptr;
    };
    for (auto* header : headers) {
        if (header->type == PtLoad || header->size == 0 || header->container != nullptr) continue;
        if (regionFor(header->source, header->size) == nullptr) {
            regions.push_back(std::make_shared<Region>(Region{header->source, header->size, header->vaddr, header->align, 0}));
        }
    }
    std::stable_sort(regions.begin(), regions.end(), [](const auto& left, const auto& right) { return left->old < right->old; });

    const auto outputRegion = [&](Header* header) -> std::shared_ptr<Region> {
        Header* host = header->type == PtLoad ? header : header->host;
        if (host != nullptr) {
            if (const auto found = loadRegions.find(host->index); found != loadRegions.end()) return found->second;
        }
        return regionFor(header->source, header->size);
    };

    for (auto* header : headers) {
        if (header->size == 0 && (header->type == PtLoad || header->source == 0)) continue;
        const auto region = outputRegion(header);
        const Int alignment = header->align;
        if (region == nullptr || alignment <= 1 || (alignment & (alignment - 1)) != 0) continue;
        const Int current = IsPowerOfTwo(region->align) ? region->align : 1;
        const Int address = header->vaddr - (header->source - region->old);
        if (PyMod(address - region->vaddr, std::min(current, alignment)) != 0) {
            Refuse("segment " + Decimal(header->index) + " has incompatible alignment within its output region");
        }
        if (alignment > current) {
            region->align = alignment;
            region->vaddr = address;
        }
    }

    Int programOffset = headerBytes.U64(0x20);
    Int cursor = std::max(FirstRegion, programOffset + count * EntrySize);
    Int padding = 0;
    for (const auto& region : regions) {
        const Int alignment = IsPowerOfTwo(region->align) ? region->align : 1;
        const Int placedAt = cursor + PyMod(region->vaddr - cursor, alignment);
        padding += placedAt - cursor;
        region->out = placedAt;
        cursor = placedAt + region->size;
    }
    Int emptyEnd = cursor;
    for (auto* header : headers) {
        if (header->type == PtLoad && header->size == 0) emptyEnd = std::max(emptyEnd, PyMod(header->vaddr, std::max<Int>(header->align, 1)));
    }
    padding += emptyEnd - cursor;
    if (padding > MaxAlignmentPadding) Refuse("segment alignment requires more than " + Decimal(MaxAlignmentPadding) + " bytes of output padding");
    cursor = emptyEnd;
    const Int outputLimit = std::max(MaxOutputFactor * data.Size(), MinOutputLimit);
    if (cursor > outputLimit) {
        Refuse("the load segments need " + Decimal(cursor) + " bytes of output, more than the " + Decimal(outputLimit) + " allowed for a " + Decimal(data.Size()) +
               " byte container");
    }

    std::vector<std::uint8_t> result(static_cast<std::size_t>(cursor), 0);
    const Bytes tail{zeroFilled};
    for (const auto& region : regions) {
        const Int start = region->old;
        const Int end = start + region->size;
        const Int target = region->out;
        const Int destinationEnd = std::min(target + region->size, static_cast<Int>(result.size()));
        const Int destinationSize = std::max<Int>(0, destinationEnd - target);
        const auto filePart = data.Slice(start, end);
        if (static_cast<Int>(filePart.size()) > destinationSize) Refuse("segment data at " + Hex(start) + " does not fit its output region");
        std::copy(filePart.begin(), filePart.end(), result.begin() + static_cast<std::ptrdiff_t>(target));
        if (static_cast<Int>(filePart.size()) < destinationSize) {
            const Int tailStart = std::max<Int>(0, start - data.Size());
            const auto tailPart = tail.Slice(tailStart, end - data.Size());
            if (static_cast<Int>(tailPart.size()) != destinationSize - static_cast<Int>(filePart.size())) {
                Refuse("segment data at " + Hex(start) + " does not match its output region");
            }
            std::copy(tailPart.begin(), tailPart.end(), result.begin() + static_cast<std::ptrdiff_t>(target + static_cast<Int>(filePart.size())));
        }
    }

    for (auto* header : headers) {
        if (header->size == 0 && (header->type == PtLoad || header->source == 0)) {
            header->out = header->type == PtLoad ? PyMod(header->vaddr, std::max<Int>(header->align, 1)) : 0;
            continue;
        }
        const auto region = outputRegion(header);
        header->out = region == nullptr ? header->source : region->out + (header->source - region->old);
        if (header->out + header->size > static_cast<Int>(result.size())) Refuse("segment " + Decimal(header->index) + " escaped the output");
    }

    auto elfHeader = parsed.header;
    if (phdr != nullptr) {
        programOffset = phdr->out;
        const auto raw = static_cast<std::uint64_t>(programOffset);
        std::memcpy(elfHeader.data() + 0x20, &raw, 8);
    }
    std::copy(elfHeader.begin(), elfHeader.end(), result.begin());
    PutU64(result, 0x28, 0, "section header offset");
    PutU16(result, 0x3C, 0);
    PutU16(result, 0x3E, 0);
    for (auto* item : headers) {
        const Int at = programOffset + item->index * EntrySize;
        if (at < 0 || at + EntrySize > static_cast<Int>(result.size())) Refuse("program header " + Decimal(item->index) + " falls outside the output");
        PutU32(result, at, item->type);
        PutU32(result, at + 4, item->flags);
        PutU64(result, at + 8, item->out, "segment offset");
        PutU64(result, at + 16, item->vaddr, "segment address");
        PutU64(result, at + 24, item->vaddr, "segment address");
        PutU64(result, at + 32, item->size, "segment size");
        PutU64(result, at + 40, item->memory, "segment memory size");
        PutU64(result, at + 48, item->align, "segment alignment");
    }
    return result;
}

double ByteEntropy(std::span<const std::uint8_t> data) {
    return Entropy(data);
}

}
