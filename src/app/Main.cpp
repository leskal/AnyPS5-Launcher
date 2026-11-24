#include <algorithm>
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <cmath>
#include <ctime>
#include <deque>
#include <optional>
#include <set>
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
#include "core/InputMapping.hpp"
#include "core/Install.hpp"
#include "core/Launch.hpp"
#include "core/Modules.hpp"
#include "core/PatchInfo.hpp"
#include "core/Profiles.hpp"
#include "core/Scanner.hpp"
#include "core/Session.hpp"
#include "core/Settings.hpp"
#include "core/Trophies.hpp"
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

// A rounded, bordered group inside a dialog, with a title and an optional note.
template <typename Body>
void Section(const char* id, const char* title, const char* note, Body body) {
    const float scale = ImGui::GetStyle().FontScaleDpi;
    ImGui::PushStyleColor(ImGuiCol_ChildBg, CardHover);
    ImGui::PushStyleColor(ImGuiCol_Border, ImGui::GetStyle().Colors[ImGuiCol_Border]);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16.0f * scale, 14.0f * scale));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 10.0f * scale);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 1.0f);
    ImGui::BeginChild(id, ImVec2(-FLT_MIN, 0), ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_Borders);
    // Fields and buttons stand out from the card: the window's colour inside, a thin border around.
    ImGui::PushStyleColor(ImGuiCol_FrameBg, WindowColor);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
    ImGui::PushTextWrapPos(0.0f);
    ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.12f);
    ImGui::TextUnformatted(title);
    ImGui::PopFont();
    if (note != nullptr && note[0] != 0) ImGui::TextColored(Dim, "%s", note);
    ImGui::Spacing();
    body();
    ImGui::PopTextWrapPos();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
    ImGui::EndChild();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(2);
    ImGui::Spacing();
}

// A row of buttons where one is chosen, like a segmented switch. Returns true when it changes.
bool Segmented(const char* id, const char* const* labels, int count, int& current) {
    ImGui::PushID(id);
    bool changed = false;
    const float width = std::min(ImGui::GetContentRegionAvail().x, ImGui::GetFontSize() * 9.0f * static_cast<float>(count));
    const float each = (width - ImGui::GetStyle().ItemSpacing.x * 0.25f * static_cast<float>(count - 1)) / static_cast<float>(count);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x * 0.25f, ImGui::GetStyle().ItemSpacing.y));
    for (int index = 0; index < count; ++index) {
        if (index > 0) ImGui::SameLine();
        const bool chosen = index == current;
        if (chosen) {
            ImGui::PushStyleColor(ImGuiCol_Button, Accent);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, AccentHover);
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, AccentActive);
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));
        }
        if (ImGui::Button(labels[index], ImVec2(each, ImGui::GetFrameHeight() * 1.15f)) && !chosen) {
            current = index;
            changed = true;
        }
        if (chosen) ImGui::PopStyleColor(4);
    }
    ImGui::PopStyleVar();
    ImGui::PopID();
    return changed;
}

enum class TopIcon { Versions, Rescan, Settings };

// A round icon button for the top bar, the icon drawn in code. Returns true when pressed.
bool IconButton(const char* id, TopIcon icon, const char* tooltip, bool disabled, bool spinning = false) {
    const float scale = ImGui::GetStyle().FontScaleDpi;
    const float size = ImGui::GetFrameHeight() * 1.2f;
    ImGui::BeginDisabled(disabled);
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const bool pressed = ImGui::InvisibleButton(id, ImVec2(size, size));
    const bool hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled);
    const bool held = ImGui::IsItemActive();
    ImGui::EndDisabled();
    if (hovered) ImGui::SetTooltip("%s", tooltip);
    auto* draw = ImGui::GetWindowDrawList();
    const ImVec2 c(at.x + size * 0.5f, at.y + size * 0.5f);
    const ImVec4 base = ImGui::GetStyle().Colors[ImGuiCol_Button];
    const ImVec4 back = held ? ImGui::GetStyle().Colors[ImGuiCol_ButtonActive] : hovered && !disabled ? ImGui::GetStyle().Colors[ImGuiCol_ButtonHovered] : base;
    draw->AddCircleFilled(c, size * 0.5f, ImGui::GetColorU32(back));
    const ImU32 ink = ImGui::GetColorU32(disabled ? ImGui::GetStyle().Colors[ImGuiCol_TextDisabled] : ImGui::GetStyle().Colors[ImGuiCol_Text]);
    const float r = size * 0.27f;
    const float line = std::max(1.5f, 1.7f * scale);
    switch (icon) {
    case TopIcon::Settings: {
        // A gear: eight teeth around a ring.
        constexpr int Teeth = 8;
        ImVec2 points[Teeth * 4];
        for (int i = 0; i < Teeth; ++i) {
            const float a = static_cast<float>(i) * 6.2831853f / Teeth;
            const float half = 6.2831853f / Teeth * 0.22f;
            const float outer = r * 1.18f;
            const float inner = r * 0.86f;
            const float spans[4] = {a - half * 1.6f, a - half, a + half, a + half * 1.6f};
            const float radii[4] = {inner, outer, outer, inner};
            for (int k = 0; k < 4; ++k) points[i * 4 + k] = ImVec2(c.x + std::cos(spans[k]) * radii[k], c.y + std::sin(spans[k]) * radii[k]);
        }
        draw->AddConcavePolyFilled(points, Teeth * 4, ink);
        draw->AddCircleFilled(c, r * 0.42f, ImGui::GetColorU32(back));
        break;
    }
    case TopIcon::Rescan: {
        // A circular arrow; it turns while a scan runs.
        const float turn = spinning ? static_cast<float>(ImGui::GetTime()) * 5.0f : 0.0f;
        const float start = -1.2f + turn;
        const float end = start + 4.9f;
        draw->PathArcTo(c, r, start, end, 24);
        draw->PathStroke(ink, 0, line);
        const ImVec2 tip(c.x + std::cos(end) * r, c.y + std::sin(end) * r);
        const ImVec2 along(-std::sin(end), std::cos(end));  // the arc's direction at its end
        const ImVec2 out(std::cos(end), std::sin(end));
        const float head = r * 0.55f;
        draw->AddTriangleFilled(ImVec2(tip.x + along.x * head * 0.8f, tip.y + along.y * head * 0.8f), ImVec2(tip.x + out.x * head * 0.6f, tip.y + out.y * head * 0.6f),
                                ImVec2(tip.x - out.x * head * 0.6f, tip.y - out.y * head * 0.6f), ink);
        break;
    }
    case TopIcon::Versions: {
        // Stacked layers: the AnyPS5 versions you can switch between.
        const float w = r * 1.15f;
        const float h = r * 0.58f;
        for (int i = 2; i >= 0; --i) {
            const float y = c.y - r * 0.55f + static_cast<float>(i) * r * 0.55f;
            const ImVec2 diamond[4] = {ImVec2(c.x, y - h), ImVec2(c.x + w, y), ImVec2(c.x, y + h), ImVec2(c.x - w, y)};
            draw->AddConvexPolyFilled(diamond, 4, ImGui::GetColorU32(back));
            draw->AddPolyline(diamond, 4, ink, ImDrawFlags_Closed, line);
        }
        break;
    }
    }
    return pressed && !disabled;
}

// A round avatar with the profile's first letter, its colour picked from the name.
void DrawProfileAvatar(const std::string& name, ImVec2 at, float size) {
    static const ImU32 colors[] = {IM_COL32(59, 130, 246, 255), IM_COL32(16, 185, 129, 255), IM_COL32(245, 158, 11, 255), IM_COL32(239, 68, 68, 255),
                                   IM_COL32(139, 92, 246, 255), IM_COL32(236, 72, 153, 255), IM_COL32(20, 184, 166, 255), IM_COL32(234, 88, 12, 255)};
    std::uint32_t hash = 2166136261u;
    for (const unsigned char c : name) hash = (hash ^ c) * 16777619u;
    const ImVec2 c(at.x + size * 0.5f, at.y + size * 0.5f);
    auto* draw = ImGui::GetWindowDrawList();
    draw->AddCircleFilled(c, size * 0.5f, colors[hash % std::size(colors)]);
    std::string letter = name.empty() ? std::string("?") : name.substr(0, 1);
    if (!name.empty() && (static_cast<unsigned char>(name[0]) & 0x80)) {
        std::size_t length = 1;  // a whole UTF-8 character
        while (length < name.size() && (static_cast<unsigned char>(name[length]) & 0xC0) == 0x80) ++length;
        letter = name.substr(0, length);
    } else if (!letter.empty()) {
        letter[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(letter[0])));
    }
    ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 0.95f * size / ImGui::GetFrameHeight());
    const ImVec2 text = ImGui::CalcTextSize(letter.c_str());
    draw->AddText(ImVec2(c.x - text.x * 0.5f, c.y - text.y * 0.5f), IM_COL32_WHITE, letter.c_str());
    ImGui::PopFont();
}

// A small chevron pointing down.
void Chevron(ImVec2 c, float size, ImU32 color) {
    const ImVec2 points[3] = {ImVec2(c.x - size, c.y - size * 0.45f), ImVec2(c.x, c.y + size * 0.5f), ImVec2(c.x + size, c.y - size * 0.45f)};
    ImGui::GetWindowDrawList()->AddPolyline(points, 3, color, ImDrawFlags_None, std::max(1.5f, 1.6f * ImGui::GetStyle().FontScaleDpi));
}

// A dialog's header: big title, a note under it, and a close button at the right. Returns true when closed.
bool DialogHeader(const char* title, const char* note) {
    const float scale = ImGui::GetStyle().FontScaleDpi;
    const float close = ImGui::GetFrameHeight();
    const float top = ImGui::GetCursorPosY();
    ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.45f);
    ImGui::TextUnformatted(title);
    ImGui::PopFont();
    if (note != nullptr && note[0] != 0) {
        ImGui::PushTextWrapPos(ImGui::GetContentRegionMax().x - close - ImGui::GetStyle().ItemSpacing.x);
        ImGui::TextColored(Dim, "%s", note);
        ImGui::PopTextWrapPos();
    }
    const float bottom = ImGui::GetCursorPosY();
    ImGui::SetCursorPos(ImVec2(ImGui::GetContentRegionMax().x - close, top));
    const bool pressed = ImGui::InvisibleButton("##close", ImVec2(close, close));
    const ImVec2 at = ImGui::GetItemRectMin();
    const ImVec2 end = ImGui::GetItemRectMax();
    auto* draw = ImGui::GetWindowDrawList();
    if (ImGui::IsItemHovered()) draw->AddRectFilled(at, end, ImGui::GetColorU32(ImGuiCol_ButtonHovered), close * 0.5f);
    const float inset = close * 0.32f;
    const ImU32 color = ImGui::GetColorU32(ImGui::IsItemHovered() ? ImGuiCol_Text : ImGuiCol_TextDisabled);
    draw->AddLine(ImVec2(at.x + inset, at.y + inset), ImVec2(end.x - inset, end.y - inset), color, 1.6f * scale);
    draw->AddLine(ImVec2(end.x - inset, at.y + inset), ImVec2(at.x + inset, end.y - inset), color, 1.6f * scale);
    ImGui::SetCursorPosY(std::max(bottom, top + close));
    ImGui::Spacing();
    return pressed;
}

// Begins a dialog that fills a share of the launcher's window and draws its own header.
bool BeginDialog(const char* name) {
    const float scale = ImGui::GetStyle().FontScaleDpi;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(22.0f * scale, 20.0f * scale));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 14.0f * scale);
    const bool open = ImGui::BeginPopupModal(name, nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
    ImGui::PopStyleVar(2);
    return open;
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

// A game's trophies as read in the background; set is empty when it has none.
struct TrophyLoad {
    std::string key;
    std::optional<TrophySet> set;
};

// A game's sce_sys/pic0.png, read in the background; empty when it has none.
struct ArtLoad {
    std::string key;
    std::vector<std::uint8_t> png;
};

ImVec4 GradeColor(TrophyGrade grade) {
    switch (grade) {
    case TrophyGrade::Platinum: return ImVec4(0.55f, 0.68f, 0.90f, 1.0f);
    case TrophyGrade::Gold: return ImVec4(0.89f, 0.70f, 0.25f, 1.0f);
    case TrophyGrade::Silver: return ImVec4(0.66f, 0.70f, 0.75f, 1.0f);
    case TrophyGrade::Bronze: return ImVec4(0.78f, 0.50f, 0.29f, 1.0f);
    }
    return ImVec4(1, 1, 1, 1);
}

// A trophy cup in the grade's colour, size by size, at the cursor. Hovering it names the grade.
void TrophyIcon(TrophyGrade grade, float size) {
    const ImVec2 at = ImGui::GetCursorScreenPos();
    auto* draw = ImGui::GetWindowDrawList();
    const ImU32 color = ImGui::GetColorU32(GradeColor(grade));
    const auto point = [&](float x, float y) { return ImVec2(at.x + x * size, at.y + y * size); };
    // Handles, then the bowl over them, the stem and a two-step base.
    draw->AddCircle(point(0.25f, 0.27f), size * 0.13f, color, 0, size * 0.075f);
    draw->AddCircle(point(0.75f, 0.27f), size * 0.13f, color, 0, size * 0.075f);
    draw->AddRectFilled(point(0.24f, 0.06f), point(0.76f, 0.50f), color, size * 0.26f, ImDrawFlags_RoundCornersBottom);
    draw->AddRectFilled(point(0.45f, 0.48f), point(0.55f, 0.70f), color);
    draw->AddRectFilled(point(0.33f, 0.68f), point(0.67f, 0.78f), color, size * 0.03f);
    draw->AddRectFilled(point(0.24f, 0.80f), point(0.76f, 0.93f), color, size * 0.03f);
    // A soft shine on the bowl.
    draw->AddRectFilled(point(0.33f, 0.12f), point(0.40f, 0.36f), ImGui::GetColorU32(ImVec4(1, 1, 1, 0.35f)), size * 0.035f);
    ImGui::Dummy(ImVec2(size, size));
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", TrophyGradeName(grade));
}

std::string LocalDate(std::int64_t seconds) {
    const std::time_t time = static_cast<std::time_t>(seconds);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &time);
#else
    localtime_r(&time, &local);
#endif
    char text[32];
    std::strftime(text, sizeof(text), "%Y-%m-%d %H:%M", &local);
    return text;
}

