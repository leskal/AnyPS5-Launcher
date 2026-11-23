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

// The palette in use; ApplyTheme sets it for dark or light.
ImVec4 Green, Amber, Red, Dim, Accent, AccentHover, AccentActive, Card, CardHover, Placeholder, WindowColor;

ImVec4 Fade(ImVec4 color, float alpha) {
    color.w = alpha;
    return color;
}

ImVec4 Rgb(int value, float alpha = 1.0f) {
    return ImVec4(static_cast<float>((value >> 16) & 0xFF) / 255.0f, static_cast<float>((value >> 8) & 0xFF) / 255.0f, static_cast<float>(value & 0xFF) / 255.0f, alpha);
}

struct Palette {
    int text, textDisabled, background, popup, border, frame, frameHover, frameActive, title, scroll, scrollHover, scrollActive, button, buttonHover, buttonActive,
        headerHover, card, cardHover, placeholder;
    int green, amber, red, dim, accent, accentHover, accentActive;
    float dimModal, rowAlt;
};

constexpr Palette DarkPalette{0xE6E8EB, 0x7C828C, 0x0F1115, 0x181B21, 0x262A31, 0x1E2228, 0x262B33, 0x2C323B, 0x14171C, 0x2A2F37, 0x353B45, 0x404754, 0x23272F,
                              0x2C323C, 0x343B47, 0x2B3240, 0x181B21, 0x1F232A, 0x1E2638, 0x34D399, 0xFBBF24, 0xF87171, 0x8B929C, 0x3B82F6, 0x5B9BFF, 0x2F6FD6, 0.6f,
                              0.02f};
constexpr Palette LightPalette{0x1F2329, 0x9AA1AB, 0xF3F4F6, 0xFFFFFF, 0xDDE1E6, 0xE6E9EE, 0xDCE0E6, 0xD2D7DF, 0xE9ECF0, 0xC9CED6, 0xB5BCC6, 0xA1A9B5, 0xE6E9EE,
                               0xDCE0E6, 0xD0D5DD, 0xDCE4F2, 0xFFFFFF, 0xF7F9FC, 0xDDE4F0, 0x059669, 0xB45309, 0xDC2626, 0x6B7280, 0x2563EB, 0x3B82F6, 0x1D4ED8, 0.35f,
                               0.03f};

// "system" follows Windows' light or dark setting; "dark" and "light" force one.
bool WantDark(const std::string& theme) {
    if (theme == "dark") return true;
    if (theme == "light") return false;
    return SDL_GetSystemTheme() != SDL_SYSTEM_THEME_LIGHT;
}

