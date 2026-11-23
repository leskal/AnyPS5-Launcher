#include <algorithm>
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_sdlrenderer3.h>
#include <imgui_stdlib.h>

#include "app/BackgroundJob.hpp"
#include "app/FolderPicker.hpp"
#include "app/IconCache.hpp"
#include "app/Processes.hpp"
#include "core/AnyPS5.hpp"
#include "core/Archive.hpp"
#include "core/FileOps.hpp"
#include "core/Install.hpp"
#include "core/Launch.hpp"
#include "core/Modules.hpp"
#include "core/Profiles.hpp"
#include "core/Scanner.hpp"
#include "core/Settings.hpp"
#include "core/Unwrap.hpp"
#include "core/Utf8.hpp"
#include "core/Versions.hpp"

#ifdef _WIN32
#include "core/WinShim.hpp"
#endif

namespace {

using namespace launcher;

constexpr ImVec4 Green{0.55f, 0.85f, 0.55f, 1.0f};
constexpr ImVec4 Amber{0.95f, 0.80f, 0.40f, 1.0f};
constexpr ImVec4 Red{0.95f, 0.45f, 0.45f, 1.0f};
constexpr ImVec4 Dim{0.60f, 0.60f, 0.65f, 1.0f};

template <typename Result>
class Background {
public:
    template <typename Work>
    void Start(Work work) {
        if (running_) return;
        if (worker_.joinable()) worker_.join();
        running_ = true;
        worker_ = std::thread([this, work = std::move(work)]() mutable {
            auto result = work();
            std::lock_guard lock(mutex_);
            finished_ = std::move(result);
            running_ = false;
        });
    }

    std::optional<Result> Take() {
        std::lock_guard lock(mutex_);
        auto result = std::move(finished_);
        finished_.reset();
        return result;
    }

    bool Running() const { return running_; }

    ~Background() {
        if (worker_.joinable()) worker_.join();
    }

private:
    std::thread worker_;
    std::atomic<bool> running_ = false;
    std::mutex mutex_;
    std::optional<Result> finished_;
};

struct ReleaseList {
    std::string repository;
    std::vector<Release> releases;
    std::string error;
};

struct AnyPS5State {
    AnyPS5Probe probe;
    std::string version;
    std::string versionError;
    std::map<std::string, TitleEntry> titles;
    std::string titlesError;
};

AnyPS5State LoadAnyPS5(const std::string& folder, const std::filesystem::path& scratch) {
    AnyPS5State state;
    state.probe = ProbeAnyPS5(folder);
    if (!state.probe.runtime) return state;
    try {
        state.version = QueryRelinkerVersion(*state.probe.runtime, RunToFile, scratch);
    } catch (const std::exception& failure) {
        state.versionError = failure.what();
    }
    if (state.probe.runtime->titleCatalog) {
        try {
            state.titles = LoadTitleCatalog(*state.probe.runtime->titleCatalog);
        } catch (const std::exception& failure) {
            state.titlesError = failure.what();
        }
    }
    return state;
}

void LoadFonts(ImGuiIO& io) {
#ifdef _WIN32
    std::string fonts = "C:\\Windows\\Fonts\\";
    if (const char* windows = SDL_getenv("WINDIR")) fonts = std::string(windows) + "\\Fonts\\";
    const char* candidates[] = {"segoeui.ttf", "YuGothM.ttc", "msyh.ttc", "malgun.ttf", "seguisym.ttf"};
#else
    std::string fonts = "/usr/share/fonts/";
    const char* candidates[] = {"truetype/dejavu/DejaVuSans.ttf", "opentype/noto/NotoSansCJK-Regular.ttc", "truetype/noto/NotoSans-Regular.ttf"};
#endif
    bool loaded = false;
    for (const char* name : candidates) {
        const auto path = fonts + name;
        if (!std::filesystem::is_regular_file(FromUtf8(path))) continue;
        ImFontConfig config;
        config.MergeMode = loaded;
        if (io.Fonts->AddFontFromFileTTF(path.c_str(), 17.0f, &config) != nullptr) loaded = true;
    }
    if (!loaded) io.Fonts->AddFontDefault();
}

bool PathExists(const std::filesystem::path& path) {
    std::error_code error;
    return std::filesystem::exists(std::filesystem::symlink_status(path, error));
}

std::string FileName(const std::filesystem::path& path) {
    return ToUtf8(path.filename());
}

void OpenFolder(const std::filesystem::path& folder) {
    std::error_code error;
    std::filesystem::create_directories(folder, error);
#ifdef _WIN32
    win::OpenFolder(folder.c_str());
#else
    SDL_OpenURL(("file://" + ToUtf8(std::filesystem::absolute(folder))).c_str());
#endif
}

FolderPicker& Picker() {
    static FolderPicker picker;
    return picker;
}

bool MenuAction(const char* label, const std::string& disabledReason, const std::string& tooltip) {
    const bool enabled = disabledReason.empty();
    const bool chosen = ImGui::MenuItem(label, nullptr, false, enabled);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled | ImGuiHoveredFlags_DelayShort)) {
        ImGui::SetTooltip("%s", enabled ? tooltip.c_str() : disabledReason.c_str());
    }
    return chosen;
}

std::string LanguageLabel(const std::string& language) {
    if (language.empty()) return "Not set (English, United States)";
    if (language == "auto" || language == "AUTO" || language == "Auto") return "Same as Windows";
    for (const auto& item : ConsoleLanguages()) {
        if (std::to_string(item.number) == language) return std::to_string(item.number) + " " + item.name;
    }
    return language;
}

struct Message {
    Message(ImVec4 color, std::string title, std::string text) : color(color), title(ReadableText(std::move(title))), text(ReadableText(std::move(text))) {}

    ImVec4 color;
    std::string title;
    std::string text;
};

class LauncherApp {
public:
    LauncherApp(SDL_Window* window, SDL_Renderer* renderer, float scale)
        : window_(window), icons_(std::make_unique<IconCache>(renderer)), scale_(scale) {
        const char* base = SDL_GetBasePath();
        launcherFolder_ = FromUtf8(base != nullptr ? base : "");
        settingsFile_ = launcherFolder_ / SettingsFileName;
        auto loaded = LoadSettings(settingsFile_);
        settings_ = loaded.settings;
        settingsError_ = loaded.error;
        versionsRoot_ = VersionsRoot(launcherFolder_);
        versions_ = LoadVersions(versionsRoot_);
        AdoptConfiguredFolder();
        ApplySettings();
        if (!loaded.existed || settings_.gamesFolder.empty()) OpenSettings();
        else StartScan();
    }

    ~LauncherApp() {
        if (session_) session_->Stop();
    }

