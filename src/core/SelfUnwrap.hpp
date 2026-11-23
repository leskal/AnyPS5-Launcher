#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace launcher {

// A C++ port of AnyPS5's tools/unwrap_self.py: it turns a decrypted SELF or fSELF
// container into a plain ELF the relinker accepts, and refuses the same inputs
// with the same messages. Keep it in step with that script.
inline constexpr const char* UnwrapSelfReference = "tools/unwrap_self.py (AnyPS5 514a52b)";

class UnwrapRefused : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

std::vector<std::uint8_t> UnwrapSelfImage(std::span<const std::uint8_t> data);

// unwrap_self.py's test for data that is still encrypted: a load segment whose
// 64 KiB samples at its start, middle and end all reach 7.5 bits per byte.
inline constexpr double EncryptedEntropyLimit = 7.5;
inline constexpr std::uint64_t EncryptedEntropySample = 0x10000;
inline constexpr std::uint64_t EncryptedLargeSegment = 0x1000;

// Shannon entropy in bits per byte, summed exactly as the script sums it.
double ByteEntropy(std::span<const std::uint8_t> data);

}