void ApplyTheme(ImGuiStyle& style, bool dark) {
    const auto& p = dark ? DarkPalette : LightPalette;
    Green = Rgb(p.green);
    Amber = Rgb(p.amber);
    Red = Rgb(p.red);
    Dim = Rgb(p.dim);
    Accent = Rgb(p.accent);
    AccentHover = Rgb(p.accentHover);
    AccentActive = Rgb(p.accentActive);
    Card = Rgb(p.card);
    CardHover = Rgb(p.cardHover);
    Placeholder = Rgb(p.placeholder);
    WindowColor = Rgb(p.background);

    if (dark) ImGui::StyleColorsDark(&style);
    else ImGui::StyleColorsLight(&style);
    auto* c = style.Colors;
    c[ImGuiCol_Text] = Rgb(p.text);
    c[ImGuiCol_TextDisabled] = Rgb(p.textDisabled);
    c[ImGuiCol_WindowBg] = Rgb(p.background);
    c[ImGuiCol_ChildBg] = Rgb(p.background, 0.0f);
    c[ImGuiCol_PopupBg] = Rgb(p.popup, 0.98f);
    c[ImGuiCol_Border] = Rgb(p.border);
    c[ImGuiCol_BorderShadow] = Rgb(0, 0.0f);
    c[ImGuiCol_FrameBg] = Rgb(p.frame);
    c[ImGuiCol_FrameBgHovered] = Rgb(p.frameHover);
    c[ImGuiCol_FrameBgActive] = Rgb(p.frameActive);
    c[ImGuiCol_TitleBg] = Rgb(p.title);
    c[ImGuiCol_TitleBgActive] = Rgb(p.popup);
    c[ImGuiCol_TitleBgCollapsed] = Rgb(p.title);
    c[ImGuiCol_MenuBarBg] = Rgb(p.title);
    c[ImGuiCol_ScrollbarBg] = Rgb(0, 0.0f);
    c[ImGuiCol_ScrollbarGrab] = Rgb(p.scroll);
    c[ImGuiCol_ScrollbarGrabHovered] = Rgb(p.scrollHover);
    c[ImGuiCol_ScrollbarGrabActive] = Rgb(p.scrollActive);
    c[ImGuiCol_CheckMark] = Accent;
    c[ImGuiCol_SliderGrab] = Accent;
    c[ImGuiCol_SliderGrabActive] = AccentHover;
    c[ImGuiCol_Button] = Rgb(p.button);
    c[ImGuiCol_ButtonHovered] = Rgb(p.buttonHover);
    c[ImGuiCol_ButtonActive] = Rgb(p.buttonActive);
    c[ImGuiCol_Header] = Rgb(p.button);
    c[ImGuiCol_HeaderHovered] = Rgb(p.headerHover);
    c[ImGuiCol_HeaderActive] = Fade(Accent, 0.35f);
    c[ImGuiCol_Separator] = Rgb(p.border);
    c[ImGuiCol_SeparatorHovered] = Accent;
    c[ImGuiCol_SeparatorActive] = AccentHover;
    c[ImGuiCol_ResizeGrip] = Rgb(0, 0.0f);
    c[ImGuiCol_ResizeGripHovered] = Fade(Accent, 0.6f);
    c[ImGuiCol_ResizeGripActive] = Accent;
    c[ImGuiCol_Tab] = Rgb(p.popup);
    c[ImGuiCol_TabHovered] = Rgb(p.headerHover);
    c[ImGuiCol_PlotHistogram] = Accent;
    c[ImGuiCol_PlotHistogramHovered] = AccentHover;
    c[ImGuiCol_TableHeaderBg] = Rgb(p.popup);
    c[ImGuiCol_TableBorderStrong] = Rgb(p.border);
    c[ImGuiCol_TableBorderLight] = Rgb(p.frame);
    c[ImGuiCol_TableRowBg] = Rgb(0, 0.0f);
    c[ImGuiCol_TableRowBgAlt] = dark ? Rgb(0xFFFFFF, p.rowAlt) : Rgb(0x000000, p.rowAlt);
    c[ImGuiCol_TextSelectedBg] = Fade(Accent, 0.35f);
    c[ImGuiCol_NavCursor] = Accent;
    c[ImGuiCol_ModalWindowDimBg] = Rgb(0, p.dimModal);
    style.WindowRounding = 10.0f;
    style.ChildRounding = 10.0f;
    style.FrameRounding = 6.0f;
    style.PopupRounding = 8.0f;
    style.GrabRounding = 6.0f;
    style.ScrollbarRounding = 8.0f;
    style.TabRounding = 6.0f;
    style.WindowPadding = ImVec2(16.0f, 14.0f);
    style.FramePadding = ImVec2(10.0f, 6.0f);
    style.ItemSpacing = ImVec2(10.0f, 8.0f);
    style.ItemInnerSpacing = ImVec2(8.0f, 6.0f);
    style.ScrollbarSize = 12.0f;
    style.WindowBorderSize = 0.0f;
    style.ChildBorderSize = 0.0f;
    style.PopupBorderSize = 1.0f;
    style.FrameBorderSize = 0.0f;
    style.WindowTitleAlign = ImVec2(0.0f, 0.5f);
}

