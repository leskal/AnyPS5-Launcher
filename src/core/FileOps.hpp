#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace launcher {

void WriteFileAtomically(const std::filesystem::path& target, std::string_view contents);

void ReplaceExistingFile(const std::filesystem::path& source, const std::filesystem::path& target);

void RenameNew(const std::filesystem::path& source, const std::filesystem::path& target);

// Moves a file or folder to a new path. On the same drive this is a rename; to
// another drive the tree is copied and the source deleted once the copy is
// complete. Never overwrites: the target must not exist.
void MoveTree(const std::filesystem::path& source, const std::filesystem::path& target);

void RequireFreeSpace(const std::filesystem::path& folder, std::uint64_t bytes);

std::string FormatBytes(std::uint64_t bytes);

}
