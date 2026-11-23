#include <algorithm>
#include <chrono>
#include <cstring>
#include <exception>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

#include "Framework.hpp"

int main(int argc, char** argv) {
    const char* filter = argc > 1 ? argv[1] : nullptr;
    int failed = 0;
    int run = 0;
    std::vector<std::pair<long long, std::string>> times;
    const auto start = std::chrono::steady_clock::now();
    for (const auto& testCase : test::Registry()) {
        if (filter != nullptr && std::strstr(testCase.name, filter) == nullptr) continue;
        ++run;
        const auto begin = std::chrono::steady_clock::now();
        std::string verdict = "PASS ";
        std::string detail;
        try {
            testCase.body();
        } catch (const test::Failure& failure) {
            ++failed;
            verdict = "FAIL ";
            detail = failure.message;
        } catch (const std::exception& failure) {
            ++failed;
            verdict = "FAIL ";
            detail = std::string("unexpected exception: ") + failure.what();
        }
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - begin).count();
        times.emplace_back(ms, testCase.name);
        std::cout << verdict << testCase.name << " (" << ms << " ms)\n";
        if (!detail.empty()) std::cout << "  " << detail << '\n';
        std::cout.flush();
    }
    const auto total = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
    std::sort(times.rbegin(), times.rend());
    std::cout << "Slowest:\n";
    for (std::size_t index = 0; index < std::min<std::size_t>(5, times.size()); ++index) {
        std::cout << "  " << times[index].first << " ms  " << times[index].second << '\n';
    }
    std::cout << (run - failed) << " of " << run << " tests passed in " << total << " ms\n";
    return failed == 0 && run > 0 ? 0 : 1;
}