    void Frame() {
        Picker().Poll();
        if (auto finished = scan_.Take()) {
            icons_->Clear();
            result_ = std::move(*finished);
            scanned_ = true;
            RecoverSessions();
            RefreshInstalls();
        }
        if (auto state = anyps5Loader_.Take(); state && !anyps5Pending_) {
            anyps5_ = std::move(*state);
            RefreshInstalls();
            if (!anyps5_.probe.runtime && !missingAsked_) {
                missingAsked_ = true;
                openMissing_ = true;
            }
        }
        if (auto list = releasesLoader_.Take()) {
            releases_ = std::move(*list);
            if (latestWanted_) {
                latestWanted_ = false;
                if (!releases_->error.empty()) {
                    message_ = Message{Red, "Can't look up AnyPS5 releases", releases_->error};
                } else if (auto newest = NewestRelease(releases_->releases)) {
                    StartDownload(releases_->repository, *newest);
                } else {
                    message_ = Message{Red, "No usable AnyPS5 release", "none of " + releases_->repository + "'s releases has a relinker and runtime for this system"};
                }
            }
        }
        if (!localPick_.empty() && !Picker().IsOpen()) {
            const auto picked = localPick_;
            localPick_.clear();
            try {
                auto entry = LocalVersion(FromUtf8(picked));
                const auto id = entry.id;
                PutVersion(versions_, std::move(entry));
                SaveVersions(versionsRoot_, versions_);
                SelectVersion(id);
                versionsError_.clear();
            } catch (const std::exception& failure) {
                versionsError_ = failure.what();
                message_ = Message{Red, "Can't add that AnyPS5", failure.what()};
            }
        }
        if (anyps5Pending_ && !anyps5Loader_.Running()) {
            anyps5Pending_ = false;
            const auto folder = settings_.anyps5Folder;
            const auto scratch = layout_.library / "logs" / "relinker-version.txt";
            anyps5Loader_.Start([folder, scratch] { return LoadAnyPS5(folder, scratch); });
        }
        if (auto outcome = job_.TakeOutcome()) {
            if (outcome->succeeded) rescanWanted_ = true;
            if (downloaded_) {
                if (outcome->succeeded && *downloaded_) {
                    const auto id = (*downloaded_)->id;
                    PutVersion(versions_, **downloaded_);
                    try {
                        SaveVersions(versionsRoot_, versions_);
                    } catch (const std::exception&) {
                    }
                    SelectVersion(id);
                }
                downloaded_.reset();
            }
            message_ = Message{outcome->succeeded ? Green : outcome->cancelled ? Amber : Red, outcome->title, outcome->message};
            jobSerial_.clear();
        }
        if (session_ && !session_->Running()) FinishSession();
        if (rescanWanted_ && !scan_.Running()) {
            rescanWanted_ = false;
            StartScan();
        }

        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->WorkPos);
        ImGui::SetNextWindowSize(viewport->WorkSize);
        ImGui::Begin("Games", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
        DrawTopBar();
        DrawGameTable();
        DrawFooter();
        DrawNewProfile();
        DrawMissingRelinker();
        DrawVersions();
        DrawSettings();
        ImGui::End();
    }

private:
    // ---- state -------------------------------------------------------------------

    void ApplySettings() {
        layout_.library = EffectiveLibrary(settings_, launcherFolder_);
        layout_.userdata = EffectiveUserdata(settings_, launcherFolder_);
        profiles_ = ListProfiles(layout_.userdata);
        layout_.profile = settings_.profile;
        if (!IsValidProfileName(layout_.profile)) layout_.profile = profiles_.empty() ? DefaultProfileName : profiles_.front();
        LoadCurrentProfile();
        anyps5_ = {};
        anyps5_.probe.problem = "checking...";
        anyps5Pending_ = true;
    }

    void LoadCurrentProfile() {
        profileError_.clear();
        try {
            profileSettings_ = LoadProfile(layout_);
        } catch (const std::exception& failure) {
            profileSettings_ = {};
            profileError_ = failure.what();
        }
    }

    void SwitchProfile(const std::string& name) {
        if (session_) return;
        try {
            layout_.profile = name;
            CreateProfile(layout_);
            settings_.profile = name;
            SaveSettings(settingsFile_, settings_);
            profiles_ = ListProfiles(layout_.userdata);
            LoadCurrentProfile();
        } catch (const std::exception& failure) {
            message_ = Message{Red, "Profile", failure.what()};
        }
    }

    void RefreshInstalls() {
        installs_.clear();
        lastRuns_.clear();
        for (const auto& game : result_.games) {
            const auto& serial = game.metadata.titleId;
            if (!game.home.empty()) {
                if (auto manifest = ReadInstallManifest(game.home)) installs_.emplace(ToUtf8(game.location), std::move(*manifest));
            }
            if (!lastRuns_.count(serial)) {
                if (auto report = LatestRunReport(layout_, serial)) lastRuns_.emplace(serial, std::move(*report));
            }
        }
    }

    // Saves left beside an exe because the launcher or the PC stopped during a game.
    void RecoverSessions() {
        if (session_) return;
        std::string recovered;
        for (const auto& game : result_.games) {
            if (game.home.empty() || !HasSession(game.home)) continue;
            try {
                MoveOutSession(game.home);
                recovered += (recovered.empty() ? "" : ", ") + game.metadata.name;
            } catch (const std::exception& failure) {
                message_ = Message{Red, "Saves of " + game.metadata.name + " are still in " + ToUtf8(game.home), failure.what()};
                return;
            }
        }
        if (!recovered.empty()) message_ = Message{Amber, "Moved saves back to their profile", "after an unfinished run of " + recovered};
    }

    void StartScan() {
        if (settings_.gamesFolder.empty()) return;
        ScanOptions options;
        options.roots.push_back(FromUtf8(settings_.gamesFolder));
        options.excluded = {layout_.library, layout_.userdata};
        scan_.Start([options] {
            ScanResult result;
            try {
                result = Scan(options);
            } catch (const std::exception& failure) {
                result.problems.push_back({options.roots.front(), failure.what()});
            }
            return result;
        });
    }

    const TitleEntry* TitleFor(const std::string& serial) const {
        const auto found = anyps5_.titles.find(serial);
        return found == anyps5_.titles.end() ? nullptr : &found->second;
    }

    const InstallManifest* InstallFor(const GameEntry& game) const {
        const auto found = installs_.find(ToUtf8(game.location));
        return found == installs_.end() ? nullptr : &found->second;
    }

    bool IsRunning(const GameEntry& game) const {
        return session_ && session_->TitleId() == game.metadata.titleId;
    }

    // ---- actions -----------------------------------------------------------------

    std::string InstallBlocker(const GameEntry& game) const {
        if (job_.Busy()) return "Another task is running";
        if (IsRunning(game)) return "Stop the game first";
        if (anyps5Pending_ || anyps5Loader_.Running()) return "Still checking the AnyPS5 folder";
        if (!anyps5_.probe.runtime) return "AnyPS5: " + anyps5_.probe.problem;
        if (game.format == PackageFormat::Zar && game.layout == ZarLayout::Nested) return "This .zar keeps the game inside a folder: convert it to a folder first";
        if (game.executable.kind == ExecutableKind::Missing) return "eboot.bin is missing";
        const bool usableEboot = game.unwrapped && game.unwrapped->kind == ExecutableKind::Elf;
        if (game.executable.kind == ExecutableKind::ElfEncrypted && !usableEboot) return "eboot.bin is still encrypted: use a decrypted dump of the game";
        if (game.format == PackageFormat::Folder) {
            const bool ebootElf = game.executable.kind == ExecutableKind::Elf || (game.unwrapped && game.unwrapped->kind == ExecutableKind::Elf);
            if (!ebootElf || game.modules.self > 0) return "Some files are still SELF: choose Convert SELF to ELF first";
        }
        if (game.format == PackageFormat::Zar && game.home.empty() && PathExists(HomeFor(game))) return FileName(HomeFor(game)) + " already exists next to the archive";
        return {};
    }

    std::string PlayBlocker(const GameEntry& game) const {
        if (session_) return session_->TitleId() == game.metadata.titleId ? "Already running" : "Another game is running";
        if (jobSerial_ == game.metadata.titleId) return "Being installed";
        if (InstallFor(game) == nullptr) return "Install it first";
        return {};
    }

