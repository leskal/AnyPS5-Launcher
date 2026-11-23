#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace launcher {

// The folders the relinker reads modules from: the standard ones, then a title's
// own --module-dir folders, without repeats.
std::vector<std::string> ModuleFolders(const std::vector<std::string>& extra = {});

// "Convert SELF to ELF" keeps each original SELF module as <name>.self.
inline constexpr std::string_view SelfBackupSuffix = ".self";
bool IsSelfBackup(std::string_view name);

// The relinker writes each converted module beside the original as
// <name>.guest.prx.
inline constexpr std::string_view GuestModuleSuffix = ".guest.prx";
bool IsGuestModule(std::string_view name);

enum class ModuleKind {
    NotModule,
    Elf,
    Self,
};

// From a file's first four bytes, the way title_run.py's is_module() decides.
ModuleKind ModuleKindOf(std::span<const std::uint8_t> head);

}
