#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace launcher {

// Reads length bytes at offset of the file being checked.
using RangeReader = std::function<std::vector<std::uint8_t>(std::uint64_t offset, std::uint64_t length)>;

// Whether a plain ELF still holds encrypted code: a dump can carry a readable
// ELF header over segments that were never decrypted. Applies AnyPS5's
// unwrap_self.py rule to the ELF's own load segments: refused when every load
// segment looks encrypted, or when the large ones that do (0x1000 bytes and up)
// hold more than half of the loaded bytes. Returns the reason, or nothing when
// the file looks decrypted or isn't an ELF this can read.
std::optional<std::string> ElfEncryption(std::uint64_t fileSize, const RangeReader& read);

std::optional<std::string> ElfEncryption(std::span<const std::uint8_t> file);

}
