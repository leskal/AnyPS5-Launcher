#include "app/FolderPicker.hpp"

#include <string>

#include <SDL3/SDL.h>

namespace launcher {

void FolderPicker::Open(SDL_Window* window, std::string* target) {
    if (target_ != nullptr || target == nullptr) return;
    target_ = target;
    {
        std::lock_guard lock(mutex_);
        finished_ = false;
        chosen_.reset();
    }
    SDL_ShowOpenFolderDialog(&FolderPicker::Callback, this, window, target->empty() ? nullptr : target->c_str(), false);
}

void FolderPicker::OpenFile(SDL_Window* window, std::string* target, const char* filterName, const char* pattern) {
    if (target_ != nullptr || target == nullptr) return;
    target_ = target;
    {
        std::lock_guard lock(mutex_);
        finished_ = false;
        chosen_.reset();
    }
    static SDL_DialogFileFilter filter;
    filter = SDL_DialogFileFilter{filterName, pattern};
    SDL_ShowOpenFileDialog(&FolderPicker::Callback, this, window, pattern != nullptr && pattern[0] != 0 ? &filter : nullptr, pattern != nullptr && pattern[0] != 0 ? 1 : 0,
                           target->empty() ? nullptr : target->c_str(), false);
}

void FolderPicker::SaveFile(SDL_Window* window, std::string* target, const char* filterName, const char* pattern) {
    if (target_ != nullptr || target == nullptr) return;
    target_ = target;
    {
        std::lock_guard lock(mutex_);
        finished_ = false;
        chosen_.reset();
    }
    static SDL_DialogFileFilter filter;
    filter = SDL_DialogFileFilter{filterName, pattern};
    const std::string suggested = *target;
    target->clear();  // stays empty when the dialog is cancelled
    SDL_ShowSaveFileDialog(&FolderPicker::Callback, this, window, &filter, 1, suggested.empty() ? nullptr : suggested.c_str());
}

void FolderPicker::Poll() {
    if (target_ == nullptr) return;
    std::lock_guard lock(mutex_);
    if (!finished_) return;
    if (chosen_) *target_ = *chosen_;
    target_ = nullptr;
}

void FolderPicker::Callback(void* userdata, const char* const* files, int) {
    auto* picker = static_cast<FolderPicker*>(userdata);
    std::lock_guard lock(picker->mutex_);
    if (files != nullptr && files[0] != nullptr) picker->chosen_ = files[0];
    picker->finished_ = true;
}

}
