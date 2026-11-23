#include "core/Links.hpp"

#include <stdexcept>
#include <system_error>
#include <vector>

#include "core/Utf8.hpp"

#ifdef _WIN32
#include "core/WinShim.hpp"
#endif

namespace launcher {

namespace {

#ifdef _WIN32
[[noreturn]] void Fail(unsigned long code, const std::string& what) {
    throw std::system_error(static_cast<int>(code), std::system_category(), what);
}
#endif

bool Exists(const std::filesystem::path& path) {
    std::error_code error;
    return std::filesystem::exists(std::filesystem::symlink_status(path, error));
}

bool PointsTo(const std::filesystem::path& link, const std::filesystem::path& target) {
    std::error_code error;
    return std::filesystem::equivalent(link, target, error) && !error;
}

void CreateDirectoryLink(const std::filesystem::path& link, const std::filesystem::path& target) {
#ifdef _WIN32
    if (const auto code = win::CreateJunction(link.c_str(), std::filesystem::absolute(target).c_str()); code != 0) {
        Fail(code, "cannot link " + ToUtf8(link) + " to " + ToUtf8(target));
    }
#else
    std::error_code error;
    std::filesystem::create_directory_symlink(std::filesystem::absolute(target), link, error);
    if (error) throw std::system_error(error, "cannot link " + ToUtf8(link) + " to " + ToUtf8(target));
#endif
}

}

bool IsDirectoryLink(const std::filesystem::path& path) {
#ifdef _WIN32
    wchar_t target[1024];
    if (win::ReadJunction(path.c_str(), target, 1024) == 0) return true;
#endif
    std::error_code error;
    return std::filesystem::is_symlink(std::filesystem::symlink_status(path, error));
}

void RemoveDirectoryLink(const std::filesystem::path& path) {
#ifdef _WIN32
    wchar_t target[1024];
    if (win::ReadJunction(path.c_str(), target, 1024) == 0) {
        if (const auto code = win::RemoveJunction(path.c_str()); code != 0) Fail(code, "cannot remove the link " + ToUtf8(path));
        return;
    }
#endif
    std::error_code error;
    if (!std::filesystem::is_symlink(std::filesystem::symlink_status(path, error))) throw std::runtime_error(ToUtf8(path) + " is not a link; refusing to remove it");
    std::filesystem::remove(path, error);
    if (error) throw std::system_error(error, "cannot remove the link " + ToUtf8(path));
}

void EnsureDirectoryLink(const std::filesystem::path& link, const std::filesystem::path& target) {
    std::error_code error;
    std::filesystem::create_directories(target, error);
    if (error) throw std::system_error(error, "cannot create " + ToUtf8(target));
    if (Exists(link)) {
        if (IsDirectoryLink(link)) {
            if (PointsTo(link, target)) return;
            RemoveDirectoryLink(link);
        } else if (std::filesystem::is_directory(link, error) && std::filesystem::is_empty(link, error)) {
            std::filesystem::remove(link, error);
            if (error) throw std::system_error(error, "cannot replace the empty folder " + ToUtf8(link));
        } else {
            throw std::runtime_error(ToUtf8(link) + " holds files of its own; move them into " + ToUtf8(target) + " and delete it, then try again");
        }
    }
    std::filesystem::create_directories(link.parent_path(), error);
    CreateDirectoryLink(link, target);
}

void EnsureHardLink(const std::filesystem::path& link, const std::filesystem::path& target) {
    std::error_code error;
    if (Exists(link)) {
        if (PointsTo(link, target)) return;
        if (IsDirectoryLink(link) || std::filesystem::is_directory(std::filesystem::symlink_status(link, error))) {
            throw std::runtime_error(ToUtf8(link) + " is a folder; refusing to replace it with a file link");
        }
        std::filesystem::remove(link, error);
        if (error) throw std::system_error(error, "cannot replace " + ToUtf8(link));
    }
    std::filesystem::create_hard_link(target, link, error);
    if (error) throw std::system_error(error, "cannot hard-link " + ToUtf8(link) + " to " + ToUtf8(target) + " (both must be on the same drive)");
}

void RemoveOwnTree(const std::filesystem::path& root) {
    if (!Exists(root)) return;
    if (IsDirectoryLink(root)) {
        RemoveDirectoryLink(root);
        return;
    }
    std::error_code error;
    if (!std::filesystem::is_directory(std::filesystem::symlink_status(root, error))) {
        std::filesystem::remove(root, error);
        if (error) throw std::system_error(error, "cannot remove " + ToUtf8(root));
        return;
    }
    std::vector<std::filesystem::path> children;
    for (std::filesystem::directory_iterator it(root, error), end; !error && it != end; it.increment(error)) children.push_back(it->path());
    if (error) throw std::system_error(error, "cannot read " + ToUtf8(root));
    for (const auto& child : children) RemoveOwnTree(child);
    std::filesystem::remove(root, error);
    if (error) throw std::system_error(error, "cannot remove " + ToUtf8(root));
}

}
