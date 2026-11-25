#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace launcher {

void WriteFileAtomically(const std::filesystem::path& target, std::string_view contents);

void ReplaceExistingFile(const std::filesystem::path& source, const std::filesystem::path& target);

void RenameNew(const std::filesystem::path& source, const std::filesystem::path& target);

// Moves a file or folder to a new path. On the same drive this is a rename; to
// another drive the tree is copied and the source deleted once the copy is
// complete. Never overwrites: the target must not exist.
void MoveTree(const std::filesystem::path& source, const std::filesystem::path& target);

// Deletes a file or folder tree, retrying for a few seconds while something still
// holds a file and clearing read-only flags. Throws when it can't.
void RemoveTree(const std::filesystem::path& path);

// True when every file under `original` is also under `copy`, byte for byte
// (`copy` may hold more).
bool HoldsCopyOf(const std::filesystem::path& copy, const std::filesystem::path& original);

void RequireFreeSpace(const std::filesystem::path& folder, std::uint64_t bytes);

std::string FormatBytes(std::uint64_t bytes);

// Windows' own error texts arrive in the system's ANSI code page (Greek, for
// example), not UTF-8. These give text the window can show.
std::string ReadableText(std::string text);
std::string ReadableError(const std::error_code& error);

}
