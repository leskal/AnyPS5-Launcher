#pragma once

#include <mutex>
#include <optional>
#include <string>

struct SDL_Window;

namespace launcher {

class FolderPicker {
public:
    void Open(SDL_Window* window, std::string* target);
    // A file instead of a folder; filterName/pattern like "Relinker" / "exe".
    void OpenFile(SDL_Window* window, std::string* target, const char* filterName, const char* pattern);
    void Poll();
    bool IsOpen() const { return target_ != nullptr; }

private:
    static void Callback(void* userdata, const char* const* files, int filter);

    std::string* target_ = nullptr;
    std::mutex mutex_;
    bool finished_ = false;
    std::optional<std::string> chosen_;
};

}