// A filled, rounded label such as "Installed" or "SELF".
void Pill(const char* text, ImVec4 color) {
    const ImVec2 padding(8.0f * ImGui::GetStyle().FontScaleDpi, 2.0f * ImGui::GetStyle().FontScaleDpi);
    const ImVec2 size = ImGui::CalcTextSize(text);
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const ImVec2 end(at.x + size.x + padding.x * 2, at.y + size.y + padding.y * 2);
    auto* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(at, end, ImGui::GetColorU32(Fade(color, 0.16f)), (end.y - at.y) * 0.5f);
    draw->AddText(ImVec2(at.x + padding.x, at.y + padding.y), ImGui::GetColorU32(color), text);
    ImGui::Dummy(ImVec2(end.x - at.x, end.y - at.y));
}

bool PrimaryButton(const char* label, const ImVec2& size = ImVec2(0, 0)) {
    ImGui::PushStyleColor(ImGuiCol_Button, Accent);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, AccentHover);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, AccentActive);
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));
    const bool pressed = ImGui::Button(label, size);
    ImGui::PopStyleColor(4);
    return pressed;
}

// Text cut to a width with an ellipsis.
std::string Elide(const std::string& text, float width) {
    if (ImGui::CalcTextSize(text.c_str()).x <= width) return text;
    std::string cut = text;
    while (!cut.empty() && ImGui::CalcTextSize((cut + "...").c_str()).x > width) {
        cut.pop_back();
        while (!cut.empty() && (static_cast<unsigned char>(cut.back()) & 0xC0) == 0x80) cut.pop_back();
        if (!cut.empty() && static_cast<unsigned char>(cut.back()) >= 0xC0) cut.pop_back();
    }
    return cut + "...";
}

