#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace launcher {

// anyps5://game/<title ID> selects a game, anyps5://play/<title ID> starts it.
// The same as command-line arguments: --game <ID>, --play <ID>, or the link itself.
struct LaunchRequest {
    enum class Kind { Select, Play };
    Kind kind = Kind::Select;
    std::string titleId;

    bool operator==(const LaunchRequest&) const = default;
};

inline constexpr const char* LinkScheme = "anyps5";

std::optional<LaunchRequest> ParseLink(std::string_view link);
std::optional<LaunchRequest> ParseArguments(const std::vector<std::string>& arguments);
std::string LinkFor(const LaunchRequest& request);

// One line, "play PPSA01234": how a second launcher hands a request to the first.
std::string FormatRequest(const LaunchRequest& request);
std::optional<LaunchRequest> ParseRequest(std::string_view line);

}
