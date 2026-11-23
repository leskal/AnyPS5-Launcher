#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace launcher {

enum class ExecutableKind {
    Missing,
    Elf,
    ElfUnsupported,
    ElfEncrypted,  // a readable ELF header over segments that are still encrypted
    Self,
    Unknown,
};

struct ExecutableInfo {
    ExecutableKind kind = ExecutableKind::Missing;
    std::string detail;
};

inline constexpr std::size_t ExecutableHeadSize = 64;

ExecutableInfo ClassifyExecutable(std::span<const std::uint8_t> head);

std::string_view ToString(ExecutableKind kind);

}