bool ContainsFolded(const std::string& haystack, const std::string& needle) {
    if (needle.empty()) return true;
    const auto fold = [](unsigned char c) { return static_cast<char>(std::tolower(c)); };
    std::string a(haystack.size(), ' '), b(needle.size(), ' ');
    std::transform(haystack.begin(), haystack.end(), a.begin(), fold);
    std::transform(needle.begin(), needle.end(), b.begin(), fold);
    return a.find(b) != std::string::npos;
}

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
        ApplyCurrentTheme();
        if (!loaded.existed || settings_.gamesFolder.empty()) OpenSettings();
        else StartScan();
    }

    // Windows switched between light and dark.
    void ThemeChanged() {
        ApplyCurrentTheme();
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
        DrawLibrary();
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
        const auto& style = ImGui::GetStyle();
        ImGui::PushFont(nullptr, style.FontSizeBase * 1.45f);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Library");
        ImGui::PopFont();
        ImGui::SameLine(0, style.ItemSpacing.x * 2);

        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 11);
        ImGui::BeginDisabled(session_ != nullptr);
        const std::string profileLabel = "Profile: " + layout_.profile;
        if (ImGui::BeginCombo("##profile", profileLabel.c_str())) {
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
        // The search box takes the room left before the buttons, and its own row when there's too little.
        const float buttons = ImGui::CalcTextSize("AnyPS5 versions").x + ImGui::CalcTextSize("Rescan").x + ImGui::CalcTextSize("Settings").x + style.FramePadding.x * 6 +
                              style.ItemSpacing.x * 2;
        ImGui::SameLine();
        const float room = ImGui::GetContentRegionMax().x - buttons - ImGui::GetCursorPosX() - style.ItemSpacing.x;
        const bool searchInline = room >= ImGui::GetFontSize() * 7;
        if (searchInline) {
            ImGui::SetNextItemWidth(std::min(room, ImGui::GetFontSize() * 16));
            ImGui::InputTextWithHint("##search", "Search name or serial", &search_);
            ImGui::SameLine();
        }
        ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), ImGui::GetContentRegionMax().x - buttons));
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
        if (!searchInline) {
            ImGui::SetNextItemWidth(-FLT_MIN);
            ImGui::InputTextWithHint("##search", "Search name or serial", &search_);
        }

        // Status line: counts, AnyPS5, problems.
        if (scan_.Running()) {
            Pill("Scanning...", Dim);
        } else if (scanned_) {
            const std::string counts = std::to_string(result_.games.size()) + (result_.games.size() == 1 ? " game" : " games") + "  \xC2\xB7  " +
                                       std::to_string(installs_.size()) + " installed";
            Pill(counts.c_str(), Dim);
        }
        ImGui::SameLine();
        if (anyps5Loader_.Running() || anyps5Pending_) {
            Pill("Checking AnyPS5...", Dim);
        } else if (!anyps5_.probe.runtime) {
            Pill("AnyPS5 not set up", Amber);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", anyps5_.probe.problem.c_str());
        } else {
            const std::string label = "AnyPS5 " + CurrentVersionLabel();
            Pill(label.c_str(), Accent);
            if (ImGui::IsItemHovered() && !anyps5_.version.empty()) ImGui::SetTooltip("relinker %s", anyps5_.version.c_str());
        }
        if (!result_.problems.empty()) {
            ImGui::SameLine();
            const std::string label = std::to_string(result_.problems.size()) + (result_.problems.size() == 1 ? " problem" : " problems");
            Pill(label.c_str(), Red);
            if (ImGui::IsItemClicked()) ImGui::OpenPopup("problems");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Click to see them");
            if (ImGui::BeginPopup("problems")) {
                for (const auto& problem : result_.problems) {
                    ImGui::TextColored(Red, "%s", ToUtf8(problem.location).c_str());
                    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 40);
                    ImGui::TextUnformatted(problem.message.c_str());
                    ImGui::PopTextWrapPos();
                    ImGui::Spacing();
                }
                ImGui::EndPopup();
            }
        }
        ImGui::SameLine();
        ImGui::TextColored(Dim, "%s", settings_.gamesFolder.empty() ? "No games folder set" : settings_.gamesFolder.c_str());
        if (!settingsError_.empty()) ImGui::TextColored(Red, "Settings: %s", settingsError_.c_str());
        if (!profileError_.empty()) ImGui::TextColored(Red, "Profile: %s", profileError_.c_str());
        ImGui::Spacing();
    }

    int FooterLines() const {
        return std::max(1, (job_.Busy() ? 1 : 0) + (session_ ? 1 : 0) + (!job_.Busy() && !session_ && message_ ? 1 : 0));
    }

    float FooterHeight() const {
        return ImGui::GetFrameHeightWithSpacing() * static_cast<float>(FooterLines()) + ImGui::GetStyle().ItemSpacing.y;
    }

    struct Badge {
        const char* text = nullptr;
        ImVec4 color;
    };

    Badge BadgeFor(const GameEntry& game) const {
        if (IsRunning(game)) return {"Running", Accent};
        if (jobSerial_ == game.metadata.titleId && job_.Busy()) return {"Working...", Amber};
        if (const auto* manifest = InstallFor(game)) {
            if (!ReinstallReason(game, *manifest, anyps5_.version).empty()) return {"Reinstall", Amber};
            return {"Installed", Green};
        }
        if (game.executable.kind == ExecutableKind::ElfEncrypted) return {"Encrypted", Red};
        if (game.executable.kind == ExecutableKind::Missing || game.executable.kind == ExecutableKind::Unknown) return {"No eboot", Red};
        if (game.format == PackageFormat::Folder && (game.executable.kind == ExecutableKind::Self || game.modules.self > 0)) return {"SELF", Amber};
        return {};
    }

    const GameEntry* SelectedGame() const {
        for (const auto& game : result_.games) {
            if (ToUtf8(game.location) == selected_) return &game;
        }
        return nullptr;
    }

    void ActivateGame(const GameEntry& game) {
        if (PlayBlocker(game).empty()) Play(game);
        else if (InstallFor(game) == nullptr && InstallBlocker(game).empty()) StartInstall(game);
    }

    void DrawLibrary() {
        const auto& style = ImGui::GetStyle();
        const float panelWidth = std::min(ImGui::GetContentRegionAvail().x * 0.38f, 380.0f * scale_);
        const float height = -FooterHeight();
        if (SelectedGame() == nullptr && !result_.games.empty()) selected_ = ToUtf8(result_.games.front().location);

        ImGui::BeginChild("grid", ImVec2(-panelWidth - style.ItemSpacing.x, height));
        DrawGrid();
        ImGui::EndChild();
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_ChildBg, Card);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16.0f * scale_, 16.0f * scale_));
        ImGui::BeginChild("details", ImVec2(0, height), ImGuiChildFlags_AlwaysUseWindowPadding);
        if (const auto* game = SelectedGame()) DrawDetails(*game);
        else DrawEmptyDetails();
        ImGui::EndChild();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();
    }

    void DrawEmptyDetails() {
        ImGui::Spacing();
        ImGui::TextColored(Dim, "%s", scan_.Running() ? "Looking for games..." : settings_.gamesFolder.empty() ? "Set your games folder in Settings." : "No games found.");
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored(Dim, "Game folders (with eboot.bin and sce_sys) and .zar archives in the games folder show up here.");
        ImGui::PopTextWrapPos();
    }

    void DrawGrid() {
        // Covers follow the window: about five to a row at the default size, bigger
        // or smaller with Ctrl + mouse wheel, and each row always filled edge to edge.
        if (ImGui::IsWindowHovered() && ImGui::GetIO().KeyCtrl && ImGui::GetIO().MouseWheel != 0.0f) {
            gridZoom_ = std::clamp(gridZoom_ * (ImGui::GetIO().MouseWheel > 0 ? 1.1f : 1.0f / 1.1f), 0.5f, 2.5f);
        }
        const float gap = 16.0f * scale_;
        const float textBlock = ImGui::GetTextLineHeightWithSpacing() * 2 + 10.0f * scale_;
        const float avail = ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ScrollbarSize;
        const float wanted = std::clamp(avail / 5.0f * gridZoom_, 96.0f * scale_, std::max(96.0f * scale_, avail));
        const int columns = std::max(1, static_cast<int>((avail + gap) / (wanted + gap)));
        const float tile = std::max(48.0f * scale_, (avail - gap * static_cast<float>(columns - 1)) / static_cast<float>(columns));
        const float rounding = 10.0f * scale_;
        int shown = 0;
        for (const auto& game : result_.games) {
            if (!ContainsFolded(game.metadata.name, search_) && !ContainsFolded(game.metadata.titleId, search_)) continue;
            if (shown % columns != 0) ImGui::SameLine(0, gap);
            ++shown;
            const auto key = ToUtf8(game.location);
            ImGui::PushID(key.c_str());
            const ImVec2 at = ImGui::GetCursorScreenPos();
            ImGui::InvisibleButton("tile", ImVec2(tile, tile + textBlock));
            const bool hovered = ImGui::IsItemHovered();
            if (ImGui::IsItemClicked(ImGuiMouseButton_Left) || ImGui::IsItemClicked(ImGuiMouseButton_Right)) selected_ = key;
            if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) ActivateGame(game);
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) ImGui::SetTooltip("%s\nDouble-click to play, right-click for all actions", key.c_str());
            if (ImGui::BeginPopupContextItem("actions")) {
                DrawActionItems(game);
                ImGui::EndPopup();
            }
            const bool selected = selected_ == key;

            auto* draw = ImGui::GetWindowDrawList();
            const ImVec2 imageEnd(at.x + tile, at.y + tile);
            draw->AddRectFilled(at, ImVec2(at.x + tile, at.y + tile + textBlock), ImGui::GetColorU32(hovered || selected ? CardHover : Card), rounding);
            if (auto* texture = icons_->Get(game.location, game.icon)) {
                draw->AddImageRounded(static_cast<ImTextureID>(reinterpret_cast<std::intptr_t>(texture)), at, imageEnd, ImVec2(0, 0), ImVec2(1, 1), IM_COL32_WHITE, rounding,
                                      ImDrawFlags_RoundCornersTop);
            } else {
                draw->AddRectFilled(at, imageEnd, ImGui::GetColorU32(Placeholder), rounding, ImDrawFlags_RoundCornersTop);
                const std::string initial = game.metadata.name.empty() ? "?" : game.metadata.name.substr(0, 1);
                ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 3.0f);
                const ImVec2 size = ImGui::CalcTextSize(initial.c_str());
                draw->AddText(ImVec2(at.x + (tile - size.x) * 0.5f, at.y + (tile - size.y) * 0.5f), ImGui::GetColorU32(Dim), initial.c_str());
                ImGui::PopFont();
            }
            if (const auto badge = BadgeFor(game); badge.text != nullptr) {
                const ImVec2 size = ImGui::CalcTextSize(badge.text);
                const ImVec2 pad(8.0f * scale_, 3.0f * scale_);
                const ImVec2 start(at.x + 8.0f * scale_, at.y + 8.0f * scale_);
                const float dot = 10.0f * scale_;
                const ImVec2 end(start.x + dot + size.x + pad.x * 2, start.y + size.y + pad.y * 2);
                draw->AddRectFilled(start, end, ImGui::GetColorU32(ImVec4(0.06f, 0.07f, 0.08f, 0.85f)), (end.y - start.y) * 0.5f);
                draw->AddCircleFilled(ImVec2(start.x + pad.x + 3.0f * scale_, (start.y + end.y) * 0.5f), 3.5f * scale_, ImGui::GetColorU32(badge.color));
                draw->AddText(ImVec2(start.x + pad.x + 10.0f * scale_, start.y + pad.y), ImGui::GetColorU32(ImVec4(1, 1, 1, 0.92f)), badge.text);
            }
            if (selected || hovered) {
                draw->AddRect(at, ImVec2(at.x + tile, at.y + tile + textBlock), ImGui::GetColorU32(selected ? Accent : Fade(Accent, 0.45f)), rounding, 0, 2.0f * scale_);
            }
            const float textX = at.x + 10.0f * scale_;
            const float textWidth = tile - 20.0f * scale_;
            draw->AddText(ImVec2(textX, imageEnd.y + 6.0f * scale_), ImGui::GetColorU32(ImGuiCol_Text), Elide(game.metadata.name, textWidth).c_str());
            const std::string sub = game.metadata.titleId + "  \xC2\xB7  " + std::string(ToString(game.format));
            draw->AddText(ImVec2(textX, imageEnd.y + 6.0f * scale_ + ImGui::GetTextLineHeightWithSpacing()), ImGui::GetColorU32(Dim), Elide(sub, textWidth).c_str());
            ImGui::PopID();
        }
        if (shown == 0) {
            ImGui::Spacing();
            ImGui::TextColored(Dim, "%s", result_.games.empty() ? (scan_.Running() ? "Scanning..." : "No games yet.") : "Nothing matches the search.");
        }
    }

    void DrawDetails(const GameEntry& game) {
        ImGui::PushTextWrapPos(0.0f);  // everything in the panel wraps to its width
        DrawDetailsBody(game);
        ImGui::PopTextWrapPos();
    }

    void DrawDetailsBody(const GameEntry& game) {
        const auto& style = ImGui::GetStyle();
        const float width = ImGui::GetContentRegionAvail().x;
        const float art = std::min(width, 280.0f * scale_);
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (width - art) * 0.5f);
        const ImVec2 at = ImGui::GetCursorScreenPos();
        auto* draw = ImGui::GetWindowDrawList();
        if (auto* texture = icons_->Get(game.location, game.icon)) {
            draw->AddImageRounded(static_cast<ImTextureID>(reinterpret_cast<std::intptr_t>(texture)), at, ImVec2(at.x + art, at.y + art), ImVec2(0, 0), ImVec2(1, 1),
                                  IM_COL32_WHITE, 12.0f * scale_);
        } else {
            draw->AddRectFilled(at, ImVec2(at.x + art, at.y + art), ImGui::GetColorU32(Placeholder), 12.0f * scale_);
        }
        ImGui::Dummy(ImVec2(art, art));
        ImGui::Spacing();
        ImGui::PushFont(nullptr, style.FontSizeBase * 1.35f);
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextUnformatted(game.metadata.name.c_str());
        ImGui::PopTextWrapPos();
        ImGui::PopFont();
        ImGui::TextColored(Dim, "%s  \xC2\xB7  v%s  \xC2\xB7  %s", game.metadata.titleId.c_str(), game.metadata.contentVersion.c_str(), ToString(game.format).data());
        if (GroupByTitleId(result_.games).at(game.metadata.titleId).size() > 1) {
            Pill("Another copy of this game is in the list", Amber);
        }
        ImGui::Spacing();

        // Primary actions.
        const auto* manifest = InstallFor(game);
        const float full = ImGui::GetContentRegionAvail().x;
        const float buttonHeight = ImGui::GetFrameHeight() * 1.35f;
        if (IsRunning(game)) {
            if (PrimaryButton("Stop", ImVec2(full, buttonHeight))) session_->Stop();
        } else if (manifest != nullptr) {
            const auto reason = PlayBlocker(game);
            ImGui::BeginDisabled(!reason.empty());
            if (PrimaryButton("Play", ImVec2(full, buttonHeight))) Play(game);
            ImGui::EndDisabled();
            if (!reason.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", reason.c_str());
        } else {
            const bool needsConversion = game.format == PackageFormat::Folder && (game.executable.kind == ExecutableKind::Self || game.modules.self > 0);
            if (needsConversion) {
                ImGui::BeginDisabled(job_.Busy());
                if (PrimaryButton("Convert SELF to ELF", ImVec2(full, buttonHeight))) StartConvertSelf(game);
                ImGui::EndDisabled();
                ImGui::PushTextWrapPos(0.0f);
                ImGui::TextColored(Dim, "Needed before Install; the originals are zipped first.");
                ImGui::PopTextWrapPos();
            } else {
                const auto reason = InstallBlocker(game);
                ImGui::BeginDisabled(!reason.empty());
                if (PrimaryButton("Install", ImVec2(full, buttonHeight))) StartInstall(game);
                ImGui::EndDisabled();
                if (!reason.empty()) {
                    ImGui::PushTextWrapPos(0.0f);
                    ImGui::TextColored(Amber, "%s", reason.c_str());
                    ImGui::PopTextWrapPos();
                }
            }
        }
        if (ImGui::Button("All actions...", ImVec2(full, 0))) ImGui::OpenPopup("details-actions");
        if (ImGui::BeginPopup("details-actions")) {
            DrawActionItems(game);
            ImGui::EndPopup();
        }

        ImGui::Spacing();
        ImGui::SeparatorText("Status");
        DrawStatus(game);
        ImGui::SeparatorText("Executable");
        DrawExecutable(game);
        ImGui::SeparatorText("Location");
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored(Dim, "%s", ToUtf8(game.home.empty() ? game.location : game.home).c_str());
        ImGui::PopTextWrapPos();
        if (game.format == PackageFormat::Zar && game.layout == ZarLayout::Nested) ImGui::TextColored(Amber, "The game is inside a folder of the archive");
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

    void DrawActionItems(const GameEntry& game) {
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
            if (PrimaryButton("Stop")) session_->Stop();
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
            ImGui::TextColored(Dim, "Select a game to see it; double-click to play; right-click for every action.");
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

    void ApplyCurrentTheme() {
        auto& style = ImGui::GetStyle();
        ApplyTheme(style, WantDark(settings_.theme));
        style.ScaleAllSizes(scale_);
        style.FontScaleDpi = scale_;
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
        (void)viewport;
        FollowWindow(0.84f, 0.86f);
        bool open = true;
        if (!ImGui::BeginPopupModal("AnyPS5 versions", &open, ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove)) return;
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
        saveError_.clear();
        openSettings_ = true;
    }

    // Sizes the next window to a share of the launcher's window, kept on screen and centred.
    void FollowWindow(float widthShare, float heightShare) {
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        const float margin = 24.0f * scale_;
        const ImVec2 room(std::max(200.0f, viewport->WorkSize.x - margin * 2), std::max(160.0f, viewport->WorkSize.y - margin * 2));
        const ImVec2 size(std::min(room.x, std::max(viewport->WorkSize.x * widthShare, std::min(room.x, 560.0f * scale_))),
                          std::min(room.y, std::max(viewport->WorkSize.y * heightShare, std::min(room.y, 360.0f * scale_))));
        ImGui::SetNextWindowSize(size, ImGuiCond_Always);
        ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
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

    void DrawSettings() {
        if (openSettings_) {
            ImGui::OpenPopup("Settings");
            openSettings_ = false;
        }
        // Follows the launcher's window every frame: resizing or maximising it resizes
        // Settings, and the form scrolls when there isn't room.
        FollowWindow(0.8f, 0.86f);
        if (!ImGui::BeginPopupModal("Settings", nullptr, ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove)) return;
        const auto& style = ImGui::GetStyle();
        const float footer = ImGui::GetTextLineHeightWithSpacing() * (saveError_.empty() ? 1.0f : 2.0f) + ImGui::GetFrameHeightWithSpacing() + style.ItemSpacing.y * 2;
        ImGui::BeginChild("settings-body", ImVec2(0, -footer));
        ImGui::PushTextWrapPos(0.0f);

        FolderField("Games folder (scanned for game folders and .zar files)", "##games", "the folder that holds your game folders and .zar files", draft_.gamesFolder);
        ImGui::Spacing();
        FolderField("Library folder (logs and shader caches; can be rebuilt)", "##library", ToUtf8(launcherFolder_ / "library"), draft_.libraryFolder);
        ImGui::Spacing();
        FolderField("Userdata folder (profiles, saves, trophies; back this up)", "##userdata", ToUtf8(launcherFolder_ / "userdata"), draft_.userdataFolder);
        ImGui::TextColored(Dim, "Keep it on the same drive as your games: saves are moved beside a game for each run, which is instant on one drive.");
        ImGui::Spacing();

        ImGui::TextUnformatted("Appearance");
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14);
        {
            const char* labels[] = {"Follow Windows", "Dark", "Light"};
            const char* values[] = {"system", "dark", "light"};
            int current = 0;
            for (int index = 0; index < 3; ++index) {
                if (draft_.theme == values[index]) current = index;
            }
            if (ImGui::Combo("##theme", &current, labels, 3)) {
                draft_.theme = values[current];
                ApplyTheme(ImGui::GetStyle(), WantDark(draft_.theme));
                ImGui::GetStyle().ScaleAllSizes(scale_);
            }
        }
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
        ImGui::PopTextWrapPos();
        ImGui::EndChild();
        ImGui::Separator();
        ImGui::TextColored(Dim, "%s", Elide("Saved to " + ToUtf8(settingsFile_), ImGui::GetContentRegionAvail().x).c_str());
        if (!saveError_.empty()) ImGui::TextColored(Red, "%s", Elide(saveError_, ImGui::GetContentRegionAvail().x).c_str());

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
                // The AnyPS5 version is chosen in "AnyPS5 versions", not here: keep whatever is current.
                draft_.anyps5Folder = settings_.anyps5Folder;
                draft_.anyps5Version = settings_.anyps5Version;
                SaveSettings(settingsFile_, draft_);
                const bool gamesChanged = draft_.gamesFolder != settings_.gamesFolder;
                settings_ = draft_;
                settingsError_.clear();
                ApplySettings();
                ApplyCurrentTheme();
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
        if (ImGui::Button("Cancel")) {
            ApplyCurrentTheme();  // undo a previewed appearance
            ImGui::CloseCurrentPopup();
        }
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
    std::string saveError_;
    bool openSettings_ = false;
    std::string search_;
    float gridZoom_ = 1.0f;
    std::string selected_;
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
    ImGuiStyle& style = ImGui::GetStyle();
    ApplyTheme(style, WantDark("system"));
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
                if (event.type == SDL_EVENT_SYSTEM_THEME_CHANGED) app.ThemeChanged();
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
            SDL_SetRenderDrawColorFloat(renderer, WindowColor.x, WindowColor.y, WindowColor.z, 1.0f);
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