    void StartInstall(const GameEntry& game) {
        const auto& runtime = *anyps5_.probe.runtime;
        const auto* title = TitleFor(game.metadata.titleId);
        std::optional<TitleEntry> titleCopy;
        if (title != nullptr) titleCopy = *title;
        jobSerial_ = game.metadata.titleId;
        job_.Start("Install " + game.metadata.name, true,
                   [game, runtime, layout = layout_, titleCopy, version = anyps5_.version](JobControl& control) {
                       const auto manifest = InstallGame({game, runtime, layout, titleCopy ? &*titleCopy : nullptr, version, TargetIsWindows, HostIsIntel()}, RunToFile, control);
                       std::string text = "Converted with AnyPS5";
                       if (!manifest.relinkerVersion.empty()) text += " " + manifest.relinkerVersion;
                       const auto count = [](int n, const char* word) { return std::to_string(n) + " " + word + (n == 1 ? "" : "s"); };
                       text += "; " + count(manifest.modules, "module");
                       if (!manifest.addedLibraries.empty()) {
                           std::string names;
                           for (const auto& name : manifest.addedLibraries) names += (names.empty() ? "" : ", ") + name;
                           text += "; copied " + names + " into AnyPS5's libs folder (once)";
                       }
                       return text;
                   });
    }

    void StartRemoveInstall(const GameEntry& game) {
        jobSerial_ = game.metadata.titleId;
        job_.Start("Remove install: " + game.metadata.name, false, [game](JobControl&) {
            const auto result = RemoveInstall(game);
            std::string text = "Restored " + ToUtf8(result.restored);
            if (!result.leftovers.empty()) {
                std::string names;
                for (const auto& name : result.leftovers) names += (names.empty() ? "" : ", ") + name;
                text += ". Left in " + ToUtf8(game.home) + " because the launcher didn't make them: " + names;
            }
            return text;
        });
    }

    void Play(const GameEntry& game) {
        try {
            auto plan = PrepareRun(game, layout_, profileSettings_, TitleFor(game.metadata.titleId));
            session_ = std::make_unique<GameSession>(game.metadata.titleId, game.metadata.name, std::move(plan));
            message_ = Message{Green, "Playing " + game.metadata.name, "Profile " + layout_.profile};
        } catch (const std::exception& failure) {
            message_ = Message{Red, "Can't start " + game.metadata.name, failure.what()};
        }
    }

    void FinishSession() {
        std::optional<RunReport> report;
        try {
            report = FinishRun(session_->Plan());
        } catch (const std::exception& failure) {
            message_ = Message{Red, "Saves of " + session_->Name() + " are still beside its exe", std::string(failure.what()) + ". The launcher tries again at its next start."};
            session_.reset();
            return;
        }
        const auto& serial = session_->TitleId();
        if (report) {
            lastRuns_[serial] = *report;
            message_ = Message{report->exit == "clean" ? Green : Amber, session_->Name(), report->summary};
        } else if (session_->StopRequested()) {
            message_ = Message{Dim, session_->Name(), "stopped"};
        } else {
            const auto code = session_->ExitCode();
            message_ = Message{Amber, session_->Name(),
                               "ended" + (code ? " with exit code " + std::to_string(*code) : std::string()) + " without a run report; see " + ToUtf8(session_->Plan().logFile)};
        }
        session_.reset();
    }

    void StartConvertSelf(const GameEntry& game) {
        const auto folder = game.location;
        const auto zip = OriginalsZipFor(game);
        const auto* title = TitleFor(game.metadata.titleId);
        const auto moduleFolders = ModuleFolders(title != nullptr ? ModuleDirectories(*title) : std::vector<std::string>{});
        const auto executables = title != nullptr ? title->executables : std::vector<std::string>{};
        job_.Start("SELF to ELF: " + game.metadata.name, true, [folder, zip, moduleFolders, executables](JobControl& control) {
            const auto result = ConvertGameSelfFiles(folder, zip, moduleFolders, executables, control);
            std::vector<std::string> done;
            if (result.eboot) done.push_back("eboot.bin");
            if (result.modules > 0) done.push_back(std::to_string(result.modules) + (result.modules == 1 ? " module" : " modules"));
            std::string text;
            for (const auto& part : done) text += (text.empty() ? "Converted " : " and ") + part;
            if (result.migrated > 0) text += (text.empty() ? "Moved " : "; moved ") + std::to_string(result.migrated) + " older .self backups into the zip";
            std::string refused;
            for (const auto& item : result.refused) refused += (refused.empty() ? "" : "; ") + item;
            if (text.empty()) throw std::runtime_error(refused.empty() ? "nothing to convert" : "nothing converted: " + refused);
            text += ". Originals with MD5 in " + ToUtf8(zip);
            if (!refused.empty()) text += ". Not converted: " + refused;
            return text;
        });
    }

    void StartRestoreOriginals(const GameEntry& game) {
        const auto folder = game.location;
        const auto zip = OriginalsZipFor(game);
        job_.Start("Restore originals: " + game.metadata.name, true, [folder, zip](JobControl& control) {
            const int count = RestoreOriginals(folder, zip, control);
            return "Restored " + std::to_string(count) + (count == 1 ? " file" : " files") + ", each matching its MD5; deleted " + ToUtf8(zip.filename());
        });
    }

    void StartExtract(const GameEntry& game, std::filesystem::path output) {
        const auto archive = game.location;
        const auto prefix = game.innerPrefix;
        job_.Start(".zar to folder: " + game.metadata.name, true, [=](JobControl& control) {
            ExtractZarToFolder(archive, prefix, output, control);
            return "Extracted to " + ToUtf8(output);
        });
    }

    void StartPack(const GameEntry& game, std::filesystem::path output) {
        const auto folder = game.location;
        job_.Start("Folder to .zar: " + game.metadata.name, true, [=](JobControl& control) {
            PackFolderToZar(folder, output, control);
            return "Packed into " + ToUtf8(output);
        });
    }

    // ---- drawing -----------------------------------------------------------------

    void DrawTopBar() {
        if (!settingsError_.empty()) ImGui::TextColored(Red, "Settings: %s", settingsError_.c_str());
        if (!profileError_.empty()) ImGui::TextColored(Red, "Profile: %s", profileError_.c_str());

        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(Dim, "Profile");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12);
        ImGui::BeginDisabled(session_ != nullptr);
        if (ImGui::BeginCombo("##profile", layout_.profile.c_str())) {
            for (const auto& name : profiles_) {
                if (ImGui::Selectable(name.c_str(), name == layout_.profile)) SwitchProfile(name);
            }
            if (std::find(profiles_.begin(), profiles_.end(), layout_.profile) == profiles_.end()) {
                ImGui::Selectable(layout_.profile.c_str(), true);
            }
            ImGui::Separator();
            if (ImGui::Selectable("New profile...")) {
                newProfileName_.clear();
                newProfileError_.clear();
                openNewProfile_ = true;
            }
            ImGui::EndCombo();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::TextColored(Dim, "Games");
        ImGui::SameLine();
        ImGui::TextUnformatted(settings_.gamesFolder.empty() ? "(not set)" : settings_.gamesFolder.c_str());

        const auto& style = ImGui::GetStyle();
        const float buttons = ImGui::CalcTextSize("AnyPS5 versions").x + ImGui::CalcTextSize("Rescan").x + ImGui::CalcTextSize("Settings").x + style.FramePadding.x * 6 +
                              style.ItemSpacing.x * 2;
        ImGui::SameLine(ImGui::GetContentRegionMax().x - buttons);
        ImGui::BeginDisabled(session_ != nullptr);
        if (ImGui::Button("AnyPS5 versions")) OpenVersions();
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(scan_.Running() || job_.Busy() || settings_.gamesFolder.empty());
        if (ImGui::Button("Rescan")) StartScan();
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(session_ != nullptr || job_.Busy());
        if (ImGui::Button("Settings")) OpenSettings();
        ImGui::EndDisabled();

        if (anyps5Loader_.Running() || anyps5Pending_) {
            ImGui::TextColored(Dim, "Checking AnyPS5...");
        } else if (!anyps5_.probe.runtime) {
            ImGui::TextColored(Amber, "AnyPS5: %s", anyps5_.probe.problem.c_str());
        }
        if (scan_.Running()) {
            ImGui::TextUnformatted("Scanning...");
        } else if (scanned_) {
            ImGui::Text("%zu games, %zu installed, %zu problems", result_.games.size(), installs_.size(), result_.problems.size());
        }
        if (!result_.problems.empty() && ImGui::CollapsingHeader("Problems")) {
            for (const auto& problem : result_.problems) {
                ImGui::TextColored(Red, "%s", ToUtf8(problem.location).c_str());
                ImGui::SameLine();
                ImGui::TextWrapped("%s", problem.message.c_str());
            }
        }
    }

