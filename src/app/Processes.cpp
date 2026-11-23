#include "app/Processes.hpp"

#include <stdexcept>

#include <SDL3/SDL.h>

#include "core/Utf8.hpp"

#ifdef _WIN32
#include "core/WinShim.hpp"
#endif

namespace launcher {

namespace {

struct Arguments {
    std::vector<const char*> pointers;

    explicit Arguments(const std::vector<std::string>& arguments) {
        for (const auto& argument : arguments) pointers.push_back(argument.c_str());
        pointers.push_back(nullptr);
    }
};

}

int RunToFile(const std::vector<std::string>& arguments, const std::filesystem::path& workingDirectory, const std::filesystem::path& outputFile,
              JobControl& control) {
    if (arguments.empty()) throw std::invalid_argument("RunToFile: no program");
    Arguments argv(arguments);
    SDL_IOStream* output = SDL_IOFromFile(ToUtf8(outputFile).c_str(), "wb");
    if (output == nullptr) throw std::runtime_error("cannot write " + ToUtf8(outputFile) + ": " + SDL_GetError());
    const auto directory = ToUtf8(workingDirectory);
    const SDL_PropertiesID properties = SDL_CreateProperties();
    SDL_SetPointerProperty(properties, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, argv.pointers.data());
    SDL_SetStringProperty(properties, SDL_PROP_PROCESS_CREATE_WORKING_DIRECTORY_STRING, directory.c_str());
    SDL_SetNumberProperty(properties, SDL_PROP_PROCESS_CREATE_STDIN_NUMBER, SDL_PROCESS_STDIO_NULL);
    SDL_SetNumberProperty(properties, SDL_PROP_PROCESS_CREATE_STDOUT_NUMBER, SDL_PROCESS_STDIO_REDIRECT);
    SDL_SetPointerProperty(properties, SDL_PROP_PROCESS_CREATE_STDOUT_POINTER, output);
    SDL_SetBooleanProperty(properties, SDL_PROP_PROCESS_CREATE_STDERR_TO_STDOUT_BOOLEAN, true);
#ifdef _WIN32
    SDL_SetBooleanProperty(properties, SDL_PROP_PROCESS_CREATE_BACKGROUND_BOOLEAN, true);
#endif
    SDL_Process* process = SDL_CreateProcessWithProperties(properties);
    SDL_DestroyProperties(properties);
    SDL_CloseIO(output);
    if (process == nullptr) throw std::runtime_error("cannot start " + arguments.front() + ": " + SDL_GetError());
    int exitCode = -1;
    while (!SDL_WaitProcess(process, false, &exitCode)) {
        if (control.Cancelled()) {
            SDL_KillProcess(process, true);
            SDL_WaitProcess(process, true, &exitCode);
            SDL_DestroyProcess(process);
            throw Cancelled();
        }
        SDL_Delay(50);
    }
    SDL_DestroyProcess(process);
    return exitCode;
}

GameSession::GameSession(std::string titleId, std::string name, RunPlan plan) : titleId_(std::move(titleId)), name_(std::move(name)), plan_(std::move(plan)) {
    const std::vector<std::string> arguments{ToUtf8(plan_.executable)};
    Arguments argv(arguments);
    SDL_Environment* environment = SDL_CreateEnvironment(true);
    if (environment == nullptr) throw std::runtime_error(std::string("cannot prepare the environment: ") + SDL_GetError());
    for (const auto& [name, value] : plan_.environment) SDL_SetEnvironmentVariable(environment, name.c_str(), value.c_str(), true);
    const auto directory = ToUtf8(plan_.workingDirectory);
    const SDL_PropertiesID properties = SDL_CreateProperties();
    SDL_SetPointerProperty(properties, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, argv.pointers.data());
    SDL_SetPointerProperty(properties, SDL_PROP_PROCESS_CREATE_ENVIRONMENT_POINTER, environment);
    SDL_SetStringProperty(properties, SDL_PROP_PROCESS_CREATE_WORKING_DIRECTORY_STRING, directory.c_str());
    SDL_SetNumberProperty(properties, SDL_PROP_PROCESS_CREATE_STDIN_NUMBER, SDL_PROCESS_STDIO_NULL);
    SDL_SetNumberProperty(properties, SDL_PROP_PROCESS_CREATE_STDOUT_NUMBER, SDL_PROCESS_STDIO_NULL);
    SDL_SetNumberProperty(properties, SDL_PROP_PROCESS_CREATE_STDERR_NUMBER, SDL_PROCESS_STDIO_NULL);
#ifdef _WIN32
    SDL_SetBooleanProperty(properties, SDL_PROP_PROCESS_CREATE_BACKGROUND_BOOLEAN, true);
#endif
    process_ = SDL_CreateProcessWithProperties(properties);
    SDL_DestroyProperties(properties);
    SDL_DestroyEnvironment(environment);
    if (process_ == nullptr) throw std::runtime_error("cannot start " + ToUtf8(plan_.executable) + ": " + SDL_GetError());
#ifdef _WIN32
    job_ = win::CreateJob();
    const auto pid = static_cast<unsigned long>(SDL_GetNumberProperty(SDL_GetProcessProperties(process_), SDL_PROP_PROCESS_PID_NUMBER, 0));
    if (job_ != nullptr && (pid == 0 || win::AssignProcessIdToJob(job_, pid) != 0)) {
        win::CloseJob(job_);
        job_ = nullptr;
    }
#endif
}

GameSession::~GameSession() {
    if (process_ != nullptr) SDL_DestroyProcess(process_);
#ifdef _WIN32
    win::CloseJob(job_);
#endif
}

bool GameSession::Running() {
    if (!mainExited_ && process_ != nullptr) {
        int code = 0;
        if (SDL_WaitProcess(process_, false, &code)) {
            mainExited_ = true;
            exitCode_ = code;
        }
    }
#ifdef _WIN32
    if (job_ != nullptr) {
        unsigned long active = 0;
        if (win::JobActiveProcesses(job_, &active) == 0) return active > 0;
    }
#endif
    return !mainExited_;
}

void GameSession::Stop() {
    stopRequested_ = true;
#ifdef _WIN32
    if (job_ != nullptr) win::TerminateJob(job_, 1);
#endif
    if (process_ != nullptr && !mainExited_) SDL_KillProcess(process_, true);
}

}
