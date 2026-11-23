#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "core/Job.hpp"

namespace launcher {

// Runs a program to completion with its standard output and error written to
// outputFile, and returns its exit code. Throws Cancelled after stopping the
// program if control is cancelled. The app implements it with SDL; tests fake it.
using ProcessRunner = std::function<int(const std::vector<std::string>& arguments, const std::filesystem::path& workingDirectory,
                                        const std::filesystem::path& outputFile, JobControl& control)>;

}