// The answer for one title from PROSPEROPatches, or why there is none.
struct OnlineLookup {
    std::string titleId;
    std::optional<TitleOnlineInfo> info;
    std::string error;
};

// Answers are kept for a day; a refresh asks again at once.
constexpr std::int64_t OnlineCacheAge = 24 * 60 * 60;

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

// One "trophy earned" card.
struct ToastItem {
    TrophyGrade grade = TrophyGrade::Bronze;
    std::string name;
    std::string detail;
    std::string game;
    std::vector<std::uint8_t> icon;
};

// A small always-on-top window that shows earned trophies over the game, one at a
// time, without taking the keyboard or the mouse from it. It has its own ImGui
// context and renderer; Frame() switches to them and back.
class TrophyToast {
public:
    explicit TrophyToast(float scale) : scale_(scale) {}

    ~TrophyToast() { Close(); }

    TrophyToast(const TrophyToast&) = delete;
    TrophyToast& operator=(const TrophyToast&) = delete;

    // area: the game's window, or empty for the primary display.
    void Push(ToastItem item, std::optional<SDL_Rect> area, bool sound) {
        queue_.push_back(std::move(item));
        if (area) area_ = area;
        if (queue_.size() == 1) shownAt_ = 0;
        if (sound) Chime();
    }

    void Frame() {
        if (queue_.empty()) {
            if (window_ != nullptr && visible_) {
                SDL_HideWindow(window_);
                visible_ = false;
            }
            return;
        }
        if (!Open()) {
            queue_.clear();
            return;
        }
        const Uint64 now = SDL_GetTicks();
        if (shownAt_ == 0) {
            shownAt_ = now;
            icons_->Clear();
        }
        const float elapsed = static_cast<float>(now - shownAt_) / 1000.0f;
        if (elapsed > Duration) {
            queue_.pop_front();
            shownAt_ = 0;
            return;
        }
        // Slide down from above, stay, slide back up.
        const float in = std::clamp(elapsed / Slide, 0.0f, 1.0f);
        const float out = std::clamp((Duration - elapsed) / Slide, 0.0f, 1.0f);
        const float shown = 1.0f - std::pow(1.0f - std::min(in, out), 3.0f);

        SDL_Rect area{};
        if (area_) {
            area = *area_;
        } else if (!SDL_GetDisplayUsableBounds(SDL_GetPrimaryDisplay(), &area)) {
            area = SDL_Rect{0, 0, 1280, 720};
        }
        const int width = static_cast<int>(440 * scale_);
        const int height = static_cast<int>(96 * scale_);
        const int margin = static_cast<int>(28 * scale_);
        const int x = area.x + (area.w - width) / 2;
        const int y = area.y + margin - static_cast<int>((1.0f - shown) * static_cast<float>(height + margin));
        SDL_SetWindowSize(window_, width, height);
        SDL_SetWindowPosition(window_, x, y);
        if (!visible_) {
            SDL_ShowWindow(window_);
            visible_ = true;
        }
        Draw(queue_.front(), elapsed);
    }

private:
    static constexpr float Duration = 6.0f;
    static constexpr float Slide = 0.35f;

