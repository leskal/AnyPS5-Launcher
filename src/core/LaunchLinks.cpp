#include "core/LaunchLinks.hpp"

#include <algorithm>
#include <cctype>

#include "core/Metadata.hpp"

namespace launcher {

namespace {

std::string Lower(std::string_view text) {
    std::string out(text);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

std::optional<LaunchRequest> Make(std::string_view kind, std::string_view titleId) {
    while (!titleId.empty() && (titleId.back() == '/' || titleId.back() == ' ' || titleId.back() == '\r' || titleId.back() == '\n')) titleId.remove_suffix(1);
    std::string id(titleId);
    std::transform(id.begin(), id.end(), id.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    if (!IsTitleId(id)) return std::nullopt;
    const auto lower = Lower(kind);
    if (lower == "game" || lower == "select") return LaunchRequest{LaunchRequest::Kind::Select, id};
    if (lower == "play") return LaunchRequest{LaunchRequest::Kind::Play, id};
    return std::nullopt;
}

}

std::optional<LaunchRequest> ParseLink(std::string_view link) {
    const auto prefix = std::string(LinkScheme) + "://";
    if (link.size() <= prefix.size() || Lower(link.substr(0, prefix.size())) != prefix) return std::nullopt;
    auto rest = link.substr(prefix.size());
    const auto slash = rest.find('/');
    if (slash == std::string_view::npos) return std::nullopt;
    return Make(rest.substr(0, slash), rest.substr(slash + 1));
}

std::optional<LaunchRequest> ParseArguments(const std::vector<std::string>& arguments) {
    for (std::size_t i = 0; i < arguments.size(); ++i) {
        const auto& argument = arguments[i];
        if (auto request = ParseLink(argument)) return request;
        if ((argument == "--play" || argument == "--game") && i + 1 < arguments.size()) {
            if (auto request = Make(argument.substr(2), arguments[i + 1])) return request;
        }
    }
    return std::nullopt;
}

std::string LinkFor(const LaunchRequest& request) {
    return std::string(LinkScheme) + "://" + (request.kind == LaunchRequest::Kind::Play ? "play/" : "game/") + request.titleId;
}

std::string FormatRequest(const LaunchRequest& request) {
    return std::string(request.kind == LaunchRequest::Kind::Play ? "play " : "game ") + request.titleId + "\n";
}

std::optional<LaunchRequest> ParseRequest(std::string_view line) {
    const auto space = line.find(' ');
    if (space == std::string_view::npos) return std::nullopt;
    return Make(line.substr(0, space), line.substr(space + 1));
}

}
