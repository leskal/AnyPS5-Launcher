#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace launcher {

// AES-128, enough for the PS4 trophy files: one block encrypted, and CBC decryption.
// Not constant-time; it only reads files the user owns.
class Aes128 {
public:
    explicit Aes128(std::span<const std::uint8_t, 16> key);

    std::array<std::uint8_t, 16> EncryptBlock(std::span<const std::uint8_t, 16> block) const;
    std::array<std::uint8_t, 16> DecryptBlock(std::span<const std::uint8_t, 16> block) const;

    // CBC: data's size must be a multiple of 16 (a trailing partial block is dropped).
    std::vector<std::uint8_t> DecryptCbc(std::span<const std::uint8_t> data, std::span<const std::uint8_t, 16> iv) const;

private:
    std::array<std::uint8_t, 176> roundKeys_{};
};

}
