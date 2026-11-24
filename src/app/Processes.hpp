#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "core/Launch.hpp"
#include "core/Process.hpp"

struct SDL_Process;

namespace launcher {

// ProcessRunner for the relinker: output goes to a file, Cancel kills it.
int RunToFile(const std::vector<std::string>& arguments, const std::filesystem::path& workingDirectory, const std::filesystem::path& outputFile,
              JobControl& control);

// A running game. On Windows the game and everything it starts are kept in a job
// object, so a title that restarts itself is still tracked and Stop ends all of it.
class GameSession {
public:
    GameSession(std::string titleId, std::string name, RunPlan plan);
    ~GameSession();

    GameSession(const GameSession&) = delete;
    GameSession& operator=(const GameSession&) = delete;

    bool Running();
    void Stop();

    const std::string& TitleId() const { return titleId_; }
    const std::string& Name() const { return name_; }
    const RunPlan& Plan() const { return plan_; }
    std::optional<int> ExitCode() const { return exitCode_; }
    bool StopRequested() const { return stopRequested_; }
    // The game's main window in screen pixels (Windows); false when it can't be found.
    bool WindowRect(int& x, int& y, int& width, int& height) const;

private:
    std::string titleId_;
    std::string name_;
    RunPlan plan_;
    SDL_Process* process_ = nullptr;
    void* job_ = nullptr;
    bool mainExited_ = false;
    bool stopRequested_ = false;
    std::optional<int> exitCode_;
};

}
