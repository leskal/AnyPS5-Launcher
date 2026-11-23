#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <vector>

struct SDL_Renderer;
struct SDL_Texture;

namespace launcher {

class IconCache {
public:
    explicit IconCache(SDL_Renderer* renderer);
    ~IconCache();

    IconCache(const IconCache&) = delete;
    IconCache& operator=(const IconCache&) = delete;

    SDL_Texture* Get(const std::filesystem::path& key, const std::vector<std::uint8_t>& png);
    void Clear();

private:
    SDL_Renderer* renderer_;
    std::map<std::filesystem::path, SDL_Texture*> textures_;
};

}
