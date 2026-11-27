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

// A system_error whose message is only ours: std::system_error adds the system's own
// text in the ANSI code page, which shows as garbage next to UTF-8 (Greek, say).
class FileError : public std::system_error {
public:
    FileError(std::error_code code, std::string text) : std::system_error(code), text_(std::move(text)) {}
    const char* what() const noexcept override { return text_.c_str(); }

private:
    std::string text_;
};

void Move(const std::filesystem::path& source, const std::filesystem::path& target, bool replace) {
#ifdef _WIN32
    const DWORD flags = MOVEFILE_WRITE_THROUGH | (replace ? MOVEFILE_REPLACE_EXISTING : 0);
    DWORD error = ERROR_SUCCESS;
    // Access denied right after writing a folder is usually the antivirus or the search
    // indexer still reading the new files: wait for it, up to about forty seconds.
    for (int attempt = 0; attempt < 45; ++attempt) {
        if (MoveFileExW(source.c_str(), target.c_str(), flags)) return;
        error = GetLastError();
        if (error != ERROR_ACCESS_DENIED && error != ERROR_SHARING_VIOLATION) break;
        Sleep(static_cast<DWORD>(std::min(100 * (attempt + 1), 1000)));
    }
    const std::error_code code(static_cast<int>(error), std::system_category());
    std::string text = "cannot move " + ToUtf8(source) + " to " + ToUtf8(target) + ": " + ReadableError(code) + " [Windows error " + std::to_string(error) + "]";
    if (error == ERROR_ACCESS_DENIED || error == ERROR_SHARING_VIOLATION) {
        text += ". Something still has a file in it open (often the antivirus scanning new files, or an Explorer window on it); close that and try again";
    }
    throw FileError(code, std::move(text));
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
    throw FileError(error, "cannot remove " + ToUtf8(path) + ": " + ReadableError(error));
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

namespace {

// The length of the UTF-8 sequence at text[at], or 0 when it isn't one.
std::size_t Utf8SequenceAt(std::string_view text, std::size_t at) {
    const auto byte = [&](std::size_t i) { return static_cast<unsigned char>(text[i]); };
    const unsigned char lead = byte(at);
    if (lead < 0x80) return 1;
    std::size_t length = 0;
    if (lead >= 0xC2 && lead <= 0xDF) length = 2;
    else if (lead >= 0xE0 && lead <= 0xEF) length = 3;
    else if (lead >= 0xF0 && lead <= 0xF4) length = 4;
    else return 0;
    if (at + length > text.size()) return 0;
    for (std::size_t i = 1; i < length; ++i) {
        if ((byte(at + i) & 0xC0) != 0x80) return 0;
    }
    return length;
}

std::string FromAnsi(std::string_view bytes) {
#ifdef _WIN32
    const int wide = MultiByteToWideChar(CP_ACP, 0, bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
    if (wide > 0) {
        std::wstring buffer(static_cast<std::size_t>(wide), L'\0');
        MultiByteToWideChar(CP_ACP, 0, bytes.data(), static_cast<int>(bytes.size()), buffer.data(), wide);
        const int size = WideCharToMultiByte(CP_UTF8, 0, buffer.data(), wide, nullptr, 0, nullptr, nullptr);
        std::string result(static_cast<std::size_t>(size), '\0');
        WideCharToMultiByte(CP_UTF8, 0, buffer.data(), wide, result.data(), size, nullptr, nullptr);
        return result;
    }
#endif
    return std::string(bytes.size(), '?');
}

}

// UTF-8 stays as it is; only the runs that aren't UTF-8 are read as the ANSI code page.
// (A message can be both: our UTF-8 text with the system's ANSI text after it.)
std::string ReadableText(std::string text) {
    if (IsUtf8(text)) return text;
    std::string result;
    std::size_t at = 0;
    std::size_t runStart = std::string::npos;
    while (at < text.size()) {
        const auto length = Utf8SequenceAt(text, at);
        if (length == 0) {
            if (runStart == std::string::npos) runStart = at;
            ++at;
            continue;
        }
        if (runStart != std::string::npos) {
            result += FromAnsi(std::string_view(text).substr(runStart, at - runStart));
            runStart = std::string::npos;
        }
        result.append(text, at, length);
        at += length;
    }
    if (runStart != std::string::npos) result += FromAnsi(std::string_view(text).substr(runStart));
    return result;
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