    bool Open() {
        if (window_ != nullptr) return true;
        if (failed_) return false;
        SDL_SetHint(SDL_HINT_WINDOW_ACTIVATE_WHEN_SHOWN, "0");
        window_ = SDL_CreateWindow("Trophy", static_cast<int>(440 * scale_), static_cast<int>(96 * scale_),
                                   SDL_WINDOW_BORDERLESS | SDL_WINDOW_ALWAYS_ON_TOP | SDL_WINDOW_UTILITY | SDL_WINDOW_NOT_FOCUSABLE | SDL_WINDOW_HIDDEN |
                                       SDL_WINDOW_HIGH_PIXEL_DENSITY);
        if (window_ != nullptr) renderer_ = SDL_CreateRenderer(window_, nullptr);
        if (window_ == nullptr || renderer_ == nullptr) {
            failed_ = true;
            Close();
            return false;
        }
#ifdef _WIN32
        win::RoundWindowCorners(SDL_GetPointerProperty(SDL_GetWindowProperties(window_), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
#endif
        icons_ = std::make_unique<IconCache>(renderer_);
        auto* previous = ImGui::GetCurrentContext();
        context_ = ImGui::CreateContext();
        ImGui::SetCurrentContext(context_);
        auto& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.ConfigFlags |= ImGuiConfigFlags_NoMouse;
        auto& style = ImGui::GetStyle();
        style.WindowPadding = ImVec2(0, 0);
        style.WindowBorderSize = 0;
        style.WindowRounding = 0;
        style.FontScaleDpi = scale_;
        style.Colors[ImGuiCol_Text] = ImVec4(0.95f, 0.96f, 0.98f, 1.0f);
        LoadFonts(io);
        ImGui_ImplSDL3_InitForSDLRenderer(window_, renderer_);
        ImGui_ImplSDLRenderer3_Init(renderer_);
        ImGui::SetCurrentContext(previous);
        return true;
    }

    void Close() {
        if (context_ != nullptr) {
            auto* previous = ImGui::GetCurrentContext();
            ImGui::SetCurrentContext(context_);
            ImGui_ImplSDLRenderer3_Shutdown();
            ImGui_ImplSDL3_Shutdown();
            ImGui::DestroyContext(context_);
            ImGui::SetCurrentContext(previous == context_ ? nullptr : previous);
            context_ = nullptr;
        }
        icons_.reset();
        if (renderer_ != nullptr) SDL_DestroyRenderer(renderer_);
        if (window_ != nullptr) SDL_DestroyWindow(window_);
        renderer_ = nullptr;
        window_ = nullptr;
        visible_ = false;
        if (audio_ != nullptr) SDL_DestroyAudioStream(audio_);
        audio_ = nullptr;
    }

    void Draw(const ToastItem& item, float elapsed) {
        auto* previous = ImGui::GetCurrentContext();
        ImGui::SetCurrentContext(context_);
        ImGui_ImplSDLRenderer3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->Pos);
        ImGui::SetNextWindowSize(viewport->Size);
        ImGui::Begin("toast", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoSavedSettings);
        auto* draw = ImGui::GetWindowDrawList();
        const ImVec2 at = viewport->Pos;
        const ImVec2 size = viewport->Size;
        const float pad = 14.0f * scale_;
        const ImU32 grade = ImGui::GetColorU32(GradeColor(item.grade));
        draw->AddRectFilled(at, ImVec2(at.x + size.x, at.y + size.y), IM_COL32(24, 26, 32, 255));
        draw->AddRectFilled(at, ImVec2(at.x + 5.0f * scale_, at.y + size.y), grade);
        // Time left, as a thin line along the bottom.
        const float left = std::clamp(1.0f - elapsed / Duration, 0.0f, 1.0f);
        draw->AddRectFilled(ImVec2(at.x, at.y + size.y - 3.0f * scale_), ImVec2(at.x + size.x * left, at.y + size.y), ImGui::GetColorU32(Fade(GradeColor(item.grade), 0.6f)));

        const float icon = size.y - pad * 2;
        const ImVec2 iconAt(at.x + pad + 5.0f * scale_, at.y + pad);
        if (auto* texture = icons_->Get(std::filesystem::path("toast"), item.icon)) {
            draw->AddImageRounded(static_cast<ImTextureID>(reinterpret_cast<std::intptr_t>(texture)), iconAt, ImVec2(iconAt.x + icon, iconAt.y + icon), ImVec2(0, 0),
                                  ImVec2(1, 1), IM_COL32_WHITE, 8.0f * scale_);
        } else {
            draw->AddRectFilled(iconAt, ImVec2(iconAt.x + icon, iconAt.y + icon), IM_COL32(48, 52, 62, 255), 8.0f * scale_);
            ImGui::SetCursorScreenPos(ImVec2(iconAt.x + icon * 0.2f, iconAt.y + icon * 0.2f));
            TrophyIcon(item.grade, icon * 0.6f);
        }
        const float textX = iconAt.x + icon + pad;
        const float textWidth = at.x + size.x - pad - textX;
        ImGui::SetCursorScreenPos(ImVec2(textX, at.y + pad - 1.0f * scale_));
        TrophyIcon(item.grade, ImGui::GetFontSize());
        ImGui::SameLine(0, 6.0f * scale_);
        ImGui::TextColored(GradeColor(item.grade), "Trophy earned");
        ImGui::SetCursorScreenPos(ImVec2(textX, ImGui::GetCursorScreenPos().y));
        ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.15f);
        ImGui::TextUnformatted(Elide(item.name.empty() ? std::string("A trophy") : item.name, textWidth).c_str());
        ImGui::PopFont();
        ImGui::SetCursorScreenPos(ImVec2(textX, ImGui::GetCursorScreenPos().y));
        ImGui::TextColored(ImVec4(0.62f, 0.65f, 0.72f, 1.0f), "%s", Elide(item.detail.empty() ? item.game : item.detail, textWidth).c_str());
        ImGui::End();
        ImGui::Render();
        SDL_SetRenderScale(renderer_, ImGui::GetIO().DisplayFramebufferScale.x, ImGui::GetIO().DisplayFramebufferScale.y);
        SDL_SetRenderDrawColor(renderer_, 24, 26, 32, 255);
        SDL_RenderClear(renderer_);
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer_);
        SDL_RenderPresent(renderer_);
        ImGui::SetCurrentContext(previous);
    }

    // Two short bell-like notes.
    void Chime() {
        if (audio_ == nullptr) {
            if (!SDL_WasInit(SDL_INIT_AUDIO) && !SDL_InitSubSystem(SDL_INIT_AUDIO)) return;
            const SDL_AudioSpec spec{SDL_AUDIO_F32, 1, 48000};
            audio_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
            if (audio_ == nullptr) return;
            SDL_ResumeAudioStreamDevice(audio_);
        }
        constexpr int Rate = 48000;
        std::vector<float> samples(static_cast<std::size_t>(Rate * 0.75));
        const auto note = [&](float start, float frequency, float level) {
            const auto first = static_cast<std::size_t>(start * Rate);
            for (std::size_t i = first; i < samples.size(); ++i) {
                const float t = static_cast<float>(i - first) / Rate;
                const float envelope = std::min(1.0f, t * 200.0f) * std::exp(-t * 6.0f);
                samples[i] += level * envelope * (std::sin(6.2831853f * frequency * t) + 0.3f * std::sin(6.2831853f * frequency * 2.0f * t));
            }
        };
        note(0.0f, 987.77f, 0.10f);   // B5
        note(0.12f, 1318.5f, 0.10f);  // E6
        SDL_PutAudioStreamData(audio_, samples.data(), static_cast<int>(samples.size() * sizeof(float)));
    }

    float scale_;
    SDL_Window* window_ = nullptr;
    SDL_Renderer* renderer_ = nullptr;
    ImGuiContext* context_ = nullptr;
    std::unique_ptr<IconCache> icons_;
    SDL_AudioStream* audio_ = nullptr;
    std::deque<ToastItem> queue_;
    std::optional<SDL_Rect> area_;
    Uint64 shownAt_ = 0;
    bool visible_ = false;
    bool failed_ = false;
};

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
        toast_ = std::make_unique<TrophyToast>(scale);
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
            trophyAsked_.clear();
            artAsked_.clear();
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
        if (auto load = trophyLoader_.Take()) trophies_[load->key] = std::move(load->set);
        if (auto load = artLoader_.Take()) art_[load->key] = std::move(load->png);
        StartGameLoads();
        if (auto lookup = onlineLoader_.Take()) {
            if (lookup->info) online_[lookup->titleId] = std::move(*lookup->info);
            if (lookup->error.empty()) onlineErrors_.erase(lookup->titleId);
            else onlineErrors_[lookup->titleId] = lookup->error;
        }
        StartOnlineLookup();
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
        DrawEditProfile();
        DrawMissingRelinker();
        DrawVersions();
        DrawSettings();
        DrawTrophies();
        if (controlsScope_ == 0) DrawControls();  // the profile's controls are drawn over Settings
        ImGui::End();
    }

    bool TakeEvent(const SDL_Event& event) { return CaptureEvent(event); }

    // Runs every loop, even while the launcher is minimized: trophy cards over the game.
    void Tick() {
        WatchTrophies(false);
        toast_->Frame();
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
        WatchTrophies(true);  // an unlock in the last moments
        watchPrimed_ = false;
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
        trophyAsked_.clear();  // the run may have earned some
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

    // The profile as a pill: avatar, name and a chevron; a menu lists the others.
    void DrawProfileSelector() {
        const auto& style = ImGui::GetStyle();
        const float scale = style.FontScaleDpi;
        const float height = ImGui::GetFrameHeight() * 1.2f;
        const float avatar = height - 8.0f * scale;
        const float nameWidth = std::min(ImGui::CalcTextSize(layout_.profile.c_str()).x, ImGui::GetFontSize() * 9);
        const float width = 4.0f * scale + avatar + 8.0f * scale + nameWidth + 10.0f * scale + ImGui::GetFontSize() * 0.6f + 12.0f * scale;
        const bool locked = session_ != nullptr;
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() - (height - ImGui::GetFrameHeight()) * 0.5f);
        const ImVec2 at = ImGui::GetCursorScreenPos();
        ImGui::BeginDisabled(locked);
        const bool pressed = ImGui::InvisibleButton("profile", ImVec2(width, height));
        const bool hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled);
        ImGui::EndDisabled();
        if (hovered) ImGui::SetTooltip("%s", locked ? "The profile can't change while a game runs" : "Profile: saves, trophies, controls and language");
        if (pressed && !locked) ImGui::OpenPopup("profile-menu");
        auto* draw = ImGui::GetWindowDrawList();
        const ImVec2 end(at.x + width, at.y + height);
        const bool open = ImGui::IsPopupOpen("profile-menu");
        draw->AddRectFilled(at, end, ImGui::GetColorU32(open ? style.Colors[ImGuiCol_ButtonActive] : hovered && !locked ? style.Colors[ImGuiCol_ButtonHovered] : style.Colors[ImGuiCol_Button]),
                           height * 0.5f);
        DrawProfileAvatar(layout_.profile, ImVec2(at.x + 4.0f * scale, at.y + 4.0f * scale), avatar);
        const ImU32 ink = ImGui::GetColorU32(locked ? style.Colors[ImGuiCol_TextDisabled] : style.Colors[ImGuiCol_Text]);
        const float textX = at.x + 4.0f * scale + avatar + 8.0f * scale;
        draw->AddText(ImVec2(textX, at.y + (height - ImGui::GetFontSize()) * 0.5f), ink, Elide(layout_.profile, nameWidth).c_str());
        Chevron(ImVec2(end.x - 12.0f * scale - ImGui::GetFontSize() * 0.3f, at.y + height * 0.5f), ImGui::GetFontSize() * 0.28f, ImGui::GetColorU32(Dim));
        ImGui::SetNextWindowPos(ImVec2(at.x, end.y + 4.0f * scale));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f * scale, 8.0f * scale));
        ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, 12.0f * scale);
        if (ImGui::BeginPopup("profile-menu")) {
            ImGui::TextColored(Dim, "Profiles");
            const float row = ImGui::GetFrameHeight() * 1.25f;
            const float menuWidth = std::max(ImGui::GetFontSize() * 14, width);
            auto names = profiles_;
            if (std::find(names.begin(), names.end(), layout_.profile) == names.end()) names.insert(names.begin(), layout_.profile);
            std::string chosen;
            bool create = false;
            // kind: 0 a profile, 1 "new" (plus), 2 "edit" (pencil).
            const auto item = [&](const std::string& id, const std::string& label, bool current, int kind) {
                const ImVec2 rowAt = ImGui::GetCursorScreenPos();
                const bool clicked = ImGui::Selectable(("##" + id).c_str(), false, ImGuiSelectableFlags_None, ImVec2(menuWidth, row));
                const float size = row - 8.0f * scale;
                if (kind != 0) {
                    auto* menuDraw = ImGui::GetWindowDrawList();
                    const ImVec2 c(rowAt.x + 4.0f * scale + size * 0.5f, rowAt.y + row * 0.5f);
                    const ImU32 ink = ImGui::GetColorU32(ImGuiCol_Text);
                    menuDraw->AddCircle(c, size * 0.5f - 1.0f, ImGui::GetColorU32(Dim), 0, 1.2f * scale);
                    if (kind == 1) {
                        menuDraw->AddLine(ImVec2(c.x - size * 0.22f, c.y), ImVec2(c.x + size * 0.22f, c.y), ink, 1.6f * scale);
                        menuDraw->AddLine(ImVec2(c.x, c.y - size * 0.22f), ImVec2(c.x, c.y + size * 0.22f), ink, 1.6f * scale);
                    } else {
                        // A pencil, tip at the bottom left.
                        const float k = size * 0.2f;
                        menuDraw->AddLine(ImVec2(c.x - k, c.y + k), ImVec2(c.x + k * 0.9f, c.y - k * 0.9f), ink, 3.2f * scale);
                        menuDraw->AddTriangleFilled(ImVec2(c.x - k * 1.25f, c.y + k * 1.25f), ImVec2(c.x - k * 1.15f, c.y + k * 0.55f), ImVec2(c.x - k * 0.55f, c.y + k * 1.15f), ink);
                    }
                } else {
                    DrawProfileAvatar(label, ImVec2(rowAt.x + 4.0f * scale, rowAt.y + 4.0f * scale), size);
                }
                auto* menuDraw = ImGui::GetWindowDrawList();
                menuDraw->AddText(ImVec2(rowAt.x + 4.0f * scale + size + 10.0f * scale, rowAt.y + (row - ImGui::GetFontSize()) * 0.5f),
                                  ImGui::GetColorU32(current ? Accent : style.Colors[ImGuiCol_Text]), label.c_str());
                if (current) {
                    // A check mark at the right.
                    const float x = rowAt.x + menuWidth - 14.0f * scale;
                    const float y = rowAt.y + row * 0.5f;
                    const float k = ImGui::GetFontSize() * 0.3f;
                    const ImVec2 check[3] = {ImVec2(x - k * 1.4f, y), ImVec2(x - k * 0.5f, y + k * 0.9f), ImVec2(x + k * 0.9f, y - k * 0.8f)};
                    menuDraw->AddPolyline(check, 3, ImGui::GetColorU32(Accent), ImDrawFlags_None, 2.0f * scale);
                }
                return clicked;
            };
            bool edit = false;
            for (const auto& name : names) {
                if (item(name, name, name == layout_.profile, 0) && name != layout_.profile) chosen = name;
            }
            ImGui::Separator();
            if (item("edit", "Edit " + layout_.profile + "...", false, 2)) edit = true;
            if (item("new", "New profile...", false, 1)) create = true;
            ImGui::EndPopup();
            if (edit) OpenEditProfile();
            if (!chosen.empty()) SwitchProfile(chosen);
            if (create) {
                newProfileName_.clear();
                newProfileError_.clear();
                openNewProfile_ = true;
            }
        }
        ImGui::PopStyleVar(2);
    }

    void DrawTopBar() {
        const auto& style = ImGui::GetStyle();
        ImGui::PushFont(nullptr, style.FontSizeBase * 1.45f);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Library");
        ImGui::PopFont();
        ImGui::SameLine(0, style.ItemSpacing.x * 2);

        DrawProfileSelector();
        // The search box takes the room left before the buttons, and its own row when there's too little.
        const float buttons = ImGui::GetFrameHeight() * 1.2f * 3 + style.ItemSpacing.x * 2;
        ImGui::SameLine();
        const float room = ImGui::GetContentRegionMax().x - buttons - ImGui::GetCursorPosX() - style.ItemSpacing.x;
        const bool searchInline = room >= ImGui::GetFontSize() * 7;
        if (searchInline) {
            ImGui::SetNextItemWidth(std::min(room, ImGui::GetFontSize() * 16));
            ImGui::InputTextWithHint("##search", "Search name or serial", &search_);
            ImGui::SameLine();
        }
        ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), ImGui::GetContentRegionMax().x - buttons));
        // The icons sit a little lower so their centres line up with the search box.
        const float iconY = ImGui::GetCursorPosY() - (ImGui::GetFrameHeight() * 1.2f - ImGui::GetFrameHeight()) * 0.5f;
        ImGui::SetCursorPosY(iconY);
        if (IconButton("versions", TopIcon::Versions, session_ != nullptr ? "AnyPS5 versions (not while a game runs)" : "AnyPS5 versions", session_ != nullptr)) OpenVersions();
        ImGui::SameLine();
        ImGui::SetCursorPosY(iconY);
        const char* rescanTip = scan_.Running() ? "Scanning..." : settings_.gamesFolder.empty() ? "Rescan (set a games folder in Settings first)" : "Rescan the games folder";
        if (IconButton("rescan", TopIcon::Rescan, rescanTip, scan_.Running() || job_.Busy() || settings_.gamesFolder.empty(), scan_.Running())) StartScan();
        ImGui::SameLine();
        ImGui::SetCursorPosY(iconY);
        if (IconButton("settings", TopIcon::Settings, session_ != nullptr ? "Settings (not while a game runs)" : "Settings", session_ != nullptr || job_.Busy())) OpenSettings();
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

    std::filesystem::path OnlineCacheFolder() const {
        return layout_.library / "cache" / "prosperopatches";
    }

    // Looks up the selected game on PROSPEROPatches, one title at a time.
    void StartOnlineLookup() {
        if (!settings_.onlineInfo || selected_.empty() || onlineLoader_.Running()) return;
        const auto* game = SelectedGame();
        if (game == nullptr) return;
        const auto titleId = game->metadata.titleId;
        const bool refresh = onlineRefresh_ == titleId;
        if (!refresh && onlineAsked_.count(titleId) != 0) return;
        onlineAsked_.insert(titleId);
        onlineRefresh_.clear();
        const auto folder = OnlineCacheFolder();
        onlineLoader_.Start([titleId, folder, refresh] {
            OnlineLookup lookup{titleId, std::nullopt, {}};
            const auto now = UnixNow();
            if (!refresh) {
                if (auto cached = LoadCachedTitle(folder, titleId, now, OnlineCacheAge)) {
                    lookup.info = std::move(cached);
                    return lookup;
                }
            }
            try {
                auto info = FetchTitleOnline(titleId, HttpGetText, HttpPostText, now);
                try {
                    SaveCachedTitle(folder, info);
                } catch (const std::exception&) {
                    // Not being able to cache only means asking again next time.
                }
                lookup.info = std::move(info);
            } catch (const std::exception& failure) {
                lookup.error = ReadableText(failure.what());
            }
            return lookup;
        });
    }

    void DrawStore(const GameEntry& game) {
        const auto& titleId = game.metadata.titleId;
        const auto found = online_.find(titleId);
        const TitleOnlineInfo* info = found == online_.end() ? nullptr : &found->second;
        const auto& contentId = !game.metadata.contentId.empty() ? game.metadata.contentId : (info != nullptr ? info->contentId : std::string());
        auto region = RegionOf(contentId);
        if (region.empty() && info != nullptr) region = info->region;
        const auto row = [](const char* label, const std::string& value) {
            if (value.empty()) return;
            ImGui::TextColored(Dim, "%s", label);
            ImGui::SameLine(ImGui::GetFontSize() * 6.0f);
            ImGui::TextUnformatted(value.c_str());
        };
        row("Region", region);
        if (info != nullptr) row("Publisher", info->publisher);

        if (!settings_.onlineInfo) {
            ImGui::TextColored(Dim, "Online lookups are off (Settings).");
        } else if (info == nullptr) {
            if (const auto error = onlineErrors_.find(titleId); error != onlineErrors_.end()) {
                ImGui::TextColored(Dim, "Can't look up updates: %s", error->second.c_str());
            } else {
                ImGui::TextColored(Dim, "Looking up updates...");
            }
        } else if (!info->found) {
            ImGui::TextColored(Dim, "PROSPEROPatches doesn't list this title.");
        } else if (const auto* newest = NewestPatch(*info)) {
            const int order = CompareVersions(game.metadata.contentVersion, newest->contentVersion);
            if (order < 0) {
                ImGui::TextColored(Amber, "Update available: %s", newest->contentVersion.c_str());
            } else if (order == 0) {
                ImGui::TextColored(Green, "Up to date (newest is %s)", newest->contentVersion.c_str());
            } else {
                ImGui::TextColored(Dim, "Newer than the newest listed update (%s)", newest->contentVersion.c_str());
            }
            const std::string header = "All updates (" + std::to_string(info->patches.size()) + ")###updates";
            if (ImGui::CollapsingHeader(header.c_str())) {
                ImGui::PushTextWrapPos(-1.0f);  // one line per update, even in a narrow panel
                if (ImGui::BeginTable("updates", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
                    ImGui::TableSetupColumn("Version");
                    ImGui::TableSetupColumn("Size");
                    ImGui::TableSetupColumn("Firmware");
                    ImGui::TableSetupColumn("Added");
                    ImGui::TableHeadersRow();
                    for (const auto& patch : info->patches) {
                        ImGui::TableNextRow();
                        // The game's own version: the whole row in the accent colour.
                        const bool current = CompareVersions(patch.contentVersion, game.metadata.contentVersion) == 0;
                        if (current) {
                            ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, ImGui::GetColorU32(Fade(Accent, 0.22f)));
                            ImGui::PushStyleColor(ImGuiCol_Text, Accent);
                        }
                        ImGui::TableNextColumn();
                        ImGui::TextUnformatted(patch.contentVersion.c_str());
                        if (current && ImGui::IsItemHovered()) ImGui::SetTooltip("Your version");
                        ImGui::TableNextColumn();
                        ImGui::TextUnformatted(patch.size.c_str());
                        ImGui::TableNextColumn();
                        ImGui::TextUnformatted(patch.requiredFirmware.c_str());
                        ImGui::TableNextColumn();
                        ImGui::TextUnformatted(patch.importDate.substr(0, 10).c_str());
                        if (current) ImGui::PopStyleColor();
                    }
                    ImGui::EndTable();
                }
                ImGui::PopTextWrapPos();
            }
        } else {
            ImGui::TextColored(Dim, "No updates are listed for this title.");
        }
        if (ImGui::Button("View on PROSPEROPatches")) SDL_OpenURL(PatchPageUrl(titleId).c_str());
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Changelogs, update downloads and add-ons, in your browser");
        if (settings_.onlineInfo) {
            ImGui::SameLine();
            ImGui::BeginDisabled(onlineLoader_.Running());
            if (ImGui::Button("Refresh")) {
                onlineErrors_.erase(titleId);
                online_.erase(titleId);
                onlineRefresh_ = titleId;
            }
            ImGui::EndDisabled();
        }
    }

    static void DrawChanges(const GameEntry& game) {
        if (game.changes.empty()) return;
        const auto* current = ChangeNoteFor(game.changes, game.metadata.contentVersion);
        const auto& shown = current != nullptr ? *current : game.changes.front();
        ImGui::TextColored(Dim, "%s %s", current != nullptr ? "This version," : "Newest notes,", shown.version.c_str());
        ImGui::TextUnformatted(shown.text.c_str());
        if (game.changes.size() > 1 && ImGui::CollapsingHeader("Earlier notes")) {
            for (const auto& note : game.changes) {
                if (&note == &shown) continue;
                ImGui::TextColored(Dim, "%s", note.version.c_str());
                ImGui::TextUnformatted(note.text.c_str());
                ImGui::Spacing();
            }
        }
    }

    // ---- trophies while playing ---------------------------------------------------

    // Polls the running game's _trophy folder (AnyPS5 writes each unlock there at
    // once) and shows a card for every unlock that wasn't there at the start.
    void WatchTrophies(bool now) {
        if (!session_) {
            watchPrimed_ = false;
            return;
        }
        const Uint64 ticks = SDL_GetTicks();
        if (!now && ticks < nextWatch_) return;
        nextWatch_ = ticks + 500;
        const auto& home = session_->Plan().workingDirectory;
        const auto records = ReadTrophyRecords(home / "_trophy");
        if (!watchPrimed_) {
            seenUnlocks_ = records.unlocks;
            watchPrimed_ = true;
            runTrophies_.reset();
            for (const auto& game : result_.games) {
                if (game.home.empty() || game.home != home) continue;
                try {
                    auto package = OpenGamePackage(game);
                    runTrophies_ = LoadTrophies(*package, TrophyLanguages(profileSettings_.language));
                } catch (const std::exception&) {
                }
                break;
            }
            return;
        }
        for (const auto& [key, tick] : records.unlocks) {
            if (!seenUnlocks_.emplace(key, tick).second || !settings_.trophyPopups) continue;
            ToastItem item;
            item.game = session_->Name();
            item.name = "Trophy " + std::to_string(key.second);
            if (runTrophies_) {
                for (const auto& trophy : runTrophies_->trophies) {
                    if (trophy.label != key.first || trophy.id != key.second) continue;
                    item.grade = trophy.grade;
                    item.name = trophy.name.empty() ? item.name : trophy.name;
                    item.detail = trophy.detail;
                    item.icon = trophy.icon;
                    break;
                }
            }
            std::optional<SDL_Rect> area;
            SDL_Rect rect{};
            if (session_->WindowRect(rect.x, rect.y, rect.w, rect.h) && rect.w > 0 && rect.h > 0) area = rect;
            toast_->Push(std::move(item), area, settings_.trophySound);
        }
    }

    // ---- artwork and trophies -----------------------------------------------------

    static std::string GameKey(const GameEntry& game) { return ToUtf8(game.location); }

    std::filesystem::path TrophyRecordsFor(const GameEntry& game) const {
        if (!game.home.empty() && HasSession(game.home)) return game.home / "_trophy";
        return layout_.Trophies(game.metadata.titleId);
    }

    // Reads the selected game's artwork and trophies, one game at a time.
    void StartGameLoads() {
        const auto* game = SelectedGame();
        if (game == nullptr) return;
        const auto key = GameKey(*game);
        if (!artLoader_.Running() && artAsked_.insert(key).second) {
            auto copy = *game;
            artLoader_.Start([copy, key] {
                ArtLoad load{key, {}};
                try {
                    auto package = OpenGamePackage(copy);
                    if (auto png = package->ReadAll("sce_sys/pic0.png", 32u << 20)) load.png = std::move(*png);
                } catch (const std::exception&) {
                }
                return load;
            });
        }
        if (!trophyLoader_.Running() && trophyAsked_.insert(key).second) {
            auto copy = *game;
            const auto records = TrophyRecordsFor(*game);
            const auto languages = TrophyLanguages(profileSettings_.language);
            trophyLoader_.Start([copy, key, records, languages] {
                TrophyLoad load{key, std::nullopt};
                try {
                    auto package = OpenGamePackage(copy);
                    load.set = LoadTrophies(*package, languages);
                    if (load.set) ApplyTrophyRecords(*load.set, ReadTrophyRecords(records));
                } catch (const std::exception&) {
                    load.set.reset();
                }
                return load;
            });
        }
    }

    // The game's pic0.png behind the details panel, faded into the card toward the bottom.
    void DrawArtwork(const GameEntry& game) {
        const auto found = art_.find(GameKey(game));
        if (found == art_.end() || found->second.empty()) return;
        auto* texture = icons_->Get(game.location / "pic0.png", found->second);
        if (texture == nullptr) return;
        float width = 0;
        float height = 0;
        SDL_GetTextureSize(texture, &width, &height);
        if (width <= 0 || height <= 0) return;
        const ImVec2 at = ImGui::GetWindowPos();
        const ImVec2 size = ImGui::GetWindowSize();
        const float shown = std::min(size.y, size.x * 0.75f);  // the top of the panel only
        // Cover: crop the picture to the shown area's shape.
        ImVec2 uv0(0, 0);
        ImVec2 uv1(1, 1);
        const float aspect = size.x / shown;
        if (width / height > aspect) {
            const float cut = (1.0f - aspect * height / width) * 0.5f;
            uv0.x = cut;
            uv1.x = 1.0f - cut;
        } else {
            const float cut = (1.0f - width / (aspect * height)) * 0.5f;
            uv0.y = cut;
            uv1.y = 1.0f - cut;
        }
        auto* draw = ImGui::GetWindowDrawList();
        draw->PushClipRect(at, ImVec2(at.x + size.x, at.y + size.y), false);
        const float rounding = ImGui::GetStyle().ChildRounding * scale_;
        const bool dark = WantDark(settings_.theme);
        draw->AddImageRounded(static_cast<ImTextureID>(reinterpret_cast<std::intptr_t>(texture)), at, ImVec2(at.x + size.x, at.y + shown), uv0, uv1,
                              ImGui::GetColorU32(ImVec4(1, 1, 1, dark ? 0.55f : 0.40f)), rounding, ImDrawFlags_RoundCornersTop);
        const ImU32 clear = ImGui::GetColorU32(Fade(Card, 0.25f));
        const ImU32 solid = ImGui::GetColorU32(Card);
        draw->AddRectFilledMultiColor(ImVec2(at.x, at.y + shown * 0.35f), ImVec2(at.x + size.x, at.y + shown), clear, clear, solid, solid);
        draw->AddRectFilled(at, ImVec2(at.x + size.x, at.y + shown * 0.35f), clear, rounding, ImDrawFlags_RoundCornersTop);
        draw->PopClipRect();
    }

    const TrophySet* TrophiesOf(const GameEntry& game, bool& loading) const {
        const auto found = trophies_.find(GameKey(game));
        loading = found == trophies_.end();
        return found == trophies_.end() || !found->second ? nullptr : &*found->second;
    }

    static void ProgressBar(float fraction, float width, ImVec4 color) {
        const float height = ImGui::GetFontSize() * 0.4f;
        const ImVec2 at = ImGui::GetCursorScreenPos();
        auto* draw = ImGui::GetWindowDrawList();
        draw->AddRectFilled(at, ImVec2(at.x + width, at.y + height), ImGui::GetColorU32(Fade(Dim, 0.25f)), height * 0.5f);
        if (fraction > 0) draw->AddRectFilled(at, ImVec2(at.x + width * std::min(fraction, 1.0f), at.y + height), ImGui::GetColorU32(color), height * 0.5f);
        ImGui::Dummy(ImVec2(width, height));
    }

    void DrawGradeCounts(const TrophySet& set) {
        bool first = true;
        for (const auto grade : {TrophyGrade::Platinum, TrophyGrade::Gold, TrophyGrade::Silver, TrophyGrade::Bronze}) {
            const auto count = set.Count(grade);
            if (count.total == 0) continue;
            const auto text = std::to_string(count.earned) + "/" + std::to_string(count.total);
            const float icon = ImGui::GetFontSize() * 1.15f;
            if (!first) {
                ImGui::SameLine(0, ImGui::GetFontSize() * 1.2f);
                if (ImGui::GetContentRegionAvail().x < icon + ImGui::CalcTextSize(text.c_str()).x + ImGui::GetFontSize() * 0.3f) ImGui::NewLine();
            }
            first = false;
            ImGui::BeginGroup();
            TrophyIcon(grade, icon);
            ImGui::SameLine(0, ImGui::GetFontSize() * 0.3f);
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (icon - ImGui::GetFontSize()) * 0.5f);
            ImGui::TextColored(GradeColor(grade), "%s", text.c_str());
            ImGui::EndGroup();
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s: %s earned", TrophyGradeName(grade), text.c_str());
        }
    }

    void DrawTrophySummary(const GameEntry& game) {
        bool loading = false;
        const auto* set = TrophiesOf(game, loading);
        if (loading) {
            ImGui::TextColored(Dim, "Reading the trophy list...");
            return;
        }
        if (set == nullptr) {
            ImGui::TextColored(Dim, "No trophy list in the game files (sce_sys/trophy2).");
            return;
        }
        const auto count = set->Count();
        ImGui::Text("%d of %d earned  \xC2\xB7  %d%%", count.earned, count.total, set->Percent());
        ProgressBar(static_cast<float>(set->Percent()) / 100.0f, ImGui::GetContentRegionAvail().x, Accent);
        DrawGradeCounts(*set);
    }

    void OpenTrophies() {
        trophyFilter_ = 0;
        openTrophies_ = true;
        // Read the records again: a run may have changed them.
        if (const auto* game = SelectedGame()) trophyAsked_.erase(GameKey(*game));
    }

    void DrawTrophies() {
        if (openTrophies_) {
            ImGui::OpenPopup("Trophies");
            openTrophies_ = false;
        }
        FollowWindow(0.70f, 0.86f);
        if (!BeginDialog("Trophies")) return;
        const auto* game = SelectedGame();
        bool loading = false;
        const auto* set = game != nullptr ? TrophiesOf(*game, loading) : nullptr;
        const std::string title = game == nullptr ? std::string("Trophies") : "Trophies: " + game->metadata.name;
        const std::string note = set == nullptr ? std::string() : std::to_string(set->Count().earned) + " of " + std::to_string(set->Count().total) +
                                                                  " earned, " + std::to_string(set->Percent()) + "%. Kept per profile (" + layout_.profile +
                                                                  "); the list updates when the game closes.";
        if (DialogHeader(title.c_str(), note.c_str())) ImGui::CloseCurrentPopup();
        if (set == nullptr) {
            ImGui::TextColored(Dim, "%s", loading ? "Reading the trophy list..." : "This game has no trophy list in its files (sce_sys/trophy2).");
            ImGui::EndPopup();
            return;
        }
        ProgressBar(static_cast<float>(set->Percent()) / 100.0f, ImGui::GetContentRegionAvail().x, Accent);
        ImGui::Spacing();
        DrawGradeCounts(*set);
        ImGui::Spacing();
        const char* filters[] = {"All", "Earned", "Not earned"};
        Segmented("trophy-filter", filters, 3, trophyFilter_);
        ImGui::SameLine(0, ImGui::GetFontSize());
        ImGui::Checkbox("Show hidden trophies", &showHidden_);
        ImGui::Spacing();

        ImGui::BeginChild("trophy-list", ImVec2(0, 0), ImGuiChildFlags_None);
        const float icon = ImGui::GetFontSize() * 3.2f;
        if (ImGui::BeginTable("trophies", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_PadOuterX | ImGuiTableFlags_SizingFixedFit)) {
            ImGui::TableSetupColumn("icon", ImGuiTableColumnFlags_WidthFixed, icon);
            ImGui::TableSetupColumn("text", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("state", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 9.5f);
            for (const auto& trophy : set->trophies) {
                const bool earned = trophy.earnedAt.has_value();
                if ((trophyFilter_ == 1 && !earned) || (trophyFilter_ == 2 && earned)) continue;
                const bool secret = trophy.hidden && !earned && !showHidden_;
                ImGui::PushID(static_cast<int>(trophy.label * 100000 + static_cast<std::uint32_t>(trophy.id)));
                ImGui::TableNextRow(ImGuiTableRowFlags_None, icon + ImGui::GetStyle().CellPadding.y * 2);
                ImGui::TableNextColumn();
                const ImVec2 at = ImGui::GetCursorScreenPos();
                auto* draw = ImGui::GetWindowDrawList();
                const auto key = game->location / ("trophy-" + std::to_string(trophy.label) + "-" + std::to_string(trophy.id));
                auto* texture = secret ? nullptr : icons_->Get(key, trophy.icon);
                if (texture != nullptr) {
                    draw->AddImageRounded(static_cast<ImTextureID>(reinterpret_cast<std::intptr_t>(texture)), at, ImVec2(at.x + icon, at.y + icon), ImVec2(0, 0), ImVec2(1, 1),
                                          ImGui::GetColorU32(ImVec4(1, 1, 1, earned ? 1.0f : 0.35f)), 8.0f * scale_);
                } else {
                    draw->AddRectFilled(at, ImVec2(at.x + icon, at.y + icon), ImGui::GetColorU32(Placeholder), 8.0f * scale_);
                    if (secret) {
                        const ImVec2 size = ImGui::CalcTextSize("?");
                        draw->AddText(ImVec2(at.x + (icon - size.x) * 0.5f, at.y + (icon - size.y) * 0.5f), ImGui::GetColorU32(Dim), "?");
                    }
                }
                ImGui::Dummy(ImVec2(icon, icon));
                ImGui::TableNextColumn();
                ImGui::PushTextWrapPos(0.0f);
                const ImVec4 text = earned ? ImGui::GetStyle().Colors[ImGuiCol_Text] : Fade(ImGui::GetStyle().Colors[ImGuiCol_Text], 0.6f);
                ImGui::TextColored(text, "%s", secret ? "Hidden trophy" : (trophy.name.empty() ? ("Trophy " + std::to_string(trophy.id)).c_str() : trophy.name.c_str()));
                ImGui::TextColored(Dim, "%s", secret ? "Keep playing to find out what it is." : trophy.detail.c_str());
                if (trophy.statId && !earned) {
                    ProgressBar(static_cast<float>(trophy.progress) / static_cast<float>(trophy.target), ImGui::GetContentRegionAvail().x * 0.6f, GradeColor(trophy.grade));
                    ImGui::SameLine();
                    ImGui::TextColored(Dim, "%llu/%llu", static_cast<unsigned long long>(trophy.progress), static_cast<unsigned long long>(trophy.target));
                }
                ImGui::PopTextWrapPos();
                ImGui::TableNextColumn();
                TrophyIcon(trophy.grade, ImGui::GetFontSize() * 1.6f);
                if (earned) ImGui::TextColored(Green, "%s", LocalDate(*trophy.earnedAt).c_str());
                else ImGui::TextColored(Dim, "Not earned");
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        ImGui::EndChild();
        ImGui::EndPopup();
    }

    // ---- controls -------------------------------------------------------------------

    std::string ProfileInputHeader() const {
        return "# AnyPS5 input mapping for the profile \"" + layout_.profile + "\", written by AnyPS5 Launcher.\n"
               "# Lists only what differs from AnyPS5's built-in keys; see docs/user/INPUT_MAPPING.md in AnyPS5.\n"
               "# Bindings for one game go into anyps5-input.<serial>.ini beside this file.\n";
    }

    std::string GameInputHeader() const {
        return "# AnyPS5 input mapping for " + controlsName_ + " (" + controlsSerial_ + ") in the profile \"" + layout_.profile + "\", written by AnyPS5 Launcher.\n"
               "# AnyPS5 reads it after anyps5-input.ini; it lists only what differs for this game.\n";
    }

    // One game's own controls; the keys of every game are edited from Settings.
    void OpenControls(const GameEntry& game) {
        controlsSerial_ = game.metadata.titleId;
        controlsName_ = game.metadata.name;
        controlsProfile_ = ReadInputFile(layout_.InputConfig());
        controlsGame_ = ReadInputFile(layout_.GameInputConfig(controlsSerial_));
        controlsScope_ = 0;
        capture_.clear();
        controlsError_.clear();
        openControls_ = true;
    }

    // The keys of every game of the profile, opened from Settings (and drawn over it).
    void OpenProfileControls() {
        controlsSerial_.clear();
        controlsName_.clear();
        controlsProfile_ = ReadInputFile(layout_.InputConfig());
        controlsGame_ = {};
        controlsScope_ = 1;
        capture_.clear();
        controlsError_.clear();
        openControls_ = true;
    }

    InputFile& ControlsTarget() { return controlsScope_ == 0 ? controlsGame_ : controlsProfile_; }

    // What an action does in the scope being edited, and where that comes from.
    std::vector<std::string> ControlsEffective(const InputActionInfo& action, const char** origin) {
        static const InputFile none;
        const auto& game = controlsScope_ == 0 ? controlsGame_ : none;
        if (origin != nullptr) {
            *origin = game.bindings.count(action.name) ? "this game" : controlsProfile_.bindings.count(action.name) ? (controlsScope_ == 0 ? "all games" : "changed") : "built-in";
        }
        return EffectiveBindings(action, controlsProfile_, game);
    }

    // Called with each SDL event while the controls window waits for a key. True when it took the event.
    bool CaptureEvent(const SDL_Event& event) {
        if (capture_.empty()) return false;
        std::string source;
        if (event.type == SDL_EVENT_KEY_DOWN) {
            if (event.key.repeat) return true;
            if (event.key.scancode == SDL_SCANCODE_ESCAPE) {
                capture_.clear();
                return true;
            }
            const char* name = SDL_GetScancodeName(event.key.scancode);
            if (name == nullptr || name[0] == 0) return true;
            source = std::string("KEY:") + name;
        } else if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
            switch (event.button.button) {
            case SDL_BUTTON_LEFT: source = "MOUSE:Left"; break;
            case SDL_BUTTON_MIDDLE: source = "MOUSE:Middle"; break;
            case SDL_BUTTON_RIGHT: source = "MOUSE:Right"; break;
            case SDL_BUTTON_X1: source = "MOUSE:X1"; break;
            case SDL_BUTTON_X2: source = "MOUSE:X2"; break;
            default: return true;
            }
        } else if (event.type == SDL_EVENT_MOUSE_WHEEL) {
            if (event.wheel.y > 0) source = "WHEEL:Up";
            else if (event.wheel.y < 0) source = "WHEEL:Down";
            else if (event.wheel.x > 0) source = "WHEEL:Right";
            else if (event.wheel.x < 0) source = "WHEEL:Left";
            else return true;
        } else if (event.type == SDL_EVENT_KEY_UP || event.type == SDL_EVENT_MOUSE_BUTTON_UP || event.type == SDL_EVENT_TEXT_INPUT) {
            return true;
        } else {
            return false;
        }
        const auto* action = FindInputAction(capture_);
        if (action == nullptr) {
            capture_.clear();
            return true;
        }
        if (action->keysOnly && source.rfind("KEY:", 0) != 0) {
            captureNote_ = std::string(action->label) + " takes a keyboard key only.";
            return true;
        }
        if (source.rfind("WHEEL:", 0) == 0 && std::string(action->group) == "AnyPS5") {
            captureNote_ = "The mouse wheel can only press pad buttons.";
            return true;
        }
        AssignSource(*action, source);
        capture_.clear();
        return true;
    }

    void AssignSource(const InputActionInfo& action, const std::string& source) {
        auto& target = ControlsTarget();
        const auto lower = [](std::string text) {
            std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return text;
        };
        // The key leaves the actions that had it, unless that would leave one with nothing.
        for (const auto& other : InputActions()) {
            if (std::string(other.name) == action.name) continue;
            auto sources = ControlsEffective(other, nullptr);
            const auto before = sources.size();
            sources.erase(std::remove_if(sources.begin(), sources.end(), [&](const std::string& kept) { return lower(kept) == lower(source); }), sources.end());
            if (sources.size() != before && !sources.empty()) target.bindings[other.name] = sources;
        }
        std::vector<std::string> sources = captureReplace_ ? std::vector<std::string>() : ControlsEffective(action, nullptr);
        if (std::none_of(sources.begin(), sources.end(), [&](const std::string& kept) { return lower(kept) == lower(source); })) sources.push_back(source);
        target.bindings[action.name] = sources;
    }

    // A pad button, stick or touchpad as a drawn symbol, size by size, at the cursor.
    // False for actions that have no symbol (shake, AnyPS5's own keys).
    static bool PadGlyph(std::string_view name, float size) {
        const ImVec2 at = ImGui::GetCursorScreenPos();
        auto* draw = ImGui::GetWindowDrawList();
        const ImVec2 c(at.x + size * 0.5f, at.y + size * 0.5f);
        const float line = std::max(1.5f, size * 0.09f);
        const ImU32 ink = ImGui::GetColorU32(ImGuiCol_Text);
        const ImU32 faint = ImGui::GetColorU32(Fade(ImGui::GetStyle().Colors[ImGuiCol_Text], 0.35f));
        const auto label = [&](const char* text, float scale) {
            ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * scale);
            const ImVec2 size2 = ImGui::CalcTextSize(text);
            draw->AddText(ImVec2(c.x - size2.x * 0.5f, c.y - size2.y * 0.5f), ink, text);
            ImGui::PopFont();
        };
        // The direction arm of a d-pad or the arrow beside a stick: 0 up, 1 right, 2 down, 3 left.
        const auto direction = [](std::string_view text) {
            if (text.ends_with("Up")) return 0;
            if (text.ends_with("Right")) return 1;
            if (text.ends_with("Down")) return 2;
            return 3;
        };
        const float r = size * 0.36f;
        if (name == "Cross") {
            const ImU32 color = IM_COL32(124, 167, 230, 255);
            draw->AddLine(ImVec2(c.x - r, c.y - r), ImVec2(c.x + r, c.y + r), color, line * 1.2f);
            draw->AddLine(ImVec2(c.x + r, c.y - r), ImVec2(c.x - r, c.y + r), color, line * 1.2f);
        } else if (name == "Circle") {
            draw->AddCircle(c, r, IM_COL32(232, 104, 104, 255), 0, line * 1.2f);
        } else if (name == "Square") {
            draw->AddRect(ImVec2(c.x - r * 0.9f, c.y - r * 0.9f), ImVec2(c.x + r * 0.9f, c.y + r * 0.9f), IM_COL32(214, 140, 210, 255), size * 0.04f, 0, line * 1.2f);
        } else if (name == "Triangle") {
            draw->AddTriangle(ImVec2(c.x, c.y - r), ImVec2(c.x + r * 1.05f, c.y + r * 0.8f), ImVec2(c.x - r * 1.05f, c.y + r * 0.8f), IM_COL32(73, 200, 165, 255), line * 1.2f);
        } else if (name == "L1" || name == "R1" || name == "L2" || name == "R2") {
            // A shoulder button: a rounded tab, taller for the triggers.
            const bool trigger = name[1] == '2';
            const float top = trigger ? c.y - size * 0.40f : c.y - size * 0.30f;
            draw->AddRect(ImVec2(at.x + size * 0.04f, top), ImVec2(at.x + size * 0.96f, c.y + size * 0.30f), ink, size * (trigger ? 0.28f : 0.16f), 0, line);
            label(std::string(name).c_str(), 0.82f);
        } else if (name == "L3" || name == "R3") {
            draw->AddCircle(c, r * 1.1f, ink, 0, line);
            draw->AddCircle(c, r * 0.65f, faint, 0, line);
            label(std::string(name).c_str(), 0.72f);
        } else if (name == "Options") {
            // Three short lines, as printed on the button.
            for (int i = -1; i <= 1; ++i) draw->AddLine(ImVec2(c.x - r * 0.75f, c.y + i * r * 0.55f), ImVec2(c.x + r * 0.75f, c.y + i * r * 0.55f), ink, line);
        } else if (name == "Up" || name == "Down" || name == "Left" || name == "Right") {
            // The d-pad cross with the pressed arm filled.
            const float arm = size * 0.15f;
            const float reach = size * 0.46f;
            const int pressed = direction(name);
            const ImVec2 arms[4][2] = {{ImVec2(c.x - arm, c.y - reach), ImVec2(c.x + arm, c.y - arm)},
                                       {ImVec2(c.x + arm, c.y - arm), ImVec2(c.x + reach, c.y + arm)},
                                       {ImVec2(c.x - arm, c.y + arm), ImVec2(c.x + arm, c.y + reach)},
                                       {ImVec2(c.x - reach, c.y - arm), ImVec2(c.x - arm, c.y + arm)}};
            draw->AddRectFilled(ImVec2(c.x - arm, c.y - arm), ImVec2(c.x + arm, c.y + arm), faint);
            for (int i = 0; i < 4; ++i) draw->AddRectFilled(arms[i][0], arms[i][1], i == pressed ? ImGui::GetColorU32(Accent) : faint, size * 0.03f);
        } else if (name.starts_with("LeftStick") || name.starts_with("RightStick")) {
            // A stick seen from above, pushed toward the direction.
            const int pushed = direction(name);
            const float dx = pushed == 1 ? 1.0f : pushed == 3 ? -1.0f : 0.0f;
            const float dy = pushed == 0 ? -1.0f : pushed == 2 ? 1.0f : 0.0f;
            draw->AddCircle(c, r * 1.15f, faint, 0, line);
            const ImVec2 cap(c.x + dx * r * 0.42f, c.y + dy * r * 0.42f);
            draw->AddCircleFilled(cap, r * 0.78f, ImGui::GetColorU32(Accent));
            ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 0.75f);
            const char* letter = name.starts_with("Left") ? "L" : "R";
            const ImVec2 size2 = ImGui::CalcTextSize(letter);
            draw->AddText(ImVec2(cap.x - size2.x * 0.5f, cap.y - size2.y * 0.5f), IM_COL32_WHITE, letter);
            ImGui::PopFont();
        } else if (name == "TouchLeft" || name == "TouchRight") {
            const ImVec2 min(at.x + size * 0.04f, c.y - size * 0.28f);
            const ImVec2 max(at.x + size * 0.96f, c.y + size * 0.28f);
            const bool left = name == "TouchLeft";
            draw->AddRectFilled(left ? min : ImVec2(c.x, min.y), left ? ImVec2(c.x, max.y) : max, ImGui::GetColorU32(Fade(Accent, 0.55f)), size * 0.08f,
                                left ? ImDrawFlags_RoundCornersLeft : ImDrawFlags_RoundCornersRight);
            draw->AddRect(min, max, ink, size * 0.08f, 0, line);
        } else {
            return false;
        }
        ImGui::Dummy(ImVec2(size, size));
        return true;
    }

    // A key as a keycap. Returns true when clicked.
    static bool Keycap(const char* id, const std::string& text, bool clash, bool removable) {
        const float scale = ImGui::GetStyle().FontScaleDpi;
        const ImVec2 pad(9.0f * scale, 4.0f * scale);
        const std::string shown = text;
        const ImVec2 textSize = ImGui::CalcTextSize(shown.c_str());
        const float cross = removable ? ImGui::GetFontSize() * 0.9f : 0.0f;
        const ImVec2 size(textSize.x + pad.x * 2 + cross, textSize.y + pad.y * 2);
        const ImVec2 at = ImGui::GetCursorScreenPos();
        const bool pressed = ImGui::InvisibleButton(id, size);
        const bool hovered = ImGui::IsItemHovered();
        auto* draw = ImGui::GetWindowDrawList();
        const ImVec2 end(at.x + size.x, at.y + size.y);
        const ImVec4 text4 = clash ? Amber : ImGui::GetStyle().Colors[ImGuiCol_Text];
        draw->AddRectFilled(at, end, ImGui::GetColorU32(hovered ? CardHover : WindowColor), 6.0f * scale);
        draw->AddRect(at, end, ImGui::GetColorU32(clash ? Amber : ImGui::GetStyle().Colors[ImGuiCol_Border]), 6.0f * scale, 0, 1.0f);
        // A darker bottom edge, like a key.
        draw->AddLine(ImVec2(at.x + 4.0f * scale, end.y - 1.0f), ImVec2(end.x - 4.0f * scale, end.y - 1.0f), ImGui::GetColorU32(Fade(ImGui::GetStyle().Colors[ImGuiCol_Border], 1.0f)), 2.0f * scale);
        draw->AddText(ImVec2(at.x + pad.x, at.y + pad.y), ImGui::GetColorU32(text4), shown.c_str());
        if (removable && hovered) {
            const ImVec2 mid(end.x - pad.x - cross * 0.35f, at.y + size.y * 0.5f);
            const float arm = cross * 0.22f;
            const ImU32 color = ImGui::GetColorU32(Dim);
            draw->AddLine(ImVec2(mid.x - arm, mid.y - arm), ImVec2(mid.x + arm, mid.y + arm), color, 1.4f * scale);
            draw->AddLine(ImVec2(mid.x + arm, mid.y - arm), ImVec2(mid.x - arm, mid.y + arm), color, 1.4f * scale);
        }
        return pressed;
    }

    static std::string LowerText(std::string text) {
        std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return text;
    }

    // One action as a card: its symbol (or name), then its keys as keycaps. Clicking the
    // card waits for a new key; + adds one; a keycap removes itself.
    void DrawControlRow(const InputActionInfo& action, const std::map<std::string, std::vector<std::string>>& conflicts, InputFile& target) {
        const auto& style = ImGui::GetStyle();
        const float scale = style.FontScaleDpi;
        ImGui::PushID(action.name);
        const char* origin = "";
        const auto sources = ControlsEffective(action, &origin);
        const float height = ImGui::GetFontSize() * 2.3f;
        const float width = ImGui::GetContentRegionAvail().x;
        const ImVec2 at = ImGui::GetCursorScreenPos();
        const ImVec2 end(at.x + width, at.y + height);
        auto* draw = ImGui::GetWindowDrawList();
        const bool waiting = capture_ == action.name;

        // Keycaps are laid out right to left first, so the card's own button stops where they start.
        float keys = 0;
        const float gap = 6.0f * scale;
        const auto keycapWidth = [&](const std::string& text, bool removable) {
            return ImGui::CalcTextSize(text.c_str()).x + 18.0f * scale + (removable ? ImGui::GetFontSize() * 0.9f : 0.0f);
        };
        const bool removable = sources.size() > 1;
        for (const auto& source : sources) keys += keycapWidth(DescribeInputSource(source), removable) + gap;
        keys += keycapWidth("+", false) + gap;
        const bool changed = target.bindings.count(action.name) != 0;
        const float reset = changed ? ImGui::CalcTextSize("Reset").x + style.FramePadding.x * 2 : 0.0f;
        const float right = keys + reset + 12.0f * scale;

        const bool hoveredRow = ImGui::IsMouseHoveringRect(at, end) && ImGui::IsWindowHovered();
        const ImVec4 text = style.Colors[ImGuiCol_Text];
        draw->AddRectFilled(at, end, ImGui::GetColorU32(waiting ? Fade(Accent, 0.22f) : Fade(text, hoveredRow ? 0.10f : 0.05f)), 10.0f * scale);
        if (waiting) draw->AddRect(at, end, ImGui::GetColorU32(Accent), 10.0f * scale, 0, 1.5f * scale);

        ImGui::SetCursorScreenPos(at);
        if (ImGui::InvisibleButton("row", ImVec2(std::max(1.0f, width - right), height))) {
            capture_ = action.name;
            captureReplace_ = true;
            captureNote_.clear();
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s: click, then press the key you want", action.label);

        // Symbol, then the name for actions without one.
        const float glyph = ImGui::GetFontSize() * 1.9f;
        ImGui::SetCursorScreenPos(ImVec2(at.x + 14.0f * scale, at.y + (height - glyph) * 0.5f));
        const bool symbol = PadGlyph(action.name, glyph);
        const float textY = at.y + (height - ImGui::GetFontSize()) * 0.5f;
        if (!symbol) {
            draw->AddText(ImVec2(at.x + 14.0f * scale, textY), ImGui::GetColorU32(ImGuiCol_Text), action.label);
        } else if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", action.label);
        }
        if (std::string_view(origin) != "built-in") {
            const float originX = at.x + 14.0f * scale + (symbol ? glyph + 12.0f * scale : ImGui::CalcTextSize(action.label).x + 12.0f * scale);
            draw->AddText(ImVec2(originX, textY), ImGui::GetColorU32(Accent), origin);
        }
        if (waiting) {
            const char* prompt = action.keysOnly ? "Press a key...  Esc cancels" : "Press a key, click or turn the wheel...  Esc cancels";
            const float x = at.x + width * 0.30f;
            draw->AddText(ImVec2(x, textY), ImGui::GetColorU32(Accent), prompt);
            if (!captureNote_.empty()) draw->AddText(ImVec2(x, textY + ImGui::GetFontSize() * 0.95f), ImGui::GetColorU32(Amber), captureNote_.c_str());
        }

        // Keycaps.
        const float capHeight = ImGui::GetFontSize() + 8.0f * scale;
        float x = end.x - right + 4.0f * scale;
        for (std::size_t index = 0; index < sources.size(); ++index) {
            const auto text = DescribeInputSource(sources[index]);
            const auto conflict = conflicts.find(LowerText(sources[index]));
            const bool clash = conflict != conflicts.end();
            ImGui::SetCursorScreenPos(ImVec2(x, at.y + (height - capHeight) * 0.5f));
            if (Keycap(("key" + std::to_string(index)).c_str(), text, clash, removable) && removable) {
                auto kept = sources;
                kept.erase(kept.begin() + static_cast<std::ptrdiff_t>(index));
                target.bindings[action.name] = kept;
            }
            if (ImGui::IsItemHovered()) {
                std::string tip = removable ? "Click to remove" : "An action keeps at least one key; click the row to change it";
                if (clash) {
                    tip += "\nAlso used by:";
                    for (const auto& user : conflict->second) {
                        if (user != action.name) tip += " " + std::string(FindInputAction(user)->label) + " (" + FindInputAction(user)->group + ")";
                    }
                }
                ImGui::SetTooltip("%s", tip.c_str());
            }
            x += keycapWidth(text, removable) + gap;
        }
        ImGui::SetCursorScreenPos(ImVec2(x, at.y + (height - capHeight) * 0.5f));
        if (Keycap("add", "+", false, false)) {
            capture_ = action.name;
            captureReplace_ = false;
            captureNote_.clear();
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Add another key or mouse button");
        x += keycapWidth("+", false) + gap;
        if (changed) {
            ImGui::SetCursorScreenPos(ImVec2(x, at.y + (height - ImGui::GetFontSize()) * 0.5f));
            if (ImGui::SmallButton("Reset")) target.bindings.erase(action.name);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", controlsScope_ == 0 ? "Back to the keys of all games" : "Back to AnyPS5's built-in keys");
        }
        ImGui::SetCursorScreenPos(ImVec2(at.x, end.y + 6.0f * scale));
        ImGui::Dummy(ImVec2(width, 0));
        ImGui::PopID();
    }

    void DrawControls() {
        if (openControls_) {
            ImGui::OpenPopup("Controls");
            openControls_ = false;
        }
        FollowWindow(0.80f, 0.90f);
        if (!BeginDialog("Controls")) return;
        const auto& style = ImGui::GetStyle();
        const bool forGame = controlsScope_ == 0;
        const std::string title = forGame ? "Controls: " + controlsName_ : "Controls for all games";
        const std::string note = forGame ? "Click a row, then press the key you want for it; Esc cancels. Changes here are for " + controlsSerial_ +
                                               " only; the keys of every game are in Settings. A connected controller keeps its own buttons. AnyPS5 reads these at the game's next start, or at once with F1."
                                         : "Click a row, then press the key you want for it; Esc cancels. These keys are for every game of profile " + layout_.profile +
                                               "; a game can still change some in its own Controls. A connected controller keeps its own buttons.";
        bool close = DialogHeader(title.c_str(), note.c_str());

        const float footer = ImGui::GetFrameHeight() * 1.3f + style.ItemSpacing.y * 3 + ImGui::GetTextLineHeightWithSpacing();
        ImGui::BeginChild("controls-body", ImVec2(0, -footer));
        const InputFile none;
        const auto conflicts = InputConflicts(controlsProfile_, controlsScope_ == 0 ? controlsGame_ : none);
        auto& target = ControlsTarget();
        std::string group;
        for (const auto& action : InputActions()) {
            if (group != action.group) {
                group = action.group;
                ImGui::Spacing();
                ImGui::TextColored(Dim, "%s", action.group);
            }
            DrawControlRow(action, conflicts, target);
        }
        ImGui::Spacing();

        ImGui::SeparatorText("Sticks and mouse");
        if (ImGui::BeginTable("input-settings", 3, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_PadOuterX | ImGuiTableFlags_RowBg)) {
            ImGui::TableSetupColumn("setting", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 13.0f);
            ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("edit", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 5.0f);
            for (const auto& setting : InputSettings()) {
                ImGui::PushID(setting.name);
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted(setting.label);
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", setting.note);
                ImGui::TableNextColumn();
                const auto own = target.settings.find(setting.name);
                std::string inherited = setting.choices.empty() ? FormatSettingValue(setting, setting.fallback) : setting.choices.front();
                const char* from = "built-in";
                if (controlsScope_ == 0) {
                    if (const auto profile = controlsProfile_.settings.find(setting.name); profile != controlsProfile_.settings.end()) {
                        inherited = profile->second;
                        from = "all games";
                    }
                }
                if (own == target.settings.end()) {
                    ImGui::AlignTextToFramePadding();
                    ImGui::TextColored(Dim, "%s (%s)", inherited.c_str(), from);
                    ImGui::TableNextColumn();
                    if (ImGui::Button("Change")) target.settings[setting.name] = inherited;
                } else {
                    ImGui::SetNextItemWidth(-FLT_MIN);
                    if (!setting.choices.empty()) {
                        int current = 0;
                        for (std::size_t i = 0; i < setting.choices.size(); ++i) {
                            if (setting.choices[i] == own->second) current = static_cast<int>(i);
                        }
                        std::vector<const char*> items;
                        for (const auto& choice : setting.choices) items.push_back(choice.c_str());
                        if (ImGui::Combo("##value", &current, items.data(), static_cast<int>(items.size()))) own->second = setting.choices[static_cast<std::size_t>(current)];
                    } else {
                        float value = static_cast<float>(std::atof(own->second.c_str()));
                        const bool changed = setting.integer ? ImGui::SliderFloat("##value", &value, static_cast<float>(setting.minimum), static_cast<float>(setting.maximum), "%.0f")
                                                             : ImGui::SliderFloat("##value", &value, static_cast<float>(setting.minimum), static_cast<float>(setting.maximum), "%.2f");
                        if (changed) own->second = FormatSettingValue(setting, value);
                    }
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", setting.note);
                    ImGui::TableNextColumn();
                    if (ImGui::Button("Reset")) target.settings.erase(setting.name);
                }
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        if (!target.other.empty()) {
            ImGui::Spacing();
            ImGui::TextColored(Dim, "Other lines in the file are kept as they are (%d), such as controller button swaps.", static_cast<int>(target.other.size()));
        }
        ImGui::EndChild();

        // Footer.
        if (!conflicts.empty()) {
            ImGui::TextColored(Amber, "%d key(s) do two things at once (shown in amber); hover one to see which.", static_cast<int>(conflicts.size()));
        } else if (!controlsError_.empty()) {
            ImGui::TextColored(Red, "%s", controlsError_.c_str());
        } else {
            ImGui::TextColored(Dim, "%s", ToUtf8(controlsScope_ == 0 ? layout_.GameInputConfig(controlsSerial_) : layout_.InputConfig()).c_str());
        }
        ImGui::Spacing();
        if (ImGui::Button("Reset all")) {
            target.bindings.clear();
            target.settings.clear();
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", controlsScope_ == 0 ? "This game uses the keys of all games again" : "Every game uses AnyPS5's built-in keys again (games with their own controls keep them)");
        const float buttons = ImGui::CalcTextSize("Cancel").x + ImGui::CalcTextSize("Save").x + style.FramePadding.x * 8 + style.ItemSpacing.x + ImGui::GetFontSize() * 2;
        ImGui::SameLine(ImGui::GetContentRegionMax().x - buttons);
        if (ImGui::Button("Cancel", ImVec2(0, ImGui::GetFrameHeight() * 1.3f))) close = true;
        ImGui::SameLine();
        if (PrimaryButton("Save", ImVec2(ImGui::GetFontSize() * 5, ImGui::GetFrameHeight() * 1.3f))) {
            try {
                if (forGame) {
                    const auto gameFile = layout_.GameInputConfig(controlsSerial_);
                    std::error_code error;
                    if (controlsGame_.Empty()) std::filesystem::remove(gameFile, error);
                    else WriteInputFile(gameFile, controlsGame_, GameInputHeader());
                    message_ = Message{Green, "Controls saved", controlsName_ + " uses them from its next start (or press F1 in the game)."};
                } else {
                    WriteInputFile(layout_.InputConfig(), controlsProfile_, ProfileInputHeader());
                    message_ = Message{Green, "Controls saved", "Every game of profile " + layout_.profile + " uses them from its next start."};
                }
                close = true;
            } catch (const std::exception& failure) {
                controlsError_ = failure.what();
            }
        }

        if (close) {
            capture_.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    void DrawDetails(const GameEntry& game) {
        ImGui::PushTextWrapPos(0.0f);  // everything in the panel wraps to its width
        DrawDetailsBody(game);
        ImGui::PopTextWrapPos();
    }

    void DrawDetailsBody(const GameEntry& game) {
        const auto& style = ImGui::GetStyle();
        DrawArtwork(game);
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
        const float half = (full - style.ItemSpacing.x) * 0.5f;
        if (ImGui::Button("Trophies", ImVec2(half, 0))) OpenTrophies();
        ImGui::SameLine();
        if (ImGui::Button("Controls", ImVec2(half, 0))) OpenControls(game);
        if (ImGui::Button("All actions...", ImVec2(full, 0))) ImGui::OpenPopup("details-actions");
        if (ImGui::BeginPopup("details-actions")) {
            DrawActionItems(game);
            ImGui::EndPopup();
        }

        ImGui::Spacing();
        ImGui::SeparatorText("Status");
        DrawStatus(game);
        ImGui::SeparatorText("Trophies");
        DrawTrophySummary(game);
        ImGui::SeparatorText("Store");
        DrawStore(game);
        if (!game.changes.empty()) {
            ImGui::SeparatorText("What's new");
            DrawChanges(game);
        }
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
        if (ImGui::MenuItem("Trophies...")) OpenTrophies();
        if (ImGui::MenuItem("Controls...")) OpenControls(game);
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

    // A game whose run didn't finish still has the profile's saves beside its exe,
    // recorded by folder path: renaming or deleting the profile now would lose them.
    std::string ProfileBusyReason() const {
        if (session_) return "a game is running";
        for (const auto& game : result_.games) {
            if (!game.home.empty() && HasSession(game.home)) return game.metadata.name + " still has the profile's saves beside its exe; restart the launcher to put them back";
        }
        return {};
    }

    void OpenEditProfile() {
        editName_ = layout_.profile;
        editProfile_ = profileSettings_;
        editError_.clear();
        openEditProfile_ = true;
    }

    void DrawEditProfile() {
        if (openEditProfile_) {
            ImGui::OpenPopup("Edit profile");
            openEditProfile_ = false;
        }
        const float scale = scale_;
        const float width = ImGui::GetFontSize() * 26;
        ImGui::SetNextWindowSize(ImVec2(width, 0));
        ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(22.0f * scale, 20.0f * scale));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 14.0f * scale);
        const bool open = ImGui::BeginPopupModal("Edit profile", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                                                              ImGuiWindowFlags_AlwaysAutoResize);
        ImGui::PopStyleVar(2);
        if (!open) return;
        const auto& style = ImGui::GetStyle();
        bool close = DialogHeader("Edit profile", "Saves, trophies, controls and screenshots stay with the profile.");

        // The avatar follows the name as it's typed.
        const float avatar = ImGui::GetFontSize() * 3.6f;
        const ImVec2 at = ImGui::GetCursorScreenPos();
        DrawProfileAvatar(editName_.empty() ? std::string("?") : editName_, at, avatar);
        ImGui::Dummy(ImVec2(avatar, avatar));
        ImGui::SameLine(0, 16.0f * scale);
        ImGui::BeginGroup();
        ImGui::TextUnformatted("Name");
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        const bool submit = ImGui::InputText("##profile-name", &editName_, ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::TextColored(Dim, "%s", "Games still see the player as \"Player\".");
        ImGui::EndGroup();
        ImGui::Spacing();

        ImGui::TextUnformatted("Console language");
        ImGui::TextColored(Dim, "The language games start in.");
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::BeginCombo("##profile-language", LanguageLabel(editProfile_.language).c_str())) {
            if (ImGui::Selectable(LanguageLabel("").c_str(), editProfile_.language.empty())) editProfile_.language.clear();
            if (ImGui::Selectable(LanguageLabel("auto").c_str(), editProfile_.language == "auto")) editProfile_.language = "auto";
            for (const auto& item : ConsoleLanguages()) {
                const auto value = std::to_string(item.number);
                if (ImGui::Selectable(LanguageLabel(value).c_str(), editProfile_.language == value)) editProfile_.language = value;
            }
            ImGui::EndCombo();
        }
        if (!editError_.empty()) {
            ImGui::Spacing();
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextColored(Red, "%s", editError_.c_str());
            ImGui::PopTextWrapPos();
        }
        ImGui::Spacing();
        ImGui::Spacing();

        const float buttonHeight = ImGui::GetFrameHeight() * 1.3f;
        const auto busy = ProfileBusyReason();
        const bool only = profiles_.size() <= 1;
        ImGui::BeginDisabled(!busy.empty() || only);
        ImGui::PushStyleColor(ImGuiCol_Text, Red);
        if (ImGui::Button("Delete profile...", ImVec2(0, buttonHeight))) ImGui::OpenPopup("Delete profile?");
        ImGui::PopStyleColor();
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) && (only || !busy.empty())) {
            ImGui::SetTooltip("%s", only ? "It's the only profile" : ("Not now: " + busy).c_str());
        }
        const float buttonWidth = ImGui::GetFontSize() * 5.5f;
        ImGui::SameLine(ImGui::GetContentRegionMax().x - buttonWidth * 2 - style.ItemSpacing.x);
        if (ImGui::Button("Cancel", ImVec2(buttonWidth, buttonHeight))) close = true;
        ImGui::SameLine();
        if (PrimaryButton("Save", ImVec2(buttonWidth, buttonHeight)) || submit) {
            try {
                if (editName_ != layout_.profile) {
                    if (!busy.empty()) throw std::runtime_error("the profile can't be renamed now: " + busy);
                    RenameProfile(layout_, editName_);
                    layout_.profile = editName_;
                    settings_.profile = editName_;
                    SaveSettings(settingsFile_, settings_);
                    profiles_ = ListProfiles(layout_.userdata);
                }
                SaveProfile(layout_, editProfile_);
                LoadCurrentProfile();
                trophyAsked_.clear();  // texts may follow the new language
                close = true;
            } catch (const std::exception& failure) {
                editError_ = ReadableText(failure.what());
            }
        }

        // Asked once more before anything goes.
        ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        if (ImGui::BeginPopupModal("Delete profile?", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove)) {
            ImGui::Text("Delete the profile \"%s\"?", layout_.profile.c_str());
#ifdef _WIN32
            ImGui::TextColored(Dim, "Its saves, trophies, controls and screenshots go to the Recycle Bin.");
#else
            ImGui::TextColored(Amber, "Its saves, trophies, controls and screenshots are deleted for good.");
#endif
            ImGui::Spacing();
            ImGui::PushStyleColor(ImGuiCol_Button, Red);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Fade(Red, 0.85f));
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));
            const bool confirmed = ImGui::Button("Delete", ImVec2(ImGui::GetFontSize() * 6, 0));
            ImGui::PopStyleColor(3);
            ImGui::SameLine();
            if (ImGui::Button("Keep it", ImVec2(ImGui::GetFontSize() * 6, 0))) ImGui::CloseCurrentPopup();
            if (confirmed) {
                ImGui::CloseCurrentPopup();
                if (DeleteCurrentProfile()) close = true;
            }
            ImGui::EndPopup();
        }

        if (close) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    // Switches to another profile, then deletes (or recycles) this one.
    bool DeleteCurrentProfile() {
        try {
            if (const auto busy = ProfileBusyReason(); !busy.empty()) throw std::runtime_error("not now: " + busy);
            const auto doomed = layout_.profile;
            std::string next;
            for (const auto& name : profiles_) {
                if (name != doomed) {
                    next = name;
                    break;
                }
            }
            if (next.empty()) throw std::runtime_error("it's the only profile");
            const auto folder = layout_.Profile();
            SwitchProfile(next);
            if (layout_.profile != next) throw std::runtime_error("couldn't switch to " + next);
#ifdef _WIN32
            if (const auto code = win::RecycleItem(folder.c_str()); code != 0) {
                throw std::runtime_error("couldn't move " + ToUtf8(folder) + " to the Recycle Bin (error " + std::to_string(code) + ")");
            }
#else
            std::filesystem::remove_all(folder);
#endif
            profiles_ = ListProfiles(layout_.userdata);
            message_ = Message{Green, "Profile deleted", "\"" + doomed + "\" is gone; now using " + next + "."};
            return true;
        } catch (const std::exception& failure) {
            editError_ = ReadableText(failure.what());
            return false;
        }
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
        FollowWindow(0.84f, 0.86f);
        if (!BeginDialog("AnyPS5 versions")) return;
        const auto& style = ImGui::GetStyle();
        const bool busy = job_.Busy() || session_ != nullptr;
        bool close = DialogHeader("AnyPS5 versions", "The selected version relinks games at Install. Games keep the version they were converted with until you reinstall them.");
        const float footer = ImGui::GetFrameHeight() * 1.3f + style.ItemSpacing.y * 3 + (job_.Busy() ? ImGui::GetTextLineHeightWithSpacing() : 0.0f) +
                             (versionsError_.empty() ? 0.0f : ImGui::GetTextLineHeightWithSpacing());
        ImGui::BeginChild("versions-body", ImVec2(0, -footer));

        std::string remove;
        Section("installed", "Installed", nullptr, [&] {
            if (versions_.versions.empty()) ImGui::TextColored(Dim, "None yet. Download a release below, or add your own build.");
            const float actions = ImGui::GetFontSize() * 10.5f;
            if (!versions_.versions.empty() && ImGui::BeginTable("versions", 3, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_PadOuterX)) {
                ImGui::TableSetupColumn("version", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("status", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 11.0f);
                ImGui::TableSetupColumn("actions", ImGuiTableColumnFlags_WidthFixed, actions);
                for (const auto& entry : versions_.versions) {
                    ImGui::PushID(entry.id.c_str());
                    const bool selected = entry.id == settings_.anyps5Version;
                    const auto& probe = ProbeFor(entry);
                    ImGui::TableNextRow(ImGuiTableRowFlags_None, ImGui::GetFrameHeight() * 2.0f);
                    if (selected) ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, ImGui::GetColorU32(Fade(Accent, 0.10f)));
                    ImGui::TableNextColumn();
                    ImGui::PushFont(nullptr, style.FontSizeBase * 1.08f);
                    ImGui::TextUnformatted(entry.Local() ? "Local build" : entry.tag.c_str());
                    ImGui::PopFont();
                    ImGui::TextColored(Dim, "%s", Elide(entry.Local() ? ToUtf8(entry.folder) : entry.repository, ImGui::GetContentRegionAvail().x).c_str());
                    ImGui::TableNextColumn();
                    ImGui::AlignTextToFramePadding();
                    if (probe.runtime) {
                        Pill("Ready", Green);
                        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s, %zu runtime libraries%s", probe.runtime->layout.c_str(), probe.runtime->libraries.size(),
                                                                      selected && !anyps5_.version.empty() ? ("\nrelinker " + anyps5_.version).c_str() : "");
                    } else {
                        Pill("Not usable", Amber);
                        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", probe.problem.c_str());
                    }
                    ImGui::TableNextColumn();
                    const float half = (actions - style.ItemSpacing.x) * 0.5f;
                    if (selected) {
                        Pill("Selected", Accent);
                    } else if (probe.runtime) {
                        ImGui::BeginDisabled(busy);
                        if (PrimaryButton("Use", ImVec2(half, 0))) SelectVersion(entry.id);
                        ImGui::EndDisabled();
                    } else {
                        ImGui::Dummy(ImVec2(half, ImGui::GetFrameHeight()));
                    }
                    ImGui::SameLine(half + style.ItemSpacing.x + ImGui::GetCursorStartPos().x);
                    ImGui::BeginDisabled(busy || selected);
                    if (ImGui::Button(entry.Local() ? "Forget" : "Delete", ImVec2(half, 0))) remove = entry.id;
                    ImGui::EndDisabled();
                    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                        ImGui::SetTooltip("%s", selected ? "Select another version first" : entry.Local() ? "Removes it from the list; the folder stays" : "Deletes the downloaded folder");
                    }
                    ImGui::PopID();
                }
                ImGui::EndTable();
            }
        });
        if (!remove.empty()) {
            try {
                RemoveVersion(versionsRoot_, versions_, remove);
                SaveVersions(versionsRoot_, versions_);
            } catch (const std::exception& failure) {
                versionsError_ = failure.what();
            }
        }

        Section("download", "Download a release", "From boykopovar/AnyPS5, or a fork that publishes releases the same way.", [&] {
            ImGui::SetNextItemWidth(std::min(ImGui::GetContentRegionAvail().x * 0.45f, ImGui::GetFontSize() * 18));
            if (ImGui::BeginCombo("##repository", releasesRepository_.c_str())) {
                if (ImGui::Selectable(OfficialRepository, releasesRepository_ == OfficialRepository)) releasesRepository_ = OfficialRepository;
                for (const auto& repository : versions_.repositories) {
                    if (ImGui::Selectable(repository.c_str(), releasesRepository_ == repository)) releasesRepository_ = repository;
                }
                ImGui::EndCombo();
            }
            ImGui::SameLine();
            ImGui::BeginDisabled(releasesLoader_.Running());
            if (PrimaryButton(releasesLoader_.Running() ? "Loading..." : "Load releases")) LoadReleases(releasesRepository_, false);
            ImGui::EndDisabled();

            if (releases_ && releases_->repository == releasesRepository_) {
                ImGui::Spacing();
                if (!releases_->error.empty()) {
                    ImGui::TextColored(Red, "%s", releases_->error.c_str());
                } else if (releases_->releases.empty()) {
                    ImGui::TextColored(Amber, "%s has no releases. Build it and add it under \"Your own build\".", releases_->repository.c_str());
                } else if (ImGui::BeginTable("releases", 3, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg,
                                             ImVec2(0, std::min(ImGui::GetFontSize() * 16.0f, ImGui::GetFrameHeight() * 1.6f * static_cast<float>(releases_->releases.size()) + 4.0f)))) {
                    ImGui::TableSetupColumn("tag", ImGuiTableColumnFlags_WidthStretch);
                    ImGui::TableSetupColumn("date", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 6.0f);
                    ImGui::TableSetupColumn("action", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 10.5f);
                    const auto newest = NewestRelease(releases_->releases);
                    for (const auto& release : releases_->releases) {
                        ImGui::PushID(release.tag.c_str());
                        ImGui::TableNextRow(ImGuiTableRowFlags_None, ImGui::GetFrameHeight() * 1.6f);
                        ImGui::TableNextColumn();
                        ImGui::AlignTextToFramePadding();
                        ImGui::TextUnformatted(release.tag.c_str());
                        if (newest && newest->tag == release.tag) {
                            ImGui::SameLine();
                            Pill("Latest", Green);
                        }
                        if (release.prerelease) {
                            ImGui::SameLine();
                            Pill("Pre-release", Amber);
                        }
                        ImGui::TableNextColumn();
                        ImGui::AlignTextToFramePadding();
                        ImGui::TextColored(Dim, "%s", release.publishedAt.substr(0, 10).c_str());
                        ImGui::TableNextColumn();
                        std::string problem;
                        const auto files = FilesFor(release, &problem);
                        if (IsInstalled(releases_->repository, release.tag)) {
                            Pill("Installed", Dim);
                        } else if (!files) {
                            ImGui::AlignTextToFramePadding();
                            ImGui::TextColored(Dim, "not for this system");
                            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", problem.c_str());
                        } else {
                            ImGui::BeginDisabled(busy);
                            const auto label = "Download  " + FormatBytes(files->relinker.size + files->runtime.size);
                            if (ImGui::Button(label.c_str(), ImVec2(-FLT_MIN, 0))) StartDownload(releases_->repository, release);
                            ImGui::EndDisabled();
                        }
                        ImGui::PopID();
                    }
                    ImGui::EndTable();
                }
            }
            ImGui::Spacing();
            ImGui::TextColored(Dim, "Add a fork");
            ImGui::SetNextItemWidth(std::min(ImGui::GetContentRegionAvail().x * 0.45f, ImGui::GetFontSize() * 18));
            ImGui::InputTextWithHint("##newrepo", "owner/repo", &newRepository_);
            ImGui::SameLine();
            ImGui::BeginDisabled(!IsValidRepository(newRepository_));
            if (ImGui::Button("Add and load")) {
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
            ImGui::TextColored(Dim, "A fork's releases need relinker-<tag>%s and prx-%s-<tag>%s, as AnyPS5's release workflow publishes them.", TargetIsWindows ? ".exe" : "",
                               TargetIsWindows ? "windows" : "linux", TargetIsWindows ? ".zip" : ".tar.gz");
        });

        Section("local", "Your own build", "An AnyPS5 clone after a build (build\\core\\relinker), or a release with its libs\\ folder beside the relinker. A fork you built works the same way.", [&] {
            ImGui::BeginDisabled(busy || Picker().IsOpen());
            if (ImGui::Button("Choose a relinker...")) Picker().OpenFile(window_, &localPick_, "Relinker", TargetIsWindows ? "exe" : "");
            ImGui::SameLine();
            if (ImGui::Button("Choose an AnyPS5 folder...")) Picker().Open(window_, &localPick_);
            ImGui::EndDisabled();
        });
        ImGui::EndChild();

        if (job_.Busy()) ImGui::TextColored(Amber, "%s...", job_.Title().c_str());
        if (!versionsError_.empty()) ImGui::TextColored(Red, "%s", Elide(versionsError_, ImGui::GetContentRegionAvail().x).c_str());
        ImGui::Spacing();
        const float buttonWidth = ImGui::GetFontSize() * 6.5f;
        ImGui::SetCursorPosX(ImGui::GetContentRegionMax().x - buttonWidth);
        if (ImGui::Button("Close", ImVec2(buttonWidth, ImGui::GetFrameHeight() * 1.3f))) close = true;
        if (close) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
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

    void FolderField(const char* label, const char* note, const char* id, const std::string& hint, std::string& value) {
        ImGui::TextUnformatted(label);
        if (note != nullptr) ImGui::TextColored(Dim, "%s", note);
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
        if (!BeginDialog("Settings")) return;
        const auto& style = ImGui::GetStyle();
        bool cancel = DialogHeader("Settings", "Changes take effect when you save.");
        const float footer = ImGui::GetFrameHeight() * 1.3f + style.ItemSpacing.y * 3 + (saveError_.empty() ? 0.0f : ImGui::GetTextLineHeightWithSpacing());
        ImGui::BeginChild("settings-body", ImVec2(0, -footer));

        Section("folders", "Folders", nullptr, [&] {
            FolderField("Games", "Scanned for game folders and .zar archives.", "##games", "the folder that holds your games", draft_.gamesFolder);
            ImGui::Spacing();
            FolderField("Library", "Logs and shader caches; safe to delete, it is rebuilt.", "##library", ToUtf8(launcherFolder_ / "library"), draft_.libraryFolder);
            ImGui::Spacing();
            FolderField("Userdata", "Profiles, saves and trophies: back this up. Keep it on the same drive as your games, so saves move instantly for each run.",
                        "##userdata", ToUtf8(launcherFolder_ / "userdata"), draft_.userdataFolder);
        });

        Section("appearance", "Appearance", "Follow Windows switches with Windows' light or dark mode.", [&] {
            const char* labels[] = {"Follow Windows", "Dark", "Light"};
            const char* values[] = {"system", "dark", "light"};
            int current = 0;
            for (int index = 0; index < 3; ++index) {
                if (draft_.theme == values[index]) current = index;
            }
            if (Segmented("theme", labels, 3, current)) {
                draft_.theme = values[current];
                ApplyTheme(ImGui::GetStyle(), WantDark(draft_.theme));
                ImGui::GetStyle().ScaleAllSizes(scale_);
            }
        });

        Section("controls", "Controls", "Keyboard and mouse keys for every game of this profile. Each game can change some of them for itself under Controls in its details.", [&] {
            if (ImGui::Button("Controls for all games...")) OpenProfileControls();
            ImGui::SameLine();
            ImGui::TextColored(Dim, "%s", ToUtf8(layout_.InputConfig()).c_str());
        });

        Section("trophy-cards", "Trophies while playing", "A card at the top of the game's window for each trophy it earns. It never takes the keyboard or the mouse from the game.", [&] {
            ImGui::Checkbox("Show a card when a trophy is earned", &draft_.trophyPopups);
            ImGui::BeginDisabled(!draft_.trophyPopups);
            ImGui::Checkbox("Play a sound with it", &draft_.trophySound);
            ImGui::EndDisabled();
        });

        Section("online", "Online", "Looks up the selected game on PROSPEROPatches.com for its update list and publisher. Only the title ID is sent; answers are kept for a day.", [&] {
            ImGui::Checkbox("Look up game updates online", &draft_.onlineInfo);
        });

        ImGui::EndChild();

        if (!saveError_.empty()) ImGui::TextColored(Red, "%s", Elide(saveError_, ImGui::GetContentRegionAvail().x).c_str());
        ImGui::Spacing();
        const float buttonHeight = ImGui::GetFrameHeight() * 1.3f;
        const float buttonWidth = ImGui::GetFontSize() * 6.5f;
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(Dim, "%s", Elide("Saved in " + ToUtf8(settingsFile_), ImGui::GetContentRegionAvail().x - buttonWidth * 2 - style.ItemSpacing.x * 3).c_str());
        ImGui::SameLine(ImGui::GetContentRegionMax().x - buttonWidth * 2 - style.ItemSpacing.x);
        ImGui::BeginDisabled(Picker().IsOpen());
        if (ImGui::Button("Cancel", ImVec2(buttonWidth, buttonHeight))) cancel = true;
        ImGui::SameLine();
        if (PrimaryButton("Save", ImVec2(buttonWidth, buttonHeight))) {
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
        ImGui::EndDisabled();
        if (cancel && !Picker().IsOpen()) {
            ApplyCurrentTheme();  // undo a previewed appearance
            ImGui::CloseCurrentPopup();
        }
        if (controlsScope_ == 1) DrawControls();
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
    bool openEditProfile_ = false;
    std::string editName_;
    ProfileSettings editProfile_;
    std::string editError_;
    std::string newProfileName_;
    std::string newProfileError_;
    std::unique_ptr<TrophyToast> toast_;
    std::map<std::pair<std::uint32_t, std::int32_t>, std::uint64_t> seenUnlocks_;
    std::optional<TrophySet> runTrophies_;  // the running game's list, for the cards
    bool watchPrimed_ = false;
    Uint64 nextWatch_ = 0;
    Background<TrophyLoad> trophyLoader_;
    std::map<std::string, std::optional<TrophySet>> trophies_;  // by game location
    std::set<std::string> trophyAsked_;
    Background<ArtLoad> artLoader_;
    std::map<std::string, std::vector<std::uint8_t>> art_;
    std::set<std::string> artAsked_;
    bool openTrophies_ = false;
    int trophyFilter_ = 0;
    bool showHidden_ = false;
    bool openControls_ = false;
    std::string controlsSerial_;
    std::string controlsName_;
    int controlsScope_ = 0;  // 0: this game, 1: every game of the profile
    InputFile controlsProfile_;
    InputFile controlsGame_;
    std::string capture_;  // the action waiting for a key
    bool captureReplace_ = false;
    std::string captureNote_;
    std::string controlsError_;
    Background<OnlineLookup> onlineLoader_;
    std::map<std::string, TitleOnlineInfo> online_;
    std::map<std::string, std::string> onlineErrors_;
    std::set<std::string> onlineAsked_;  // titles looked up (or being looked up) this session
    std::string onlineRefresh_;          // a title whose cached answer should be ignored
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
                if (app.TakeEvent(event)) continue;  // a key for the controls window
                ImGui_ImplSDL3_ProcessEvent(&event);
                if (event.type == SDL_EVENT_QUIT) done = true;
                if (event.type == SDL_EVENT_SYSTEM_THEME_CHANGED) app.ThemeChanged();
                if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED && event.window.windowID == SDL_GetWindowID(window)) done = true;
            }
            app.Tick();
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
