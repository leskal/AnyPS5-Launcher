#include "core/Modules.hpp"

#include <algorithm>

namespace launcher {

std::vector<std::string> ModuleFolders(const std::vector<std::string>& extra) {
    std::vector<std::string> folders{"sce_module", "sce_modules", "prx"};
    for (const auto& folder : extra) {
        if (std::find(folders.begin(), folders.end(), folder) == folders.end()) folders.push_back(folder);
    }
    return folders;
}

bool IsSelfBackup(std::string_view name) {
    return name.size() > SelfBackupSuffix.size() && name.ends_with(SelfBackupSuffix);
}

bool IsGuestModule(std::string_view name) {
    return name.size() > GuestModuleSuffix.size() && name.ends_with(GuestModuleSuffix);
}

ModuleKind ModuleKindOf(std::span<const std::uint8_t> head) {
    if (head.size() < 4) return ModuleKind::NotModule;
    if (head[0] == 0x7F && head[1] == 'E' && head[2] == 'L' && head[3] == 'F') return ModuleKind::Elf;
    if ((head[0] == 0x4F && head[1] == 0x15 && head[2] == 0x3D && head[3] == 0x1D) || (head[0] == 0x54 && head[1] == 0x14 && head[2] == 0xF5 && head[3] == 0xEE)) {
        return ModuleKind::Self;
    }
    return ModuleKind::NotModule;
}

}