    int FooterLines() const {
        return std::max(1, (job_.Busy() ? 1 : 0) + (session_ ? 1 : 0) + (!job_.Busy() && !session_ && message_ ? 1 : 0));
    }

    float FooterHeight() const {
        return ImGui::GetFrameHeightWithSpacing() * static_cast<float>(FooterLines()) + ImGui::GetStyle().ItemSpacing.y;
    }

    void DrawGameTable() {
        const auto groups = GroupByTitleId(result_.games);
        const float iconSize = 64.0f * scale_;
        constexpr ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_Resizable |
                                          ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp;
        if (!ImGui::BeginTable("games", 7, flags, ImVec2(0, -FooterHeight()))) return;
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, iconSize);
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 2.6f);
        ImGui::TableSetupColumn("Serial", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("Version", ImGuiTableColumnFlags_WidthStretch, 0.9f);
        ImGui::TableSetupColumn("Format", ImGuiTableColumnFlags_WidthStretch, 1.1f);
        ImGui::TableSetupColumn("eboot", ImGuiTableColumnFlags_WidthStretch, 1.2f);
        ImGui::TableSetupColumn("AnyPS5", ImGuiTableColumnFlags_WidthStretch, 2.4f);
        ImGui::TableHeadersRow();
        for (const auto& game : result_.games) {
            ImGui::PushID(ToUtf8(game.location).c_str());
            ImGui::TableNextRow(ImGuiTableRowFlags_None, iconSize);
            ImGui::TableNextColumn();
            if (auto* texture = icons_->Get(game.location, game.icon)) {
                ImGui::Image(static_cast<ImTextureID>(reinterpret_cast<std::intptr_t>(texture)), ImVec2(iconSize, iconSize));
            } else {
                ImGui::Dummy(ImVec2(iconSize, iconSize));
            }
            ImGui::TableNextColumn();
            ImGui::Selectable(game.metadata.name.c_str(), IsRunning(game), ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap, ImVec2(0, iconSize));
            if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                if (PlayBlocker(game).empty()) Play(game);
                else if (InstallFor(game) == nullptr && InstallBlocker(game).empty()) StartInstall(game);
            }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) ImGui::SetTooltip("%s\nDouble-click to play, right-click for actions", ToUtf8(game.location).c_str());
            DrawContextMenu(game);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(game.metadata.titleId.c_str());
            if (groups.at(game.metadata.titleId).size() > 1) ImGui::TextColored(Amber, "duplicate");
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(game.metadata.contentVersion.c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(ToString(game.format).data());
            if (game.format == PackageFormat::Zar) ImGui::TextColored(game.layout == ZarLayout::Nested ? Amber : Dim, "%s", ToString(game.layout).data());
            ImGui::TableNextColumn();
            DrawExecutable(game);
            ImGui::TableNextColumn();
            DrawStatus(game);
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    static void DrawExecutable(const GameEntry& game) {
        const auto& info = game.executable;
        const ImVec4 color = info.kind == ExecutableKind::Elf ? Green : info.kind == ExecutableKind::Self ? Amber : Red;
        ImGui::TextColored(color, "eboot.bin: %s", ToString(info.kind).data());
        if (!info.detail.empty()) {
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextColored(Dim, "%s", info.detail.c_str());
            ImGui::PopTextWrapPos();
        }
        if (game.unwrapped) {
            const bool good = game.unwrapped->kind == ExecutableKind::Elf;
            ImGui::TextColored(good ? Green : Red, "eboot.elf: %s", ToString(game.unwrapped->kind).data());
        }
        if (game.modules.elf + game.modules.self > 0) {
            if (game.modules.self == 0) ImGui::TextColored(Green, "modules: %d ELF", game.modules.elf);
            else ImGui::TextColored(Amber, "modules: %d ELF, %d SELF", game.modules.elf, game.modules.self);
        }
    }

    void DrawStatus(const GameEntry& game) {
        const auto& serial = game.metadata.titleId;
        if (IsRunning(game)) {
            ImGui::TextColored(Green, "Running");
        } else if (jobSerial_ == serial && job_.Busy()) {
            ImGui::TextColored(Amber, "Installing...");
        } else if (const auto* manifest = InstallFor(game)) {
            const auto reason = ReinstallReason(game, *manifest, anyps5_.version);
            if (reason.empty()) ImGui::TextColored(Green, "Installed %s", manifest->installedAt.substr(0, 10).c_str());
            else ImGui::TextColored(Amber, "Reinstall: %s", reason.c_str());
        } else {
            ImGui::TextColored(Dim, "Not installed");
        }
        if (const auto run = lastRuns_.find(serial); run != lastRuns_.end() && !IsRunning(game)) {
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextColored(run->second.exit == "clean" ? Dim : Amber, "Last run: %s", run->second.summary.c_str());
            ImGui::PopTextWrapPos();
        }
    }

    void DrawContextMenu(const GameEntry& game) {
        if (!ImGui::BeginPopupContextItem("actions")) return;
        ImGui::TextColored(Dim, "%s", game.metadata.name.c_str());
        ImGui::Separator();
        const auto& serial = game.metadata.titleId;
        const auto* manifest = InstallFor(game);

        if (IsRunning(game)) {
            if (ImGui::MenuItem("Stop")) session_->Stop();
        } else if (MenuAction("Play", PlayBlocker(game), "Runs the converted game with profile " + layout_.profile)) {
            Play(game);
        }
        {
            const auto home = HomeFor(game);
            std::string how;
            if (!game.home.empty()) how = "Relinks the game again in " + ToUtf8(home) + ".";
            else if (game.format == PackageFormat::Folder && !game.app0Parent.empty()) how = "The game already sits in " + ToUtf8(game.location) + "; nothing is moved.";
            else if (game.format == PackageFormat::Folder) how = "Moves the game's files into " + ToUtf8(home / "app0") + " (a rename, nothing is copied), then relinks.";
            else how = "Moves the .zar unchanged to " + ToUtf8(home / "app0.zar") + " and extracts only eboot, sce_sys and the modules into " +
                       ToUtf8(home / "app0") + ", then relinks.";
            if (MenuAction(manifest != nullptr ? "Reinstall with AnyPS5" : "Install with AnyPS5", InstallBlocker(game),
                           how + " The exe goes into " + ToUtf8(home) + "; the relinker writes <module>.guest.prx beside each module. Nothing is linked.")) {
                StartInstall(game);
            }
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Open saves folder")) OpenFolder(layout_.Saves(serial));
        if (ImGui::MenuItem("Open logs folder")) OpenFolder(layout_.Logs(serial));
        if (!game.home.empty() && ImGui::MenuItem("Open game folder")) OpenFolder(game.home);
        {
            std::string reason = job_.Busy() ? "Another task is running" : IsRunning(game) ? "Stop the game first" : "";
            if (reason.empty() && game.home.empty()) reason = "Not set up for AnyPS5";
            const std::string what = game.format == PackageFormat::Zar ? "moves the .zar back to " + ToUtf8(game.home.parent_path() / "<its old name>") + " and deletes the extracted files"
                                                                       : "moves the game's files out of app0 back into " + ToUtf8(game.home);
            if (MenuAction("Remove install", reason, "Deletes the exe and the relinker's outputs, " + what + ". Saves stay in the profile.")) StartRemoveInstall(game);
        }
        ImGui::Separator();

        const std::string busy = job_.Busy() ? "Another task is running" : IsRunning(game) ? "Stop the game first" : "";
        {
            std::string reason = busy;
            if (reason.empty() && game.format != PackageFormat::Folder) reason = "The files are inside the archive; Install unwraps them while extracting";
            if (reason.empty() && game.executable.kind != ExecutableKind::Self && game.modules.self == 0) reason = "eboot.bin and the modules are already ELF";
            std::string label = "Convert SELF to ELF";
            if (game.executable.kind == ExecutableKind::Self && game.modules.self > 0) label += " (eboot.bin and " + std::to_string(game.modules.self) + " modules)";
            else if (game.modules.self > 0) label += " (" + std::to_string(game.modules.self) + (game.modules.self == 1 ? " module)" : " modules)");
            else if (game.executable.kind == ExecutableKind::Self) label += " (eboot.bin)";
            if (MenuAction(label.c_str(), reason,
                           "First stores the originals in " + FileName(OriginalsZipFor(game)) + " beside the game folder, with their MD5, and checks the zip.\n"
                           "Then replaces eboot.bin, the SELF modules (sce_module, sce_modules, prx, the title's module folders) and extra executables with their ELF.\n"
                           "Same conversion as AnyPS5's unwrap_self.py. Install needs this done first.")) {
                StartConvertSelf(game);
            }
        }
        {
            const auto zip = OriginalsZipFor(game);
            std::string reason = busy;
            if (reason.empty() && game.format != PackageFormat::Folder) reason = "Only for game folders";
            if (reason.empty() && !PathExists(zip)) reason = "No " + FileName(zip) + " beside the game folder";
            if (MenuAction("Restore original SELF files", reason, "Puts every file from " + FileName(zip) + " back, each checked against its MD5, then deletes the zip.")) {
                StartRestoreOriginals(game);
            }
        }
        {
            const auto output = FolderOutputFor(game);
            std::string reason = busy;
            if (reason.empty() && game.format != PackageFormat::Zar) reason = "This game is already a folder";
            if (reason.empty() && !game.home.empty()) reason = "Remove the install first";
            if (reason.empty() && PathExists(output)) reason = FileName(output) + " already exists next to the archive";
            if (MenuAction("Convert .zar to folder", reason, "Extracts to " + ToUtf8(output) + ". The .zar is kept.")) StartExtract(game, output);
        }
        {
            const auto output = ZarOutputFor(game);
            std::string reason = busy;
            if (reason.empty() && game.format != PackageFormat::Folder) reason = "This game is already a .zar";
            if (reason.empty() && !game.home.empty()) reason = "Remove the install first";
            if (reason.empty() && PathExists(output)) reason = FileName(output) + " already exists next to the folder";
            if (MenuAction("Convert folder to .zar", reason, "Packs into " + ToUtf8(output) + ". The folder is kept.")) StartPack(game, output);
        }
        ImGui::EndPopup();
    }

    void DrawFooter() {
        ImGui::Separator();
        const auto& style = ImGui::GetStyle();
        bool drew = false;
        if (job_.Busy()) {
            drew = true;
            auto& control = job_.Control();
            const auto total = control.Total();
            const auto done = control.Done();
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(job_.Title().c_str());
            ImGui::SameLine();
            const float cancelWidth = job_.Cancellable() ? ImGui::CalcTextSize("Cancel").x + style.FramePadding.x * 2 + style.ItemSpacing.x : 0.0f;
            if (total > 0) {
                const bool bytes = total != 1000;
                const std::string label = bytes ? FormatBytes(done) + " / " + FormatBytes(total) : std::to_string(done / 10) + "%";
                ImGui::ProgressBar(static_cast<float>(static_cast<double>(done) / static_cast<double>(total)), ImVec2(-cancelWidth, 0), label.c_str());
            } else {
                ImGui::ProgressBar(-1.0f * static_cast<float>(ImGui::GetTime()), ImVec2(-cancelWidth, 0), "Working...");
            }
            if (job_.Cancellable()) {
                ImGui::SameLine();
                ImGui::BeginDisabled(control.Cancelled());
                if (ImGui::Button("Cancel")) control.Cancel();
                ImGui::EndDisabled();
            }
        }
        if (session_) {
            drew = true;
            ImGui::AlignTextToFramePadding();
            ImGui::TextColored(Green, "Playing %s", session_->Name().c_str());
            ImGui::SameLine();
            ImGui::TextColored(Dim, "profile %s", layout_.profile.c_str());
            ImGui::SameLine(ImGui::GetContentRegionMax().x - ImGui::CalcTextSize("Stop").x - style.FramePadding.x * 2);
            if (ImGui::Button("Stop")) session_->Stop();
        }
        if (!drew && message_) {
            ImGui::AlignTextToFramePadding();
            ImGui::TextColored(message_->color, "%s:", message_->title.c_str());
            ImGui::SameLine();
            ImGui::TextUnformatted(message_->text.c_str());
            ImGui::SameLine(ImGui::GetContentRegionMax().x - ImGui::GetFrameHeight());
            if (ImGui::Button("x", ImVec2(ImGui::GetFrameHeight(), 0))) message_.reset();
        } else if (!drew) {
            ImGui::AlignTextToFramePadding();
            ImGui::TextColored(Dim, "Double-click a game to play it; right-click for install and conversions.");
        }
    }

    void DrawNewProfile() {
        if (openNewProfile_) {
            ImGui::OpenPopup("New profile");
            openNewProfile_ = false;
        }
        if (!ImGui::BeginPopupModal("New profile", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) return;
        ImGui::TextUnformatted("Each profile has its own saves, trophies, input mapping and screenshots.");
        ImGui::TextColored(Dim, "Games still show the player name \"Player\": AnyPS5 doesn't let it be changed yet.");
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 18);
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        const bool submit = ImGui::InputTextWithHint("##name", "profile name", &newProfileName_, ImGuiInputTextFlags_EnterReturnsTrue);
        if (!newProfileError_.empty()) ImGui::TextColored(Red, "%s", newProfileError_.c_str());
        if (ImGui::Button("Create") || submit) {
            if (!IsValidProfileName(newProfileName_)) {
                newProfileError_ = "Use a name that can be a folder name (no \\ / : * ? \" < > |)";
            } else if (std::find(profiles_.begin(), profiles_.end(), newProfileName_) != profiles_.end()) {
                newProfileError_ = "That profile already exists";
            } else {
                SwitchProfile(newProfileName_);
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    // ---- AnyPS5 versions ----------------------------------------------------------

    // A folder set in an older launcher, or by hand, becomes a local version.
    void AdoptConfiguredFolder() {
        if (settings_.anyps5Folder.empty()) return;
        for (const auto& entry : versions_.versions) {
            if (ToUtf8(entry.folder) == settings_.anyps5Folder) {
                if (settings_.anyps5Version != entry.id) settings_.anyps5Version = entry.id;
                return;
            }
        }
        VersionEntry entry;
        entry.id = "local:" + settings_.anyps5Folder;
        entry.folder = FromUtf8(settings_.anyps5Folder);
        settings_.anyps5Version = entry.id;
        PutVersion(versions_, std::move(entry));
        try {
            SaveVersions(versionsRoot_, versions_);
        } catch (const std::exception&) {
        }
    }

    std::string CurrentVersionLabel() const {
        if (const auto* entry = FindVersion(versions_, settings_.anyps5Version)) return entry->Label();
        return settings_.anyps5Folder.empty() ? std::string("none") : "local: " + settings_.anyps5Folder;
    }

    void SelectVersion(const std::string& id) {
        const auto* entry = FindVersion(versions_, id);
        if (entry == nullptr || session_) return;
        settings_.anyps5Version = id;
        settings_.anyps5Folder = ToUtf8(entry->folder);
        try {
            SaveSettings(settingsFile_, settings_);
        } catch (const std::exception& failure) {
            message_ = Message{Red, "Settings", failure.what()};
        }
        missingAsked_ = true;
        ApplySettings();
        message_ = Message{Green, "AnyPS5 " + entry->Label(), "Installed games converted with another version show \"Reinstall\"."};
    }

    void OpenVersions() {
        versionProbes_.clear();
        versionsError_.clear();
        openVersions_ = true;
    }

    void LoadReleases(const std::string& repository, bool downloadNewest) {
        if (releasesLoader_.Running()) return;
        latestWanted_ = downloadNewest;
        releasesLoader_.Start([repository] {
            ReleaseList list;
            list.repository = repository;
            try {
                list.releases = ParseReleases(HttpGetText(ReleasesUrl(repository)));
            } catch (const std::exception& failure) {
                list.error = failure.what();
            }
            return list;
        });
    }

    void StartDownload(const std::string& repository, const Release& release) {
        if (job_.Busy()) return;
        downloaded_ = std::make_shared<std::optional<VersionEntry>>();
        auto result = downloaded_;
        const auto root = versionsRoot_;
        job_.Start("Download AnyPS5 " + release.tag + (repository == OfficialRepository ? "" : " (" + repository + ")"), true,
                   [root, repository, release, result](JobControl& control) {
                       *result = DownloadRelease(root, repository, release, HttpDownload, RunToFile, control);
                       return "Downloaded to " + ToUtf8((*result)->folder) + " and selected";
                   });
    }

    const AnyPS5Probe& ProbeFor(const VersionEntry& entry) {
        const auto key = ToUtf8(entry.folder);
        auto found = versionProbes_.find(key);
        if (found == versionProbes_.end()) found = versionProbes_.emplace(key, ProbeAnyPS5(key)).first;
        return found->second;
    }

    bool IsInstalled(const std::string& repository, const std::string& tag) const {
        return FindVersion(versions_, repository + "@" + tag) != nullptr;
    }

    void DrawMissingRelinker() {
        if (openMissing_ && !ImGui::IsPopupOpen("Settings")) {
            ImGui::OpenPopup("No AnyPS5 relinker");
            openMissing_ = false;
        }
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        if (!ImGui::BeginPopupModal("No AnyPS5 relinker", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) return;
        ImGui::TextUnformatted("The launcher needs AnyPS5's relinker and runtime to install and play games.");
        if (!settings_.anyps5Folder.empty() && !anyps5_.probe.problem.empty()) ImGui::TextColored(Amber, "%s", anyps5_.probe.problem.c_str());
        ImGui::Spacing();
        ImGui::Text("Download the latest release of %s?", OfficialRepository);
        ImGui::TextColored(Dim, "The relinker and the runtime libraries of the same release, into %s.", ToUtf8(versionsRoot_).c_str());
        ImGui::Spacing();
        ImGui::BeginDisabled(job_.Busy() || releasesLoader_.Running());
        if (ImGui::Button("Download the latest")) {
            LoadReleases(OfficialRepository, true);
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Choose a version...")) {
            ImGui::CloseCurrentPopup();
            OpenVersions();
        }
        ImGui::SameLine();
        if (ImGui::Button("Not now")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    void DrawVersions() {
        if (openVersions_) {
            ImGui::OpenPopup("AnyPS5 versions");
            openVersions_ = false;
        }
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowSize(ImVec2(viewport->WorkSize.x * 0.8f, viewport->WorkSize.y * 0.8f), ImGuiCond_Appearing);
        ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        bool open = true;
        if (!ImGui::BeginPopupModal("AnyPS5 versions", &open, ImGuiWindowFlags_NoSavedSettings)) return;
        const bool busy = job_.Busy() || session_ != nullptr;

        ImGui::TextUnformatted("Installed");
        ImGui::TextColored(Dim, "The selected version relinks games at Install. Each game keeps the version it was converted with until you reinstall it.");
        std::string remove;
        if (ImGui::BeginTable("versions", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 6);
            ImGui::TableSetupColumn("Version", ImGuiTableColumnFlags_WidthStretch, 1.4f);
            ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthStretch, 2.0f);
            ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 5);
            ImGui::TableHeadersRow();
            for (const auto& entry : versions_.versions) {
                ImGui::PushID(entry.id.c_str());
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                const bool selected = entry.id == settings_.anyps5Version;
                const auto& probe = ProbeFor(entry);
                if (selected) {
                    ImGui::TextColored(Green, "Selected");
                } else {
                    ImGui::BeginDisabled(busy || !probe.runtime);
                    if (ImGui::Button("Use")) SelectVersion(entry.id);
                    ImGui::EndDisabled();
                }
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(entry.Local() ? "local" : entry.tag.c_str());
                ImGui::TextColored(Dim, "%s", entry.Local() ? ToUtf8(entry.folder).c_str() : entry.repository.c_str());
                ImGui::TableNextColumn();
                ImGui::PushTextWrapPos(0.0f);
                if (probe.runtime) {
                    ImGui::TextColored(Green, "%s, %zu runtime libraries", probe.runtime->layout.c_str(), probe.runtime->libraries.size());
                    if (selected && !anyps5_.version.empty()) ImGui::TextColored(Dim, "relinker %s", anyps5_.version.c_str());
                } else {
                    ImGui::TextColored(Amber, "%s", probe.problem.c_str());
                }
                ImGui::PopTextWrapPos();
                ImGui::TableNextColumn();
                ImGui::BeginDisabled(busy || selected);
                if (ImGui::Button(entry.Local() ? "Forget" : "Delete")) remove = entry.id;
                ImGui::EndDisabled();
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                    ImGui::SetTooltip("%s", selected ? "Select another version first"
                                                     : entry.Local() ? "Removes it from the list; the folder stays" : "Deletes the downloaded folder");
                }
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        if (!remove.empty()) {
            try {
                RemoveVersion(versionsRoot_, versions_, remove);
                SaveVersions(versionsRoot_, versions_);
            } catch (const std::exception& failure) {
                versionsError_ = failure.what();
            }
        }
        if (versions_.versions.empty()) ImGui::TextColored(Dim, "None yet.");
        ImGui::BeginDisabled(busy || Picker().IsOpen());
        if (ImGui::Button("Add a local relinker...")) Picker().OpenFile(window_, &localPick_, "Relinker", TargetIsWindows ? "exe" : "");
        ImGui::SameLine();
        if (ImGui::Button("Add a local AnyPS5 folder...")) Picker().Open(window_, &localPick_);
        ImGui::EndDisabled();
        ImGui::TextColored(Dim, "A relinker from an AnyPS5 build (build\\core\\relinker) or from a release with its libs\\ beside it; a fork you built works the same way.");

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::TextUnformatted("Download a release");
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 18);
        if (ImGui::BeginCombo("##repository", releasesRepository_.c_str())) {
            if (ImGui::Selectable(OfficialRepository, releasesRepository_ == OfficialRepository)) releasesRepository_ = OfficialRepository;
            for (const auto& repository : versions_.repositories) {
                if (ImGui::Selectable(repository.c_str(), releasesRepository_ == repository)) releasesRepository_ = repository;
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(releasesLoader_.Running());
        if (ImGui::Button(releasesLoader_.Running() ? "Loading..." : "Load releases")) LoadReleases(releasesRepository_, false);
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14);
        ImGui::InputTextWithHint("##newrepo", "owner/repo of a fork", &newRepository_);
        ImGui::SameLine();
        ImGui::BeginDisabled(!IsValidRepository(newRepository_));
        if (ImGui::Button("Add repository")) {
            if (newRepository_ != OfficialRepository &&
                std::find(versions_.repositories.begin(), versions_.repositories.end(), newRepository_) == versions_.repositories.end()) {
                versions_.repositories.push_back(newRepository_);
                try {
                    SaveVersions(versionsRoot_, versions_);
                } catch (const std::exception& failure) {
                    versionsError_ = failure.what();
                }
            }
            releasesRepository_ = newRepository_;
            newRepository_.clear();
            LoadReleases(releasesRepository_, false);
        }
        ImGui::EndDisabled();
        ImGui::TextColored(Dim, "A fork works when it publishes GitHub releases with relinker-<tag>%s and prx-%s-<tag>%s, as AnyPS5's release workflow does.",
                           TargetIsWindows ? ".exe" : "", TargetIsWindows ? "windows" : "linux", TargetIsWindows ? ".zip" : ".tar.gz");

        if (releases_ && releases_->repository == releasesRepository_) {
            if (!releases_->error.empty()) {
                ImGui::TextColored(Red, "%s", releases_->error.c_str());
            } else if (releases_->releases.empty()) {
                ImGui::TextColored(Amber, "%s has no releases. Build it and add its relinker as a local version instead.", releases_->repository.c_str());
            } else if (ImGui::BeginTable("releases", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_ScrollY, ImVec2(0, ImGui::GetFontSize() * 14))) {
                const auto newest = NewestRelease(releases_->releases);
                for (const auto& release : releases_->releases) {
                    ImGui::PushID(release.tag.c_str());
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(release.tag.c_str());
                    if (newest && newest->tag == release.tag) {
                        ImGui::SameLine();
                        ImGui::TextColored(Green, "latest");
                    }
                    if (release.prerelease) {
                        ImGui::SameLine();
                        ImGui::TextColored(Amber, "pre-release");
                    }
                    ImGui::TableNextColumn();
                    ImGui::TextColored(Dim, "%s", release.publishedAt.substr(0, 10).c_str());
                    ImGui::TableNextColumn();
                    std::string problem;
                    const auto files = FilesFor(release, &problem);
                    if (IsInstalled(releases_->repository, release.tag)) {
                        ImGui::TextColored(Dim, "installed");
                    } else if (!files) {
                        ImGui::TextColored(Dim, "%s", problem.c_str());
                    } else {
                        ImGui::BeginDisabled(busy);
                        const auto label = "Download (" + FormatBytes(files->relinker.size + files->runtime.size) + ")";
                        if (ImGui::Button(label.c_str())) StartDownload(releases_->repository, release);
                        ImGui::EndDisabled();
                    }
                    ImGui::PopID();
                }
                ImGui::EndTable();
            }
        }
        if (job_.Busy()) ImGui::TextColored(Amber, "%s...", job_.Title().c_str());
        if (!versionsError_.empty()) ImGui::TextColored(Red, "%s", versionsError_.c_str());
        ImGui::Spacing();
        if (ImGui::Button("Close")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        (void)open;
    }

    void OpenSettings() {
        draft_ = settings_;
        draftProfile_ = profileSettings_;
        draftProbe_ = ProbeAnyPS5(draft_.anyps5Folder);
        draftProbeFor_ = draft_.anyps5Folder;
        saveError_.clear();
        openSettings_ = true;
    }

    void FolderField(const char* label, const char* id, const std::string& hint, std::string& value) {
        ImGui::TextUnformatted(label);
        const auto& style = ImGui::GetStyle();
        const float browse = ImGui::CalcTextSize("Browse...").x + style.FramePadding.x * 2 + style.ItemSpacing.x;
        ImGui::SetNextItemWidth(-browse);
        ImGui::InputTextWithHint(id, hint.c_str(), &value);
        ImGui::SameLine();
        ImGui::PushID(id);
        ImGui::BeginDisabled(Picker().IsOpen());
        if (ImGui::Button("Browse...")) Picker().Open(window_, &value);
        ImGui::EndDisabled();
        ImGui::PopID();
    }

    void DrawAnyPS5Status() {
        if (draftProbeFor_ != draft_.anyps5Folder) {
            draftProbe_ = ProbeAnyPS5(draft_.anyps5Folder);
            draftProbeFor_ = draft_.anyps5Folder;
        }
        ImGui::Indent();
        if (const auto& runtime = draftProbe_.runtime) {
            ImGui::TextColored(Green, "Found an AnyPS5 %s", runtime->layout.c_str());
            if (draftProbeFor_ == settings_.anyps5Folder && !anyps5_.version.empty()) {
                ImGui::SameLine();
                ImGui::TextColored(Dim, "relinker %s", anyps5_.version.c_str());
            }
            ImGui::TextColored(Dim, "relinker: %s", ToUtf8(runtime->relinker).c_str());
            ImGui::TextColored(Dim, "%zu runtime libraries; per-title settings: %s", runtime->libraries.size(),
                               runtime->titleCatalog ? "tools/titles.json" : "none");
            if (draftProbeFor_ == settings_.anyps5Folder && !anyps5_.versionError.empty()) ImGui::TextColored(Amber, "%s", anyps5_.versionError.c_str());
            if (draftProbeFor_ == settings_.anyps5Folder && !anyps5_.titlesError.empty()) ImGui::TextColored(Amber, "%s", anyps5_.titlesError.c_str());
        } else {
            ImGui::TextColored(Amber, "%s", draftProbe_.problem.c_str());
            ImGui::TextColored(Dim, "Use your AnyPS5 clone after a full build (build\\core\\relinker and build\\core\\libs\\libs), or an extracted release with libs\\.");
        }
        ImGui::Unindent();
    }

    void DrawSettings() {
        if (openSettings_) {
            ImGui::OpenPopup("Settings");
            openSettings_ = false;
        }
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowSize(ImVec2(viewport->WorkSize.x * 0.75f, 0), ImGuiCond_Appearing);
        ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        if (!ImGui::BeginPopupModal("Settings", nullptr, ImGuiWindowFlags_NoSavedSettings)) return;

        ImGui::TextUnformatted("AnyPS5 version");
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 24);
        {
            const auto* current = FindVersion(versions_, draft_.anyps5Version);
            const std::string preview = current != nullptr ? current->Label() : draft_.anyps5Folder.empty() ? std::string("none") : draft_.anyps5Folder;
            if (ImGui::BeginCombo("##anyps5version", preview.c_str())) {
                for (const auto& entry : versions_.versions) {
                    if (ImGui::Selectable(entry.Label().c_str(), entry.id == draft_.anyps5Version)) {
                        draft_.anyps5Version = entry.id;
                        draft_.anyps5Folder = ToUtf8(entry.folder);
                    }
                }
                ImGui::EndCombo();
            }
        }
        ImGui::SameLine();
        ImGui::TextColored(Dim, "download or add versions with \"AnyPS5 versions\" in the top bar");
        DrawAnyPS5Status();
        ImGui::Spacing();
        FolderField("Games folder (scanned for game folders and .zar files)", "##games", "the folder that holds your game folders and .zar files", draft_.gamesFolder);
        ImGui::Spacing();
        FolderField("Library folder (logs and shader caches; can be rebuilt)", "##library", ToUtf8(launcherFolder_ / "library"), draft_.libraryFolder);
        ImGui::Spacing();
        FolderField("Userdata folder (profiles, saves, trophies; back this up)", "##userdata", ToUtf8(launcherFolder_ / "userdata"), draft_.userdataFolder);
        ImGui::TextColored(Dim, "Keep it on the same drive as your games: saves are moved beside a game for each run, which is instant on one drive.");
        ImGui::Spacing();

        ImGui::Separator();
        ImGui::Text("Profile \"%s\"", layout_.profile.c_str());
        ImGui::TextUnformatted("Console language");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 16);
        if (ImGui::BeginCombo("##language", LanguageLabel(draftProfile_.language).c_str())) {
            if (ImGui::Selectable(LanguageLabel("").c_str(), draftProfile_.language.empty())) draftProfile_.language.clear();
            if (ImGui::Selectable(LanguageLabel("auto").c_str(), draftProfile_.language == "auto")) draftProfile_.language = "auto";
            for (const auto& item : ConsoleLanguages()) {
                const auto value = std::to_string(item.number);
                if (ImGui::Selectable(LanguageLabel(value).c_str(), draftProfile_.language == value)) draftProfile_.language = value;
            }
            ImGui::EndCombo();
        }
        ImGui::Spacing();
        ImGui::TextColored(Dim, "Saved to %s", ToUtf8(settingsFile_).c_str());
        if (!saveError_.empty()) ImGui::TextColored(Red, "%s", saveError_.c_str());
        ImGui::Separator();

        ImGui::BeginDisabled(Picker().IsOpen());
        if (ImGui::Button("Save")) {
            try {
                const auto library = EffectiveLibrary(draft_, launcherFolder_);
                const auto userdata = EffectiveUserdata(draft_, launcherFolder_);
                if (!draft_.gamesFolder.empty()) {
                    const auto games = FromUtf8(draft_.gamesFolder).lexically_normal();
                    for (const auto& [label, folder] : {std::pair{"library", library}, std::pair{"userdata", userdata}}) {
                        const auto relative = folder.lexically_normal().lexically_relative(games);
                        if (!relative.empty() && ToUtf8(*relative.begin()) != "..") {
                            throw std::runtime_error(std::string("the ") + label + " folder is inside the games folder; choose another place");
                        }
                    }
                }
                SaveSettings(settingsFile_, draft_);
                const bool gamesChanged = draft_.gamesFolder != settings_.gamesFolder;
                settings_ = draft_;
                settingsError_.clear();
                ApplySettings();
                CreateProfile(layout_);
                SaveProfile(layout_, draftProfile_);
                profileSettings_ = draftProfile_;
                profiles_ = ListProfiles(layout_.userdata);
                if (gamesChanged || !scanned_) {
                    result_ = {};
                    scanned_ = false;
                    icons_->Clear();
                }
                rescanWanted_ = true;
                ImGui::CloseCurrentPopup();
            } catch (const std::exception& failure) {
                saveError_ = std::string("Could not save: ") + failure.what();
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
        ImGui::EndDisabled();
        ImGui::EndPopup();
    }

    SDL_Window* window_;
    std::unique_ptr<IconCache> icons_;
    float scale_;
    std::filesystem::path launcherFolder_;
    std::filesystem::path settingsFile_;
    Settings settings_;
    std::string settingsError_;
    Layout layout_;
    std::vector<std::string> profiles_;
    ProfileSettings profileSettings_;
    std::string profileError_;
    AnyPS5State anyps5_;
    Background<AnyPS5State> anyps5Loader_;
    bool anyps5Pending_ = false;
    std::map<std::string, InstallManifest> installs_;
    std::map<std::string, RunReport> lastRuns_;
    Settings draft_;
    ProfileSettings draftProfile_;
    AnyPS5Probe draftProbe_;
    std::string draftProbeFor_;
    std::string saveError_;
    bool openSettings_ = false;
    std::filesystem::path versionsRoot_;
    VersionStore versions_;
    Background<ReleaseList> releasesLoader_;
    std::optional<ReleaseList> releases_;
    std::string releasesRepository_ = OfficialRepository;
    std::string newRepository_;
    std::string versionsError_;
    std::string localPick_;
    std::map<std::string, AnyPS5Probe> versionProbes_;
    std::shared_ptr<std::optional<VersionEntry>> downloaded_;
    bool openVersions_ = false;
    bool openMissing_ = false;
    bool missingAsked_ = false;
    bool latestWanted_ = false;
    bool openNewProfile_ = false;
    std::string newProfileName_;
    std::string newProfileError_;
    Background<ScanResult> scan_;
    BackgroundJob job_;
    std::string jobSerial_;
    std::unique_ptr<GameSession> session_;
    ScanResult result_;
    bool scanned_ = false;
    bool rescanWanted_ = false;
    std::optional<Message> message_;
};

}

int main(int, char**) {
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "SDL_Init failed: %s", SDL_GetError());
        return 1;
    }
    const float scale = SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay());
    SDL_Window* window = SDL_CreateWindow("AnyPS5 Launcher", static_cast<int>(1280 * scale), static_cast<int>(800 * scale),
                                          SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIDDEN | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (window == nullptr) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "SDL_CreateWindow failed: %s", SDL_GetError());
        return 1;
    }
    SDL_Renderer* renderer = SDL_CreateRenderer(window, nullptr);
    if (renderer == nullptr) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "SDL_CreateRenderer failed: %s", SDL_GetError());
        return 1;
    }
    SDL_SetRenderVSync(renderer, 1);
    SDL_SetWindowPosition(window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    SDL_ShowWindow(window);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    io.IniFilename = nullptr;
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.Colors[ImGuiCol_FrameBg] = ImVec4(0.20f, 0.22f, 0.27f, 1.0f);
    style.Colors[ImGuiCol_FrameBgHovered] = ImVec4(0.26f, 0.29f, 0.36f, 1.0f);
    style.Colors[ImGuiCol_FrameBgActive] = ImVec4(0.30f, 0.34f, 0.42f, 1.0f);
    style.FrameRounding = 3.0f;
    style.ScaleAllSizes(scale);
    style.FontScaleDpi = scale;
    LoadFonts(io);
    ImGui_ImplSDL3_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer3_Init(renderer);

    {
        LauncherApp app(window, renderer, scale);
        bool done = false;
        while (!done) {
            SDL_Event event;
            while (SDL_PollEvent(&event)) {
                ImGui_ImplSDL3_ProcessEvent(&event);
                if (event.type == SDL_EVENT_QUIT) done = true;
                if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED && event.window.windowID == SDL_GetWindowID(window)) done = true;
            }
            if (SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED) {
                SDL_Delay(10);
                continue;
            }
            ImGui_ImplSDLRenderer3_NewFrame();
            ImGui_ImplSDL3_NewFrame();
            ImGui::NewFrame();
            app.Frame();
            ImGui::Render();
            SDL_SetRenderScale(renderer, io.DisplayFramebufferScale.x, io.DisplayFramebufferScale.y);
            SDL_SetRenderDrawColorFloat(renderer, 0.08f, 0.08f, 0.10f, 1.0f);
            SDL_RenderClear(renderer);
            ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);
            SDL_RenderPresent(renderer);
        }
    }

    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
