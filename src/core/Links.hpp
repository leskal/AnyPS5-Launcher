#pragma once

#include <filesystem>

namespace launcher {

// Directory links are NTFS junctions on Windows and symbolic links elsewhere.
bool IsDirectoryLink(const std::filesystem::path& path);

// Makes link a directory link to target. An existing link to another place is
// replaced, an empty real folder is replaced, and anything else is refused, so
// no file is ever deleted.
void EnsureDirectoryLink(const std::filesystem::path& link, const std::filesystem::path& target);

// Removes only the link itself. Refuses if path is not a directory link.
void RemoveDirectoryLink(const std::filesystem::path& path);

// Makes link a hard link to target, replacing an older hard link or file at link.
void EnsureHardLink(const std::filesystem::path& link, const std::filesystem::path& target);

// Deletes a tree that the launcher created. Directory links inside it are removed
// as links and never followed.
void RemoveOwnTree(const std::filesystem::path& root);

}
