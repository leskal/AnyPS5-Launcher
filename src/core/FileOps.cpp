#include "core/FileOps.hpp"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <stdexcept>
#include <system_error>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include "core/Utf8.hpp"

namespace launcher {

namespace {

void Move(const std::filesystem::path& source, const std::filesystem::path& target, bool replace) {
#ifdef _WIN32
    const DWORD flags = MOVEFILE_WRITE_THROUGH | (replace ? MOVEFILE_REPLACE_EXISTING : 0);
    DWORD error = ERROR_SUCCESS;
    for (int attempt = 0; attempt < 20; ++attempt) {
        if (MoveFileExW(source.c_str(), target.c_str(), flags)) return;
        error = GetLastError();
        if (error != ERROR_ACCESS_DENIED && error != ERROR_SHARING_VIOLATION) break;
        Sleep(100);
    }
    throw std::system_error(static_cast<int>(error), std::system_category(), "cannot move " + ToUtf8(source) + " to " + ToUtf8(target));
#else
    std::error_code error;
    if (!replace && std::filesystem::exists(target, error)) throw std::runtime_error(ToUtf8(target) + " already exists");
    std::filesystem::rename(source, target, error);
    if (error) throw std::system_error(error, "cannot move " + ToUtf8(source) + " to " + ToUtf8(target));
#endif
}

}

void WriteFileAtomically(const std::filesystem::path& target, std::string_view contents) {
    auto temporary = target;
    temporary += ".tmp";
    {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        if (!file) throw std::runtime_error("cannot write " + ToUtf8(temporary));
        file.write(contents.data(), static_cast<std::streamsize>(contents.size()));
        file.flush();
        if (!file) throw std::runtime_error("cannot write " + ToUtf8(temporary));
    }
    try {
        ReplaceExistingFile(temporary, target);
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        throw;
    }
}

void ReplaceExistingFile(const std::filesystem::path& source, const std::filesystem::path& target) {
    Move(source, target, true);
}

void RenameNew(const std::filesystem::path& source, const std::filesystem::path& target) {
    Move(source, target, false);
}

namespace {

bool IsOtherDrive(const std::system_error& failure) {
#ifdef _WIN32
    if (failure.code().category() == std::system_category() && failure.code().value() == ERROR_NOT_SAME_DEVICE) return true;
#endif
    return failure.code() == std::errc::cross_device_link;
}

}

void MoveTree(const std::filesystem::path& source, const std::filesystem::path& target) {
    std::error_code error;
    if (std::filesystem::exists(std::filesystem::symlink_status(target, error))) throw std::runtime_error(ToUtf8(target) + " already exists");
    try {
        RenameNew(source, target);
        return;
    } catch (const std::system_error& failure) {
        if (!IsOtherDrive(failure)) throw;
    }
    std::filesystem::copy(source, target, std::filesystem::copy_options::recursive, error);
    if (error) {
        std::error_code ignored;
        std::filesystem::remove_all(target, ignored);
        throw std::system_error(error, "cannot copy " + ToUtf8(source) + " to " + ToUtf8(target));
    }
    std::filesystem::remove_all(source, error);
    if (error) throw std::system_error(error, "copied " + ToUtf8(source) + " to " + ToUtf8(target) + " but cannot remove the original");
}

void RequireFreeSpace(const std::filesystem::path& folder, std::uint64_t bytes) {
    std::error_code error;
    const auto space = std::filesystem::space(folder, error);
    if (error) return;
    if (space.available < bytes) {
        throw std::runtime_error("not enough free space: " + FormatBytes(bytes) + " needed, " + FormatBytes(space.available) + " available");
    }
}

std::string FormatBytes(std::uint64_t bytes) {
    const char* units[] = {"bytes", "KiB", "MiB", "GiB", "TiB"};
    auto value = static_cast<double>(bytes);
    std::size_t unit = 0;
    while (value >= 1024.0 && unit + 1 < std::size(units)) {
        value /= 1024.0;
        ++unit;
    }
    char text[32];
    if (unit == 0) std::snprintf(text, sizeof(text), "%llu bytes", static_cast<unsigned long long>(bytes));
    else std::snprintf(text, sizeof(text), "%.1f %s", value, units[unit]);
    return text;
}


}
