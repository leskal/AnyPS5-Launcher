#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace launcher {

struct GameMetadata {
    std::string titleId;
    std::string name;
    std::string contentVersion;
    std::string source;
};

bool IsTitleId(std::string_view text);

GameMetadata ParseParamJson(std::span<const std::uint8_t> bytes, std::string_view preferredLocale = {});

GameMetadata ParseParamSfo(std::span<const std::uint8_t> bytes);

}
