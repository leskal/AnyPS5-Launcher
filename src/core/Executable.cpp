#include "core/Executable.hpp"

#include <array>
#include <cstdio>

namespace launcher {

namespace {

constexpr std::array<std::uint8_t, 4> ElfMagic{0x7F, 0x45, 0x4C, 0x46};
constexpr std::array<std::uint8_t, 4> SelfMagic{0x4F, 0x15, 0x3D, 0x1D};
constexpr std::array<std::uint8_t, 4> SelfMagicKernel{0x54, 0x14, 0xF5, 0xEE};

constexpr std::uint8_t ElfClass64 = 2;
constexpr std::uint8_t ElfDataLittleEndian = 1;
constexpr std::uint16_t ElfMachineX86_64 = 62;
constexpr std::size_t ElfMachineOffset = 0x12;

bool StartsWith(std::span<const std::uint8_t> head, const std::array<std::uint8_t, 4>& magic) {
    if (head.size() < magic.size()) return false;
    for (std::size_t index = 0; index < magic.size(); ++index) {
        if (head[index] != magic[index]) return false;
    }
    return true;
}

std::string HexBytes(std::span<const std::uint8_t> head) {
    std::string text;
    const auto count = head.size() < 4 ? head.size() : std::size_t{4};
    for (std::size_t index = 0; index < count; ++index) {
        char byte[4];
        std::snprintf(byte, sizeof(byte), index == 0 ? "%02X" : " %02X", head[index]);
        text += byte;
    }
    return text;
}

}

ExecutableInfo ClassifyExecutable(std::span<const std::uint8_t> head) {
    if (StartsWith(head, SelfMagic) || StartsWith(head, SelfMagicKernel)) return {ExecutableKind::Self, {}};
    if (!StartsWith(head, ElfMagic)) {
        if (head.empty()) return {ExecutableKind::Unknown, "empty file"};
        return {ExecutableKind::Unknown, "first bytes " + HexBytes(head)};
    }
    if (head.size() < ElfMachineOffset + 2) return {ExecutableKind::ElfUnsupported, "ELF header is truncated"};
    if (head[4] != ElfClass64) return {ExecutableKind::ElfUnsupported, "ELF is not 64-bit"};
    if (head[5] != ElfDataLittleEndian) return {ExecutableKind::ElfUnsupported, "ELF is not little-endian"};
    const auto machine = static_cast<std::uint16_t>(head[ElfMachineOffset] | (head[ElfMachineOffset + 1] << 8));
    if (machine != ElfMachineX86_64) return {ExecutableKind::ElfUnsupported, "ELF machine " + std::to_string(machine) + " is not x86-64"};
    return {ExecutableKind::Elf, {}};
}

std::string_view ToString(ExecutableKind kind) {
    switch (kind) {
    case ExecutableKind::Missing: return "missing";
    case ExecutableKind::Elf: return "ELF";
    case ExecutableKind::ElfUnsupported: return "ELF (unsupported)";
    case ExecutableKind::ElfEncrypted: return "ELF, still encrypted";
    case ExecutableKind::Self: return "SELF";
    case ExecutableKind::Unknown: return "unknown";
    }
    return "unknown";
}

}
