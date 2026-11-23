#include "core/Unwrap.hpp"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <span>
#include <stdexcept>
#include <system_error>
#include <vector>

#include "core/Executable.hpp"
#include "core/FileOps.hpp"
#include "core/Modules.hpp"
#include "core/SelfUnwrap.hpp"
#include "core/Utf8.hpp"

namespace launcher {

namespace {

std::vector<std::uint8_t> ReadWholeFile(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) throw std::runtime_error("cannot open " + ToUtf8(path));
    std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    if (file.bad()) throw std::runtime_error("cannot read " + ToUtf8(path));
    return bytes;
}

std::vector<std::uint8_t> ReadHead(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    std::vector<std::uint8_t> head(4);
    file.read(reinterpret_cast<char*>(head.data()), 4);
    head.resize(static_cast<std::size_t>(file.gcount()));
    return head;
}

void WriteNew(const std::filesystem::path& file, const std::vector<std::uint8_t>& bytes) {
    std::ofstream stream(file, std::ios::binary | std::ios::trunc);
    stream.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    stream.close();
    if (!stream) throw std::runtime_error("cannot write " + ToUtf8(file));
}

void ConvertModuleInPlace(const std::filesystem::path& module) {
    auto backup = module;
    backup += std::string(SelfBackupSuffix);
    auto temporary = module;
    temporary += ".elf.tmp";
    std::error_code error;
    if (std::filesystem::exists(std::filesystem::symlink_status(backup, error))) {
        throw std::runtime_error(ToUtf8(backup.filename()) + " already exists; remove it or the module to convert it again");
    }
    const auto image = UnwrapSelfImage(ReadWholeFile(module));
    if (ModuleKindOf(std::span(image).first(std::min<std::size_t>(image.size(), 4))) != ModuleKind::Elf) throw std::runtime_error("the unwrapped file isn't an ELF");
    std::filesystem::remove(temporary, error);
    WriteNew(temporary, image);
    try {
        RenameNew(module, backup);
    } catch (...) {
        std::filesystem::remove(temporary, error);
        throw;
    }
    try {
        RenameNew(temporary, module);
    } catch (...) {
        RenameNew(backup, module);
        std::filesystem::remove(temporary, error);
        throw;
    }
}

}

std::filesystem::path UnwrappedEbootFor(const GameEntry& game) {
    return game.location / UnwrappedEbootName;
}

void UnwrapSelf(const std::filesystem::path& input, const std::filesystem::path& output) {
    std::error_code error;
    if (std::filesystem::exists(output, error) && std::filesystem::equivalent(input, output, error)) {
        throw UnwrapRefused("output aliases the input container");
    }
    const auto image = UnwrapSelfImage(ReadWholeFile(input));
    const auto info = ClassifyExecutable(std::span(image).first(std::min<std::size_t>(image.size(), ExecutableHeadSize)));
    if (info.kind != ExecutableKind::Elf) {
        throw std::runtime_error("the unwrapped file isn't a supported ELF: " + std::string(ToString(info.kind)) + (info.detail.empty() ? "" : " (" + info.detail + ")"));
    }
    WriteFileAtomically(output, std::string_view(reinterpret_cast<const char*>(image.data()), image.size()));
}

SelfConversion ConvertGameSelfFiles(const std::filesystem::path& game, const std::vector<std::string>& moduleFolders, const std::vector<std::string>& executables, JobControl& control) {
    struct Item {
        std::filesystem::path path;
        std::string relative;
    };
    std::vector<Item> modules;
    std::error_code error;
    for (const auto& folder : moduleFolders) {
        const auto directory = game / FromUtf8(folder);
        if (!std::filesystem::is_directory(directory, error)) continue;
        std::vector<std::filesystem::path> files;
        for (std::filesystem::directory_iterator it(directory, error), end; !error && it != end; it.increment(error)) {
            if (!it->is_symlink(error) && it->is_regular_file(error)) files.push_back(it->path());
        }
        std::sort(files.begin(), files.end());
        for (const auto& file : files) {
            const auto name = ToUtf8(file.filename());
            if (IsSelfBackup(name) || IsGuestModule(name) || name.ends_with(".elf.tmp")) continue;
            if (ModuleKindOf(ReadHead(file)) == ModuleKind::Self) modules.push_back({file, folder + "/" + name});
        }
    }
    for (const auto& executable : executables) {
        const auto file = game / FromUtf8(executable);
        if (std::filesystem::is_regular_file(file, error) && ModuleKindOf(ReadHead(file)) == ModuleKind::Self) modules.push_back({file, executable});
    }
    const auto eboot = game / "eboot.bin";
    const bool ebootIsSelf = ModuleKindOf(ReadHead(eboot)) == ModuleKind::Self;
    control.SetTotal(static_cast<std::uint64_t>(modules.size()) + (ebootIsSelf ? 1 : 0));

    SelfConversion result;
    if (ebootIsSelf) {
        try {
            UnwrapSelf(eboot, game / UnwrappedEbootName);
            result.eboot = true;
        } catch (const UnwrapRefused& refused) {
            result.refused.push_back("eboot.bin: " + std::string(refused.what()));
        }
        control.Advance(1);
    }
    for (const auto& module : modules) {
        ThrowIfCancelled(control);
        try {
            ConvertModuleInPlace(module.path);
            ++result.modules;
        } catch (const std::exception& failure) {
            result.refused.push_back(module.relative + ": " + failure.what());
        }
        control.Advance(1);
    }
    return result;
}

}
