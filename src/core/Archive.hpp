#pragma once

#include <filesystem>
#include <string>
#include <string_view>

#include "core/Job.hpp"
#include "core/Scanner.hpp"

namespace launcher {

std::filesystem::path ZarOutputFor(const GameEntry& game);

std::filesystem::path FolderOutputFor(const GameEntry& game);

void PackFolderToZar(const std::filesystem::path& folder, const std::filesystem::path& output, JobControl& control);

void ExtractZarToFolder(const std::filesystem::path& archive, std::string_view innerPrefix, const std::filesystem::path& output, JobControl& control);

bool IsSafeArchiveName(std::string_view name);

}
