#include "Fixtures.hpp"
#include "Framework.hpp"

#include "core/Executable.hpp"

using launcher::ClassifyExecutable;
using launcher::ExecutableKind;

TEST(ClassifiesElf64X86) {
    CHECK(ClassifyExecutable(fixture::Elf64()).kind == ExecutableKind::Elf);
}

TEST(ClassifiesBothSelfMagics) {
    CHECK(ClassifyExecutable(fixture::Self()).kind == ExecutableKind::Self);
    std::vector<std::uint8_t> kernel{0x54, 0x14, 0xF5, 0xEE, 0, 0, 0, 0};
    CHECK(ClassifyExecutable(kernel).kind == ExecutableKind::Self);
}

TEST(RejectsElfThatIsNot64BitLittleEndianX86) {
    auto elf32 = fixture::Elf64();
    elf32[4] = 1;
    CHECK(ClassifyExecutable(elf32).kind == ExecutableKind::ElfUnsupported);
    auto bigEndian = fixture::Elf64();
    bigEndian[5] = 2;
    CHECK(ClassifyExecutable(bigEndian).kind == ExecutableKind::ElfUnsupported);
    auto arm = fixture::Elf64();
    arm[0x12] = 183;
    const auto info = ClassifyExecutable(arm);
    CHECK(info.kind == ExecutableKind::ElfUnsupported);
    CHECK_EQ(info.detail, std::string("ELF machine 183 is not x86-64"));
}

TEST(ReportsUnknownBytesInHex) {
    std::vector<std::uint8_t> zip{0x50, 0x4B, 0x03, 0x04, 0x14};
    const auto info = ClassifyExecutable(zip);
    CHECK(info.kind == ExecutableKind::Unknown);
    CHECK_EQ(info.detail, std::string("first bytes 50 4B 03 04"));
    CHECK(ClassifyExecutable({}).kind == ExecutableKind::Unknown);
}

TEST(TruncatedElfIsUnsupported) {
    std::vector<std::uint8_t> head{0x7F, 'E', 'L', 'F', 2, 1};
    CHECK(ClassifyExecutable(head).kind == ExecutableKind::ElfUnsupported);
}
