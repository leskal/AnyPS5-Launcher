#pragma once

#include <filesystem>
#include <functional>
#include <string>

#include "core/Job.hpp"

namespace launcher {

// Downloads url into file: written to <file>.partial and renamed when complete.
// Progress is added to the job; its total is set to the size the server sends
// when the job has none yet. Follows
// redirects (GitHub release assets redirect to their storage host). Throws on a
// non-2xx status, on a network error, or when cancelled.
using Downloader = std::function<void(const std::string& url, const std::filesystem::path& file, JobControl& control)>;

void HttpDownload(const std::string& url, const std::filesystem::path& file, JobControl& control);

// A small text response, such as a GitHub API reply. At most 8 MiB.
using TextFetcher = std::function<std::string(const std::string& url)>;

std::string HttpGetText(const std::string& url);

// POSTs body with the given Content-Type and returns the (small) answer.
using TextPoster = std::function<std::string(const std::string& url, const std::string& body, const std::string& contentType)>;

std::string HttpPostText(const std::string& url, const std::string& body, const std::string& contentType);

}
