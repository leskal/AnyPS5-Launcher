#include "app/IconCache.hpp"

#include <limits>

#include <SDL3/SDL.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#include <stb_image.h>

namespace launcher {

IconCache::IconCache(SDL_Renderer* renderer) : renderer_(renderer) {}

IconCache::~IconCache() { Clear(); }

SDL_Texture* IconCache::Get(const std::filesystem::path& key, const std::vector<std::uint8_t>& png) {
    if (const auto found = textures_.find(key); found != textures_.end()) return found->second;
    SDL_Texture* texture = nullptr;
    if (!png.empty() && png.size() <= static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        int width = 0;
        int height = 0;
        int channels = 0;
        if (auto* pixels = stbi_load_from_memory(png.data(), static_cast<int>(png.size()), &width, &height, &channels, 4)) {
            texture = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, width, height);
            if (texture != nullptr) {
                SDL_UpdateTexture(texture, nullptr, pixels, width * 4);
                SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_LINEAR);
            }
            stbi_image_free(pixels);
        }
    }
    textures_.emplace(key, texture);
    return texture;
}

void IconCache::Clear() {
    for (auto& [key, texture] : textures_) {
        if (texture != nullptr) SDL_DestroyTexture(texture);
    }
    textures_.clear();
}

}
