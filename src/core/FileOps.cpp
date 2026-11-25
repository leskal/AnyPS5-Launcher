#include "core/FileOps.hpp"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <stdexcept>
#include <system_error>
#include <vector>

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
    const std::error_code code(static_cast<int>(error), std::system_category());
    throw std::system_error(code, "cannot move " + ToUtf8(source) + " to " + ToUtf8(target) + ": " + ReadableError(code) + " [Windows error " + std::to_string(error) + "]");
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
    RemoveTree(source);
}

void RemoveTree(const std::filesystem::path& path) {
    std::error_code error;
    for (int attempt = 0; attempt < 20; ++attempt) {
        if (attempt > 0) {
#ifdef _WIN32
            Sleep(150);  // a crashed game, the indexer or antivirus may still hold a file
#endif
            // Read-only files can't be deleted on Windows: make them writable first.
            std::error_code ignored;
            for (auto it = std::filesystem::recursive_directory_iterator(path, ignored); !ignored && it != std::filesystem::recursive_directory_iterator(); it.increment(ignored)) {
                std::filesystem::permissions(it->path(), std::filesystem::perms::owner_write, std::filesystem::perm_options::add, ignored);
                ignored.clear();
            }
            std::filesystem::permissions(path, std::filesystem::perms::owner_write, std::filesystem::perm_options::add, ignored);
        }
        error.clear();
        std::filesystem::remove_all(path, error);
        if (!error) return;
    }
    throw std::system_error(error, "cannot remove " + ToUtf8(path) + ": " + ReadableError(error));
}

bool HoldsCopyOf(const std::filesystem::path& copy, const std::filesystem::path& original) {
    std::error_code error;
    if (!std::filesystem::is_directory(original, error)) {
        if (!std::filesystem::is_regular_file(original, error)) return false;
        if (!std::filesystem::is_regular_file(copy, error)) return false;
        if (std::filesystem::file_size(original, error) != std::filesystem::file_size(copy, error) || error) return false;
        std::ifstream a(original, std::ios::binary), b(copy, std::ios::binary);
        if (!a || !b) return false;
        std::vector<char> x(1 << 16), y(1 << 16);
        while (a && b) {
            a.read(x.data(), static_cast<std::streamsize>(x.size()));
            b.read(y.data(), static_cast<std::streamsize>(y.size()));
            if (a.gcount() != b.gcount() || !std::equal(x.begin(), x.begin() + a.gcount(), y.begin())) return false;
        }
        return a.eof() && b.eof();
    }
    if (!std::filesystem::is_directory(copy, error)) return false;
    for (auto it = std::filesystem::directory_iterator(original, error); !error && it != std::filesystem::directory_iterator(); it.increment(error)) {
        if (!HoldsCopyOf(copy / it->path().filename(), it->path())) return false;
    }
    return !error;
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

namespace {

bool IsUtf8(const std::string& text) {
    for (std::size_t index = 0; index < text.size();) {
        const auto lead = static_cast<unsigned char>(text[index]);
        const std::size_t length = lead < 0x80 ? 1 : (lead & 0xE0) == 0xC0 ? 2 : (lead & 0xF0) == 0xE0 ? 3 : (lead & 0xF8) == 0xF0 ? 4 : 0;
        if (length == 0 || index + length > text.size()) return false;
        for (std::size_t next = 1; next < length; ++next) {
            if ((static_cast<unsigned char>(text[index + next]) & 0xC0) != 0x80) return false;
        }
        index += length;
    }
    return true;
}

}

std::string ReadableText(std::string text) {
    if (IsUtf8(text)) return text;
#ifdef _WIN32
    const int wide = MultiByteToWideChar(CP_ACP, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (wide > 0) {
        std::wstring buffer(static_cast<std::size_t>(wide), L'\0');
        MultiByteToWideChar(CP_ACP, 0, text.data(), static_cast<int>(text.size()), buffer.data(), wide);
        const int bytes = WideCharToMultiByte(CP_UTF8, 0, buffer.data(), wide, nullptr, 0, nullptr, nullptr);
        std::string result(static_cast<std::size_t>(bytes), '\0');
        WideCharToMultiByte(CP_UTF8, 0, buffer.data(), wide, result.data(), bytes, nullptr, nullptr);
        return result;
    }
#endif
    for (auto& c : text) {
        if (static_cast<unsigned char>(c) >= 0x80) c = '?';
    }
    return text;
}

std::string ReadableError(const std::error_code& error) {
#ifdef _WIN32
    if (error.category() == std::system_category()) {
        wchar_t* buffer = nullptr;
        const DWORD length = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr,
                                            static_cast<DWORD>(error.value()), 0, reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
        if (length > 0 && buffer != nullptr) {
            std::wstring text(buffer, length);
            LocalFree(buffer);
            while (!text.empty() && (text.back() == L'\r' || text.back() == L'\n' || text.back() == L' ' || text.back() == L'.')) text.pop_back();
            const int bytes = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
            std::string result(static_cast<std::size_t>(bytes), '\0');
            WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), bytes, nullptr, nullptr);
            return result;
        }
    }
#endif
    return ReadableText(error.message());
}

}
