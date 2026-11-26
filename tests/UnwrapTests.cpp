#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>

extern "C" {
#include <sha_256.h>
}

#include "Fixtures.hpp"
#include "Framework.hpp"
#include "UnwrapGolden.hpp"

#include "core/SelfUnwrap.hpp"
#include "core/Unwrap.hpp"

using launcher::UnwrapRefused;
using launcher::UnwrapSelf;
using launcher::UnwrapSelfImage;

namespace {

std::vector<std::uint8_t> FromHex(std::string_view hex) {
    std::vector<std::uint8_t> bytes(hex.size() / 2);
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        bytes[index] = static_cast<std::uint8_t>(std::stoi(std::string(hex.substr(index * 2, 2)), nullptr, 16));
    }
    return bytes;
}

std::string Sha256(const std::vector<std::uint8_t>& bytes) {
    std::uint8_t hash[SIZE_OF_SHA_256_HASH];
    calc_sha_256(hash, bytes.data(), bytes.size());
    std::string text;
    for (const auto byte : hash) {
        char pair[3];
        std::snprintf(pair, sizeof(pair), "%02x", byte);
        text += pair;
    }
    return text;
}

std::vector<std::uint8_t> ReadBytes(const std::filesystem::path& file) {
    std::ifstream stream(file, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

void Write(const std::filesystem::path& file, const std::vector<std::uint8_t>& bytes) {
    std::ofstream(file, std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

std::string Refusal(const std::vector<std::uint8_t>& data) {
    try {
        (void)UnwrapSelfImage(data);
    } catch (const UnwrapRefused& refused) {
        return refused.what();
    }
    return "(not refused)";
}

}

TEST(UnwrapMatchesAnyPS5ScriptOutput) {
    for (const auto& item : golden::cases) {
        const auto image = UnwrapSelfImage(FromHex(item.selfHex));
        std::cout << "  " << item.name << ": " << image.size() << " bytes\n";
        CHECK_EQ(image.size(), item.elfSize);
        CHECK_EQ(Sha256(image), std::string(item.elfSha256));
    }
}

TEST(UnwrapRefusesWithTheScriptsMessages) {
    CHECK_EQ(Refusal(std::vector<std::uint8_t>(0x3FF, 0)), std::string("file is smaller than the SELF header area"));
    auto elf = fixture::Elf64();
    elf.resize(0x1000);
    CHECK_EQ(Refusal(elf), std::string("not a SELF container (magic 0x464c457f)"));
    auto noInner = fixture::Self();
    noInner.resize(0x1000);
    CHECK_EQ(Refusal(noInner), std::string("no inner ELF header"));
    CHECK_EQ(Refusal(FromHex(golden::encrypted_load_hex)), std::string(golden::encryptedMessage));
}

// As AnyPS5's test_metadata_elf_marker_does_not_mask_the_inner_elf: "\x7fELF" in
// the metadata before the inner ELF is passed over.
TEST(UnwrapSkipsAnElfMarkerInTheMetadata) {
    const auto& item = golden::cases[0];
    auto self = FromHex(item.selfHex);
    self[0x100] = 0x7F;
    self[0x101] = 'E';
    self[0x102] = 'L';
    self[0x103] = 'F';
    const auto image = UnwrapSelfImage(self);
    CHECK_EQ(image.size(), item.elfSize);
    CHECK_EQ(Sha256(image), std::string(item.elfSha256));
}

TEST(UnwrapWritesTheFileAndReplacesAnOldOne) {
    fixture::TempDir temp;
    const auto input = temp.Path() / "eboot.bin";
    const auto output = temp.Path() / "eboot.elf";
    Write(input, FromHex(golden::cases[0].selfHex));
    Write(output, fixture::Bytes("old"));
    UnwrapSelf(input, output);
    CHECK_EQ(Sha256(ReadBytes(output)), std::string(golden::cases[0].elfSha256));
    CHECK(!std::filesystem::exists(temp.Path() / "eboot.elf.tmp"));
}

TEST(UnwrapFailureKeepsTheOldFile) {
    fixture::TempDir temp;
    const auto input = temp.Path() / "eboot.bin";
    const auto output = temp.Path() / "eboot.elf";
    auto notSelf = fixture::Elf64();
    notSelf.resize(0x1000);
    Write(input, notSelf);
    Write(output, fixture::Bytes("old"));
    CHECK_THROWS(UnwrapSelf(input, output));
    CHECK(ReadBytes(output) == fixture::Bytes("old"));
    CHECK(!std::filesystem::exists(temp.Path() / "eboot.elf.tmp"));
}

TEST(UnwrapRefusesToOverwriteItsInput) {
    fixture::TempDir temp;
    const auto input = temp.Path() / "eboot.bin";
    Write(input, FromHex(golden::cases[0].selfHex));
    CHECK_THROWS(UnwrapSelf(input, input));
    CHECK(ReadBytes(input) == FromHex(golden::cases[0].selfHex));
}
