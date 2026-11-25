#include <algorithm>
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <cmath>
#include <ctime>
#include <fstream>
#include <iterator>
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

#include <nlohmann/json.hpp>

#include "app/BackgroundJob.hpp"
#include "app/FolderPicker.hpp"
#include "app/IconCache.hpp"
#include "app/Processes.hpp"
#include "core/AnyPS5.hpp"
#include "core/Archive.hpp"
#include "core/Diagnosis.hpp"
#include "core/FileOps.hpp"
#include "core/GameSettings.hpp"
#include "core/InputMapping.hpp"
#include "core/Install.hpp"
#include "core/Launch.hpp"
#include "core/Modules.hpp"
#include "core/PatchInfo.hpp"
#include "core/Playtime.hpp"
#include "core/Profiles.hpp"
#include "core/LaunchLinks.hpp"
#include "core/SaveBackup.hpp"
#include "core/SaveSync.hpp"
#include "core/SelfUpdate.hpp"
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
    float widest = ImGui::GetFontSize() * 9.0f;  // every part as wide as the longest label needs
    for (int index = 0; index < count; ++index) widest = std::max(widest, ImGui::CalcTextSize(labels[index]).x + ImGui::GetStyle().FramePadding.x * 4);
    const float width = std::min(ImGui::GetContentRegionAvail().x, widest * static_cast<float>(count));
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

enum class TopIcon { Versions, Rescan, Settings, Grid, Home };

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
    case TopIcon::Grid: {
        // Four squares.
        const float k = r * 0.9f;
        const float g = r * 0.22f;
        for (int i = 0; i < 4; ++i) {
            const float x = (i % 2 == 0) ? c.x - k : c.x + g;
            const float y = (i < 2) ? c.y - k : c.y + g;
            draw->AddRectFilled(ImVec2(x, y), ImVec2(x + k - g, y + k - g), ink, 2.0f * scale);
        }
        break;
    }
    case TopIcon::Home: {
        // One big tile with two small ones beside it.
        draw->AddRectFilled(ImVec2(c.x - r * 1.1f, c.y - r * 0.75f), ImVec2(c.x + r * 0.15f, c.y + r * 0.75f), ink, 2.5f * scale);
        draw->AddRectFilled(ImVec2(c.x + r * 0.4f, c.y - r * 0.1f), ImVec2(c.x + r * 0.8f, c.y + r * 0.3f), ink, 1.5f * scale);
        draw->AddRectFilled(ImVec2(c.x + r * 0.95f, c.y - r * 0.1f), ImVec2(c.x + r * 1.35f, c.y + r * 0.3f), ink, 1.5f * scale);
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

// What the game settings window shows from disk, read in the background: a shader
// cache can hold thousands of files, and walking it held up the window opening.
struct GameFacts {
    std::string serial;
    std::string cacheSize;  // empty when there's no cache
    bool hasSaves = false;
    std::vector<SaveBackup> backups;
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
void DrawTrophyCup(ImDrawList* draw, ImVec2 at, TrophyGrade grade, float size) {
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
}

void TrophyIcon(TrophyGrade grade, float size) {
    DrawTrophyCup(ImGui::GetWindowDrawList(), ImGui::GetCursorScreenPos(), grade, size);
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

// A launcher update downloaded and unpacked in the background, waiting to be put in place.
struct StagedLauncher {
    std::string tag;
    std::filesystem::path exe;  // empty when it failed
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

// The font of the PS5/PS4 badge on game tiles, when one is beside the launcher:
// fonts\<anything with "phatboy" in its name>.otf or .ttf (Phatboy Slim by
// TracerTong Fontworks, which is not part of the launcher). nullptr: the badge
// uses the launcher's own font.
ImFont* PlatformFont = nullptr;

void LoadPlatformFont(ImGuiIO& io) {
    const char* base = SDL_GetBasePath();
    if (base == nullptr) return;
    const auto folder = FromUtf8(base) / "fonts";
    std::error_code error;
    if (!std::filesystem::is_directory(folder, error)) return;
    std::vector<std::filesystem::path> found;
    for (const auto& item : std::filesystem::directory_iterator(folder, error)) {
        auto name = ToUtf8(item.path().filename());
        std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (name.find("phatboy") == std::string::npos) continue;
        if (!name.ends_with(".otf") && !name.ends_with(".ttf")) continue;
        found.push_back(item.path());
    }
    if (found.empty()) return;
    // The plain weight: a name that says "regular", else the shortest name.
    std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) {
        auto rank = [](const std::filesystem::path& path) {
            auto name = ToUtf8(path.filename());
            std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            const bool styled = name.find("italic") != std::string::npos || name.find("oblique") != std::string::npos;
            return std::pair{(name.find("regular") != std::string::npos ? 0 : 1) + (styled ? 2 : 0), name.size()};
        };
        return rank(a) < rank(b);
    });
    ImFontConfig config;
    PlatformFont = io.Fonts->AddFontFromFileTTF(ToUtf8(found.front()).c_str(), 17.0f, &config);
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
        if (renderer_ != nullptr) SDL_SetRenderTextureAddressMode(renderer_, SDL_TEXTURE_ADDRESS_CLAMP, SDL_TEXTURE_ADDRESS_CLAMP);
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

// Opens a file with its program (a log in the text editor, a picture in the viewer).
void OpenPath(const std::filesystem::path& file) {
#ifdef _WIN32
    win::OpenFolder(file.c_str());
#else
    SDL_OpenURL(("file://" + ToUtf8(std::filesystem::absolute(file))).c_str());
#endif
}

// Deletes a file or folder: to the Recycle Bin on Windows, for good elsewhere.
void MoveToBin(const std::filesystem::path& path) {
#ifdef _WIN32
    if (const auto code = win::RecycleItem(path.c_str()); code != 0) throw std::runtime_error("couldn't move " + ToUtf8(path) + " to the Recycle Bin (error " + std::to_string(code) + ")");
#else
    std::filesystem::remove_all(path);
#endif
}

std::string SystemDescription() {
#ifdef _WIN32
    wchar_t version[256];
    win::WindowsVersion(version, 256);
    return ToUtf8(std::filesystem::path(version));
#else
    return SDL_GetPlatform();
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
        CleanUpLauncherUpdate(LauncherExe(), launcherFolder_);
#ifdef _WIN32
        // anyps5://play/<title ID> and anyps5://game/<title ID> open this launcher (this user only).
        win::RegisterUrlScheme(L"anyps5", L"URL:AnyPS5 Launcher", LauncherExe().c_str());
#endif
        if (settings_.checkUpdates) CheckLauncherUpdate();
        if (!loaded.existed) OpenSetup();
        else if (GameFolders(settings_).empty()) OpenSettings();
        else StartScan();
    }

    // Windows switched between light and dark.
    void ThemeChanged() {
        ApplyCurrentTheme();
    }

    ~LauncherApp() {
        FlushPlaytime();
        if (session_) session_->Stop();
        // A downloaded update goes in place as the launcher closes, so the next start is the new one.
        ApplyLauncherUpdate(false);
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
                const bool known = FindVersion(versions_, id) != nullptr;
                PutVersion(versions_, std::move(entry));
                SaveVersions(versionsRoot_, versions_);
                SelectVersion(id);
                versionsError_.clear();
                if (!known) {
                    // A new build: offer to name it straight away.
                    renamingVersion_ = id;
                    renameText_ = FindVersion(versions_, id)->Label();
                    renameFocus_ = true;
                }
            } catch (const std::exception& failure) {
                versionsError_ = failure.what();
                message_ = Message{Red, "Can't add that AnyPS5", failure.what()};
            }
        }
        if (!backupPick_.empty() && !Picker().IsOpen()) {
            StartBackupAllSaves(FromUtf8(backupPick_));
            backupPick_.clear();
        }
        if (!exportPick_.empty() && !Picker().IsOpen()) {
            ExportGameSettings(FromUtf8(exportPick_));
            exportPick_.clear();
        }
        if (!importPick_.empty() && !Picker().IsOpen()) {
            ImportGameSettings(FromUtf8(importPick_));
            importPick_.clear();
        }
        if (auto load = trophyLoader_.Take()) trophies_[load->key] = std::move(load->set);
        if (auto facts = factsLoader_.Take(); facts && facts->serial == settingsSerial_) {
            cacheSize_ = std::move(facts->cacheSize);
            hasSaves_ = facts->hasSaves;
            backups_ = std::move(facts->backups);
            factsReady_ = true;
        }
        StartGameFacts();
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
            jobLocation_.clear();
            if (!settingsSerial_.empty()) RefreshGameFacts();  // a backup or restore may have just ended
            // A game waiting for its saves: start it now, synced or not (the message says which).
            if (!pendingPlay_.empty()) {
                const auto location = pendingPlay_;
                pendingPlay_.clear();
                for (const auto& game : result_.games) {
                    if (ToUtf8(game.location) == location && PlayBlocker(game).empty()) StartGame(game);
                }
            }
        }
        // Reinstall all: one game after another.
        if (!job_.Busy() && !reinstallQueue_.empty() && !session_) {
            const auto location = reinstallQueue_.front();
            reinstallQueue_.erase(reinstallQueue_.begin());
            for (const auto& game : result_.games) {
                if (ToUtf8(game.location) == location && InstallBlocker(game).empty()) StartInstall(game);
            }
        }
        HandleRequests();
        if (auto list = launcherReleases_.Take()) {
#ifdef LAUNCHER_VERSION
            if (list->error.empty()) launcherUpdate_ = FindLauncherUpdate(list->releases, LAUNCHER_VERSION);
#endif
            launcherCheckError_ = list->error;
            launcherChecked_ = true;
            StageLauncher();  // download it quietly; it's put in place on restart or when the launcher closes
        }
        if (auto staged = launcherStager_.Take()) stagedLauncher_ = std::move(*staged);
        TrackPlaytime();
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
        if (settings_.view == "home") DrawHome();
        else DrawLibrary();
        DrawFooter();
        DrawSetup();
        DrawNewProfile();
        DrawEditProfile();
        DrawLauncherUpdate();
        // Options on a controller opens Settings when nothing else is open.
        if (ImGui::IsKeyPressed(ImGuiKey_GamepadStart, false) && !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId) && !session_) OpenSettings();
        DrawGameSettings();
        DrawScreenshots();
        DrawLaunchScreen();
        DrawMissingRelinker();
        if (!versionsInSettings_) DrawVersions();
        DrawSettings();
        DrawTrophies();
        if (controlsScope_ == 0) DrawControls();  // the profile's controls are drawn over Settings
        ImGui::End();
    }

    bool TakeEvent(const SDL_Event& event) { return CaptureEvent(event); }

    // Runs every loop, even while the launcher is minimized: trophy cards over the game.
    // While a pad's test is on screen the pad only shows there: it doesn't move
    // around Settings or press anything (the mouse and keyboard still do).
    void BeforeFrame() {
        auto& io = ImGui::GetIO();
        const bool testing = padTestDrawn_;
        padTestDrawn_ = false;
        if (testing) io.ConfigFlags &= ~ImGuiConfigFlags_NavEnableGamepad;
        else io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
    }

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
        playtimes_ = LoadPlaytimes(layout_.PlaytimeFile());
    }

    // ---- play time: from the game's window showing to its exit, saved every minute ----

    void TrackPlaytime() {
        if (!session_ || !session_->Running()) return;
        const auto now = SDL_GetTicks();
        if (windowSeenAt_ == 0) {
            if (now - windowCheckedAt_ < 500) return;
            windowCheckedAt_ = now;
            int x = 0, y = 0, w = 0, h = 0;
            if (session_->WindowRect(x, y, w, h) && w > 0) windowSeenAt_ = now;
            // Where the window can't be found, count from the start once it has run a while.
            else if (now - sessionStartedAt_ > 25000) windowSeenAt_ = sessionStartedAt_;
            return;
        }
        if (now - (countedUntil_ != 0 ? countedUntil_ : windowSeenAt_) >= 60000) FlushPlaytime();
    }

    // Adds the time not yet counted to the profile's file.
    void FlushPlaytime() {
        if (!session_ || windowSeenAt_ == 0) return;
        const auto now = SDL_GetTicks();
        const auto from = countedUntil_ != 0 ? countedUntil_ : windowSeenAt_;
        const auto seconds = (now - from) / 1000;
        if (seconds == 0) return;
        try {
            playtimes_[session_->TitleId()] = AddPlaytime(layout_.PlaytimeFile(), session_->TitleId(), seconds, !sessionCounted_, UnixNow());
            countedUntil_ = from + seconds * 1000;
            sessionCounted_ = true;
        } catch (const std::exception&) {
            // Tried again at the next minute; the time isn't lost while the game runs.
        }
    }

    // Counted time plus what the running session has added since.
    std::uint64_t PlayedSeconds(const std::string& serial) const {
        std::uint64_t seconds = 0;
        if (const auto found = playtimes_.find(serial); found != playtimes_.end()) seconds = found->second.seconds;
        if (session_ && session_->TitleId() == serial && windowSeenAt_ != 0) {
            const auto from = countedUntil_ != 0 ? countedUntil_ : windowSeenAt_;
            seconds += (SDL_GetTicks() - from) / 1000;
        }
        return seconds;
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
                Diagnose(serial);
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
        ScanOptions options;
        options.roots = GameFolders(settings_);  // PS5 and PS4
        if (options.roots.empty()) return;
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

    // The AnyPS5 version a game uses: the one it's pinned to, else the selected one.
    const AnyPS5State& StateFor(const std::string& serial) const {
        const auto pin = versions_.pins.find(serial);
        if (pin == versions_.pins.end() || pin->second == settings_.anyps5Version) return anyps5_;
        const auto* entry = FindVersion(versions_, pin->second);
        if (entry == nullptr) return anyps5_;
        auto found = pinnedStates_.find(pin->second);
        if (found == pinnedStates_.end()) {
            // Read once (relinker --version and titles.json), then kept.
            found = pinnedStates_.emplace(pin->second, LoadAnyPS5(ToUtf8(entry->folder), layout_.library / "logs" / "relinker-version.txt")).first;
        }
        return found->second;
    }

    const TitleEntry* TitleFor(const std::string& serial) const {
        const auto& state = StateFor(serial);
        const auto found = state.titles.find(serial);
        return found == state.titles.end() ? nullptr : &found->second;
    }

    std::string VersionLabelFor(const std::string& serial) {
        const auto pin = versions_.pins.find(serial);
        if (pin == versions_.pins.end()) return CurrentVersionLabel();
        const auto* entry = FindVersion(versions_, pin->second);
        return entry != nullptr ? entry->Label() + " (pinned)" : CurrentVersionLabel();
    }

    // Installed games whose relinker isn't the one they'd use now.
    std::vector<const GameEntry*> GamesToReinstall() {
        std::vector<const GameEntry*> games;
        for (const auto& game : result_.games) {
            if (const auto* manifest = InstallFor(game); manifest != nullptr && !ReinstallReason(game, *manifest, StateFor(game.metadata.titleId).version).empty()) {
                games.push_back(&game);
            }
        }
        return games;
    }

    void ReinstallAll() {
        for (const auto* game : GamesToReinstall()) reinstallQueue_.push_back(ToUtf8(game->location));
    }

    const InstallManifest* InstallFor(const GameEntry& game) const {
        const auto found = installs_.find(ToUtf8(game.location));
        return found == installs_.end() ? nullptr : &found->second;
    }

    // A job on this copy of the game (install, remove, sync before play). Jobs for a
    // title rather than a copy (a save backup) count for every copy of it.
    bool JobOn(const GameEntry& game) const {
        if (!job_.Busy() || jobSerial_ != game.metadata.titleId) return false;
        return jobLocation_.empty() || jobLocation_ == ToUtf8(game.location);
    }

    // By the copy, not the title ID: two copies of a game (a .zar and a folder) share the ID.
    bool IsRunning(const GameEntry& game) const {
        return session_ && sessionLocation_ == ToUtf8(game.location);
    }

    // ---- actions -----------------------------------------------------------------

    std::string InstallBlocker(const GameEntry& game) const {
        if (job_.Busy()) return "Another task is running";
        if (IsRunning(game)) return "Stop the game first";
        if (anyps5Pending_ || anyps5Loader_.Running()) return "Still checking the AnyPS5 folder";
        const auto& state = StateFor(game.metadata.titleId);
        if (!state.probe.runtime) return "AnyPS5: " + state.probe.problem;
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
        if (session_) return IsRunning(game) ? "Already running" : "Another game is running";
        if (JobOn(game)) return "Being installed";
        if (InstallFor(game) == nullptr) return "Install it first";
        return {};
    }

    void StartInstall(const GameEntry& game) {
        const auto& state = StateFor(game.metadata.titleId);
        if (!state.probe.runtime) return;
        const auto& runtime = *state.probe.runtime;
        const auto* title = TitleFor(game.metadata.titleId);
        std::optional<TitleEntry> titleCopy;
        if (title != nullptr) titleCopy = *title;
        jobSerial_ = game.metadata.titleId;
        jobLocation_ = ToUtf8(game.location);
        job_.Start("Install " + game.metadata.name, true,
                   [game, runtime, layout = layout_, titleCopy, version = state.version, intel = UseIntelFlag(settings_, hostIntel_)](JobControl& control) {
                       const auto manifest = InstallGame({game, runtime, layout, titleCopy ? &*titleCopy : nullptr, version, TargetIsWindows, intel}, RunToFile, control);
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
        jobLocation_ = ToUtf8(game.location);
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

    // Starts a game; with save sync on, newer saves from the sync folder come in first.
    void Play(const GameEntry& game) {
        if (!settings_.syncFolder.empty() && !job_.Busy() && !session_) {
            const auto layout = layout_;
            const auto serial = game.metadata.titleId;
            const auto root = FromUtf8(settings_.syncFolder);
            pendingPlay_ = ToUtf8(game.location);
            jobSerial_ = serial;
            jobLocation_ = pendingPlay_;
            job_.Start("Sync saves: " + game.metadata.name, false, [layout, serial, root](JobControl& control) {
                const auto result = PullSaves(layout, serial, root, control);
                if (result.action == SyncResult::Action::Pulled) {
                    return std::string("Newer saves came in from the sync folder") + (result.backup ? "; the ones here are kept in " + ToUtf8(result.backup->filename()) : std::string());
                }
                return std::string("Saves are up to date");
            });
            return;
        }
        StartGame(game);
    }

    void StartGame(const GameEntry& game) {
        try {
            // Saves an earlier run left beside the exe (the game crashed, or the launcher
            // stopped): move them back now instead of waiting for a restart.
            if (!session_ && !game.home.empty() && HasSession(game.home)) {
#ifdef _WIN32
                const auto exe = ExecutableIn(game.home, game.metadata.titleId).filename();
                if (win::ProcessRunning(exe.c_str())) {
                    throw std::runtime_error(ToUtf8(exe) + " is still running from the last time (it may hang after a crash); end it in Task Manager, then press Play again");
                }
#endif
                try {
                    MoveOutSession(game.home);
                } catch (const std::exception& failure) {
                    throw std::runtime_error(std::string("the saves of the last run are still beside the exe and can't be moved back: ") + failure.what());
                }
            }
            auto plan = PrepareRun(game, layout_, profileSettings_, TitleFor(game.metadata.titleId), LoadGameSettings(layout_.GameSettingsFile(game.metadata.titleId)));
            session_ = std::make_unique<GameSession>(game.metadata.titleId, game.metadata.name, std::move(plan));
            sessionLocation_ = ToUtf8(game.location);
            sessionStartedAt_ = SDL_GetTicks();
            windowSeenAt_ = 0;
            windowCheckedAt_ = 0;
            countedUntil_ = 0;
            sessionCounted_ = false;
            message_ = Message{Green, "Playing " + game.metadata.name, "Profile " + layout_.profile};
            launching_ = true;
            launchStarted_ = SDL_GetTicks();
        } catch (const std::exception& failure) {
            message_ = Message{Red, "Can't start " + game.metadata.name, failure.what()};
        }
    }

    void FinishSession() {
        FlushPlaytime();  // the last minutes, before the session goes
        WatchTrophies(true);  // an unlock in the last moments
        watchPrimed_ = false;
        std::optional<RunReport> report;
        try {
            report = FinishRun(session_->Plan());
        } catch (const std::exception& failure) {
            message_ = Message{Red, "Saves of " + session_->Name() + " are still beside its exe", std::string(failure.what()) + ". The launcher tries again when you press Play, and at its next start."};
            session_.reset();
            return;
        }
        const std::string serial = session_->TitleId();  // a copy: session_ goes away below
        diagnoses_[serial] = DiagnoseRun(report, ReadLogTail(session_->Plan().logFile, 128 * 1024), session_->ExitCode(), session_->StopRequested());
        launching_ = false;
        if (minimizedForGame_) {
            SDL_RestoreWindow(window_);
            SDL_RaiseWindow(window_);
            minimizedForGame_ = false;
        }
        if (const auto& diagnosis = diagnoses_[serial]; diagnosis.level == RunDiagnosis::Level::Problem) {
            message_ = Message{Red, session_->Name(), diagnosis.headline + (diagnosis.detail.empty() ? std::string() : ": " + diagnosis.detail)};
            session_.reset();
            trophyAsked_.clear();
            PushAfterRun(serial);
            return;
        }
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
        PushAfterRun(serial);
    }

    // With save sync on, this PC's newer saves go to the sync folder after a run.
    void PushAfterRun(const std::string& serial) {
        if (settings_.syncFolder.empty() || job_.Busy()) return;
        const auto layout = layout_;
        const auto root = FromUtf8(settings_.syncFolder);
        job_.Start("Sync saves: " + serial, false, [layout, serial, root](JobControl& control) {
            const auto result = PushSaves(layout, serial, root, control);
            if (result.action == SyncResult::Action::Pushed) return std::string("Saves sent to the sync folder");
            return std::string("The sync folder already has these saves");
        });
    }

    void StartBackupAllSaves(std::filesystem::path target) {
        if (job_.Busy() || session_) return;
        if (target.extension() != ".zip") target += ".zip";
        const auto profiles = backupAllProfiles_ ? profiles_ : std::vector<std::string>{layout_.profile};
        job_.Start("Back up saves", true, [userdata = layout_.userdata, profiles, target](JobControl& control) {
            const auto result = BackUpAllSaves(userdata, profiles, target, control);
            return "Saves of " + std::to_string(result.games) + (result.games == 1 ? " game" : " games") +
                   (result.profiles > 1 ? " from " + std::to_string(result.profiles) + " profiles" : std::string()) + " in " + ToUtf8(target);
        });
    }

    void StartConvertSelf(const GameEntry& game) {
        const auto folder = game.location;
        const auto zip = OriginalsZipFor(game, layout_.library);
        const auto* title = TitleFor(game.metadata.titleId);
        auto extraFolders = title != nullptr ? ModuleDirectories(*title) : std::vector<std::string>{};
        extraFolders.insert(extraFolders.end(), game.moduleDirs.begin(), game.moduleDirs.end());
        const auto executables = title != nullptr ? title->executables : std::vector<std::string>{};
        job_.Start("SELF to ELF: " + game.metadata.name, true, [folder, zip, extraFolders, executables](JobControl& control) mutable {
            // Every folder the game keeps modules in, looked for now rather than at each scan.
            FolderPackage package(folder);
            for (auto& found : FindModuleDirectories(package)) extraFolders.push_back(std::move(found));
            const auto moduleFolders = ModuleFolders(extraFolders);  // drops repeats
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
        const auto zip = OriginalsZipFor(game, layout_.library);
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
    static const char* PadTypeName(SDL_GamepadType type) {
        switch (type) {
        case SDL_GAMEPAD_TYPE_PS5: return "DualSense";
        case SDL_GAMEPAD_TYPE_PS4: return "DualShock 4";
        case SDL_GAMEPAD_TYPE_PS3: return "DualShock 3";
        case SDL_GAMEPAD_TYPE_XBOX360: return "Xbox 360";
        case SDL_GAMEPAD_TYPE_XBOXONE: return "Xbox";
        case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO: return "Switch Pro";
        case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR: return "Joy-Con pair";
        default: return "Gamepad";
        }
    }

    // The connected pads with their kind, connection and battery; a live test and vibration.
    void DrawControllers() {
        int count = 0;
        SDL_JoystickID* ids = SDL_GetGamepads(&count);
        std::set<SDL_JoystickID> present;
        for (int i = 0; i < count; ++i) {
            present.insert(ids[i]);
            if (pads_.count(ids[i]) == 0) {
                if (SDL_Gamepad* pad = SDL_OpenGamepad(ids[i])) pads_[ids[i]] = pad;
            }
        }
        SDL_free(ids);
        for (auto it = pads_.begin(); it != pads_.end();) {
            if (present.count(it->first) == 0) {
                SDL_CloseGamepad(it->second);
                it = pads_.erase(it);
            } else {
                ++it;
            }
        }
        if (pads_.empty()) {
            ImGui::TextColored(Amber, "No controller found.");
            ImGui::TextColored(Dim, "USB: use a cable that carries data, not only charge. Bluetooth: pair it in Windows first (DualSense and DualShock 4: hold Create/Share and PS until the light flashes).");
        }
        int player = 1;
        for (const auto& [id, pad] : pads_) {
            ImGui::PushID(static_cast<int>(id));
            const char* name = SDL_GetGamepadName(pad);
            const auto type = SDL_GetGamepadType(pad);
            const auto connection = SDL_GetGamepadConnectionState(pad);
            int percent = -1;
            const auto power = SDL_GetGamepadPowerInfo(pad, &percent);
            std::string facts = PadTypeName(type);
            facts += connection == SDL_JOYSTICK_CONNECTION_WIRELESS ? "  \xC2\xB7  Bluetooth" : connection == SDL_JOYSTICK_CONNECTION_WIRED ? "  \xC2\xB7  USB" : "";
            if (power == SDL_POWERSTATE_CHARGING) facts += "  \xC2\xB7  charging";
            if (percent >= 0) facts += "  \xC2\xB7  battery " + std::to_string(percent) + "%";
            // Name and facts on the left; Test and Vibrate on the right, like the other rows' buttons.
            const auto& style = ImGui::GetStyle();
            const float buttonWidth = ImGui::CalcTextSize("Stop test").x + style.FramePadding.x * 4.0f;
            const float buttonsWidth = buttonWidth * 2.0f + style.ItemSpacing.x;
            const float left = ImGui::GetCursorPosX();
            const float right = left + ImGui::GetContentRegionAvail().x;
            const float top = ImGui::GetCursorPosY();
            ImGui::BeginGroup();
            ImGui::PushTextWrapPos(right - buttonsWidth - style.ItemSpacing.x * 2.0f);
            ImGui::Text("%d. %s", player++, name != nullptr ? name : "Gamepad");
            ImGui::TextColored(percent >= 0 && percent <= 15 && power != SDL_POWERSTATE_CHARGING ? Amber : Dim, "%s", facts.c_str());
            ImGui::PopTextWrapPos();
            ImGui::EndGroup();
            const float textHeight = ImGui::GetItemRectSize().y;
            const float rowHeight = std::max(textHeight, ImGui::GetFrameHeight());
            ImGui::SameLine();
            ImGui::SetCursorPos(ImVec2(right - buttonsWidth, top + (rowHeight - ImGui::GetFrameHeight()) * 0.5f));
            if (ImGui::Button(padTest_ == id ? "Stop test" : "Test", ImVec2(buttonWidth, 0))) padTest_ = padTest_ == id ? 0 : id;
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", "Shows the buttons, sticks and triggers as you press them");
            ImGui::SameLine();
            if (ImGui::Button("Vibrate", ImVec2(buttonWidth, 0))) SDL_RumbleGamepad(pad, 0xC000, 0xC000, 500);
            ImGui::SetCursorPosY(top + rowHeight + style.ItemSpacing.y);
            if (padTest_ == id) {
                DrawPadTest(pad);
                ImGui::TextColored(Dim, "%s", "While the test is open the pad only shows here; Stop test gives it back to the launcher.");
            }
            ImGui::Spacing();
            ImGui::PopID();
        }
#ifdef _WIN32
        // Programs that take pads away from games, or change what they look like.
        struct Taker {
            const wchar_t* exe;
            const char* text;
        };
        static const Taker takers[] = {
            {L"steam.exe", "Steam is running: Steam Input can turn a pad into a mouse or an Xbox pad. Turn it off for PlayStation pads (Steam > Settings > Controller), or close Steam before playing."},
            {L"DS4Windows.exe", "DS4Windows is running: it can hide the DualShock or DualSense behind a virtual Xbox pad. Close it, or turn off its 'Hide DS4 controller' option."},
            {L"DSX.exe", "DualSenseX is running: it can hide the DualSense behind a virtual pad. Close it before playing."},
            {L"DualSenseX.exe", "DualSenseX is running: it can hide the DualSense behind a virtual pad. Close it before playing."},
            {L"HidHideClient.exe", "HidHide's client is open: if the pad is hidden there, add the game's exe to its allowed programs."},
        };
        for (const auto& taker : takers) {
            if (win::ProcessRunning(taker.exe)) {
                ImGui::PushTextWrapPos(0.0f);
                ImGui::TextColored(Amber, "%s", taker.text);
                ImGui::PopTextWrapPos();
            }
        }
#endif
    }

    // A live picture of the pad: face buttons, shoulders, d-pad, both sticks and the triggers.
    void DrawPadTest(SDL_Gamepad* pad) {
        padTestDrawn_ = true;
        const float scale = scale_;
        const float width = std::min(ImGui::GetContentRegionAvail().x, 440.0f * scale);
        const float height = width * 0.5f;
        const ImVec2 at = ImGui::GetCursorScreenPos();
        ImGui::Dummy(ImVec2(width, height));
        auto* draw = ImGui::GetWindowDrawList();
        draw->AddRectFilled(at, ImVec2(at.x + width, at.y + height), ImGui::GetColorU32(WindowColor), 16.0f * scale);
        const ImU32 off = ImGui::GetColorU32(Fade(ImGui::GetStyle().Colors[ImGuiCol_Text], 0.25f));
        const ImU32 on = ImGui::GetColorU32(Accent);
        const auto pressed = [&](SDL_GamepadButton button) { return SDL_GetGamepadButton(pad, button); };
        const auto button = [&](ImVec2 c, float r, SDL_GamepadButton which) { draw->AddCircleFilled(c, r, pressed(which) ? on : off); };
        const float u = width / 22.0f;
        // Face buttons on the right, d-pad on the left.
        const ImVec2 face(at.x + u * 17, at.y + u * 4.2f);
        button(ImVec2(face.x, face.y - u * 1.3f), u * 0.6f, SDL_GAMEPAD_BUTTON_NORTH);
        button(ImVec2(face.x, face.y + u * 1.3f), u * 0.6f, SDL_GAMEPAD_BUTTON_SOUTH);
        button(ImVec2(face.x - u * 1.3f, face.y), u * 0.6f, SDL_GAMEPAD_BUTTON_WEST);
        button(ImVec2(face.x + u * 1.3f, face.y), u * 0.6f, SDL_GAMEPAD_BUTTON_EAST);
        const ImVec2 dpad(at.x + u * 5, at.y + u * 4.2f);
        const auto arm = [&](float dx, float dy, SDL_GamepadButton which) {
            const ImVec2 c(dpad.x + dx * u * 1.2f, dpad.y + dy * u * 1.2f);
            draw->AddRectFilled(ImVec2(c.x - u * 0.55f, c.y - u * 0.55f), ImVec2(c.x + u * 0.55f, c.y + u * 0.55f), pressed(which) ? on : off, u * 0.15f);
        };
        arm(0, -1, SDL_GAMEPAD_BUTTON_DPAD_UP);
        arm(0, 1, SDL_GAMEPAD_BUTTON_DPAD_DOWN);
        arm(-1, 0, SDL_GAMEPAD_BUTTON_DPAD_LEFT);
        arm(1, 0, SDL_GAMEPAD_BUTTON_DPAD_RIGHT);
        // Shoulders and triggers along the top.
        const auto bar = [&](float x, SDL_GamepadButton bumper, SDL_GamepadAxis trigger) {
            draw->AddRectFilled(ImVec2(at.x + x, at.y + u * 0.5f), ImVec2(at.x + x + u * 3, at.y + u * 1.1f), pressed(bumper) ? on : off, u * 0.3f);
            const float t = static_cast<float>(SDL_GetGamepadAxis(pad, trigger)) / 32767.0f;
            draw->AddRect(ImVec2(at.x + x + u * 3.4f, at.y + u * 0.3f), ImVec2(at.x + x + u * 3.9f, at.y + u * 1.6f), off, u * 0.1f);
            draw->AddRectFilled(ImVec2(at.x + x + u * 3.4f, at.y + u * 1.6f - u * 1.3f * std::clamp(t, 0.0f, 1.0f)), ImVec2(at.x + x + u * 3.9f, at.y + u * 1.6f), on, u * 0.1f);
        };
        bar(u * 2.5f, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, SDL_GAMEPAD_AXIS_LEFT_TRIGGER);
        bar(u * 15.5f, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER);
        // Sticks, with AnyPS5's default dead zone of 0 shown as the centre dot.
        const auto stick = [&](ImVec2 c, SDL_GamepadAxis ax, SDL_GamepadAxis ay, SDL_GamepadButton click) {
            const float r = u * 1.6f;
            draw->AddCircle(c, r, off, 0, 1.5f * scale);
            const float x = static_cast<float>(SDL_GetGamepadAxis(pad, ax)) / 32767.0f;
            const float y = static_cast<float>(SDL_GetGamepadAxis(pad, ay)) / 32767.0f;
            draw->AddCircleFilled(ImVec2(c.x + std::clamp(x, -1.0f, 1.0f) * r, c.y + std::clamp(y, -1.0f, 1.0f) * r), u * 0.55f, pressed(click) ? on : ImGui::GetColorU32(ImGuiCol_Text));
        };
        stick(ImVec2(at.x + u * 8, at.y + u * 8.2f), SDL_GAMEPAD_AXIS_LEFTX, SDL_GAMEPAD_AXIS_LEFTY, SDL_GAMEPAD_BUTTON_LEFT_STICK);
        stick(ImVec2(at.x + u * 14, at.y + u * 8.2f), SDL_GAMEPAD_AXIS_RIGHTX, SDL_GAMEPAD_AXIS_RIGHTY, SDL_GAMEPAD_BUTTON_RIGHT_STICK);
        // Options, Create and the touchpad in the middle.
        button(ImVec2(at.x + u * 9, at.y + u * 3), u * 0.35f, SDL_GAMEPAD_BUTTON_BACK);
        button(ImVec2(at.x + u * 13, at.y + u * 3), u * 0.35f, SDL_GAMEPAD_BUTTON_START);
        draw->AddRectFilled(ImVec2(at.x + u * 9.6f, at.y + u * 2), ImVec2(at.x + u * 12.4f, at.y + u * 4.6f), pressed(SDL_GAMEPAD_BUTTON_TOUCHPAD) ? on : off, u * 0.3f);
        button(ImVec2(at.x + u * 11, at.y + u * 5.6f), u * 0.4f, SDL_GAMEPAD_BUTTON_GUIDE);
    }

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
        const float buttons = ImGui::GetFrameHeight() * 1.2f * 4 + style.ItemSpacing.x * 3;
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
        {
            const bool home = settings_.view == "home";
            if (IconButton("view", home ? TopIcon::Grid : TopIcon::Home, home ? "Show the games as a grid" : "Show the games as a row, the selected one large", false)) {
                settings_.view = home ? "grid" : "home";
                try {
                    SaveSettings(settingsFile_, settings_);
                } catch (const std::exception&) {
                }
            }
            ImGui::SameLine();
            ImGui::SetCursorPosY(iconY);
        }
        if (IconButton("versions", TopIcon::Versions, session_ != nullptr ? "AnyPS5 versions (not while a game runs)" : "AnyPS5 versions", session_ != nullptr)) OpenVersions();
        ImGui::SameLine();
        ImGui::SetCursorPosY(iconY);
        const bool noFolder = GameFolders(settings_).empty();
        const char* rescanTip = scan_.Running() ? "Scanning..." : noFolder ? "Rescan (set a games folder in Settings first)" : "Rescan the games folders";
        if (IconButton("rescan", TopIcon::Rescan, rescanTip, scan_.Running() || job_.Busy() || noFolder, scan_.Running())) StartScan();
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
        if (const auto outdated = GamesToReinstall(); !outdated.empty()) {
            ImGui::SameLine();
            const std::string label = std::to_string(outdated.size()) + (outdated.size() == 1 ? " game to reinstall" : " games to reinstall");
            Pill(label.c_str(), Amber);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Converted with another AnyPS5 than the one they use now. Click to reinstall them one after another.");
            if (ImGui::IsItemClicked() && !job_.Busy() && !session_ && reinstallQueue_.empty()) ReinstallAll();
        }
        if (!reinstallQueue_.empty()) {
            ImGui::SameLine();
            Pill((std::to_string(reinstallQueue_.size()) + " more to reinstall").c_str(), Dim);
        }
        if (launcherUpdate_) {
            ImGui::SameLine();
            const auto& tag = launcherUpdate_->release.tag;
            if (LauncherUpdateReady()) {
                Pill(("Update ready: " + tag).c_str(), Green);
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Downloaded. Click to see what's new and restart, or keep going: it's put in place when you close the launcher.");
            } else if (launcherStager_.Running()) {
                Pill(("Launcher " + tag + "  " + std::to_string(StagePercent()) + "%").c_str(), Dim);
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Downloading the new launcher in the background.");
            } else if (stagedLauncher_ && !stagedLauncher_->error.empty()) {
                Pill(("Launcher " + tag).c_str(), Amber);
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("The download failed: %s\nClick to see the update and try again.", stagedLauncher_->error.c_str());
            } else {
                Pill(("Launcher " + tag).c_str(), Green);
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("A newer launcher is out. Click to see what's new.");
            }
            if (ImGui::IsItemClicked()) StartLauncherUpdate();
        }
        ImGui::SameLine();
        {
            std::string folders;
            if (!settings_.gamesFolder.empty()) folders = "PS5 " + settings_.gamesFolder;
            if (!settings_.ps4GamesFolder.empty()) folders += (folders.empty() ? "" : "   \xC2\xB7   ") + std::string("PS4 ") + settings_.ps4GamesFolder;
            ImGui::TextColored(Dim, "%s", folders.empty() ? "No games folder set" : folders.c_str());
        }
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

    // "PS5" or "PS4" in a small dark tag, as the console's own home screen marks games.
    // right: the tag's top-right corner. height: the tag's height.
    void DrawPlatformTag(ImDrawList* draw, const GameEntry& game, ImVec2 topRight, float height) const {
        const auto platform = PlatformOf(game.metadata.titleId);
        if (platform.empty()) return;
        ImFont* font = PlatformFont != nullptr ? PlatformFont : ImGui::GetFont();
        const float size = height * (PlatformFont != nullptr ? 0.78f : 0.62f);
        const ImVec2 text = font->CalcTextSizeA(size, FLT_MAX, 0.0f, platform.c_str());
        const float padX = height * 0.36f;
        const ImVec2 start(topRight.x - text.x - padX * 2, topRight.y);
        const ImVec2 end(topRight.x, topRight.y + height);
        draw->AddRectFilled(start, end, IM_COL32(12, 14, 18, 215), height * 0.28f);
        const ImVec2 at(start.x + padX, start.y + (height - text.y) * 0.5f);
        draw->AddText(font, size, at, IM_COL32(255, 255, 255, 240), platform.c_str());
        if (PlatformFont == nullptr) draw->AddText(font, size, ImVec2(at.x + 0.6f * scale_, at.y), IM_COL32(255, 255, 255, 240), platform.c_str());  // bolder
    }

    Badge BadgeFor(const GameEntry& game) const {
        if (IsRunning(game)) return {"Running", Accent};
        if (JobOn(game)) return {"Working...", Amber};
        if (const auto* manifest = InstallFor(game)) {
            if (!ReinstallReason(game, *manifest, StateFor(game.metadata.titleId).version).empty()) return {"Reinstall", Amber};
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
        ImGui::TextColored(Dim, "%s", scan_.Running() ? "Looking for games..." : GameFolders(settings_).empty() ? "Set your games folder in Settings." : "No games found.");
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
            // With a controller or the arrow keys: moving onto a tile selects it, ✕ (or Enter) plays it, △ opens its actions.
            if (ImGui::IsItemFocused() && ImGui::GetIO().NavVisible) {
                if (selected_ != key) selected_ = key;
                if (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceDown, false) || ImGui::IsKeyPressed(ImGuiKey_Enter, false)) ActivateGame(game);
                if (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceUp, false)) ImGui::OpenPopup("actions");
            }
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
            DrawPlatformTag(draw, game, ImVec2(at.x + tile - 8.0f * scale_, at.y + 8.0f * scale_), ImGui::GetTextLineHeight() + 6.0f * scale_);
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


    // ---- first start ------------------------------------------------------------------

    void OpenSetup() {
        draft_ = settings_;
        setupStep_ = 0;
        setupProfile_ = layout_.profile.empty() ? std::string(DefaultProfileName) : layout_.profile;
        setupError_.clear();
        setupCounted_.clear();
        setupGameCount_.reset();
        missingAsked_ = true;  // the setup has its own AnyPS5 step
        openSetup_ = true;
    }

    // ---- first start: a sidebar with the steps, the step itself on the right -------------

    static constexpr int SetupSteps = 4;

    struct SetupStepText {
        const char* title;
        const char* subtitle;
    };

    static constexpr SetupStepText SetupStepTexts[SetupSteps] = {
        {"Look and feel", "Theme and library view"},
        {"Your games", "Where your dumps are"},
        {"AnyPS5", "Relinker and libraries"},
        {"Your profile", "Saves, trophies, controls"},
    };

    static ImU32 Col(int rgb, float alpha = 1.0f) {
        return IM_COL32((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF, static_cast<int>(alpha * 255.0f));
    }

    // A check mark inside a box of this size.
    static void DrawCheck(ImDrawList* draw, ImVec2 center, float size, ImU32 color, float thickness) {
        const ImVec2 points[3] = {ImVec2(center.x - size * 0.30f, center.y + size * 0.02f), ImVec2(center.x - size * 0.08f, center.y + size * 0.24f),
                                  ImVec2(center.x + size * 0.32f, center.y - size * 0.22f)};
        draw->AddPolyline(points, 3, color, ImDrawFlags_None, thickness);
    }

    // The app's mark: a rounded square in the accent with a play triangle.
    void DrawLogo(ImDrawList* draw, ImVec2 at, float size) {
        const ImU32 top = ImGui::GetColorU32(AccentHover);
        const ImU32 bottom = ImGui::GetColorU32(AccentActive);
        draw->AddRectFilled(at, ImVec2(at.x + size, at.y + size), bottom, size * 0.28f);
        draw->AddRectFilled(at, ImVec2(at.x + size, at.y + size * 0.6f), top, size * 0.28f, ImDrawFlags_RoundCornersTop);
        draw->AddRectFilledMultiColor(ImVec2(at.x, at.y + size * 0.3f), ImVec2(at.x + size, at.y + size * 0.6f), top, top, bottom, bottom);
        const ImVec2 c(at.x + size * 0.53f, at.y + size * 0.5f);
        const float r = size * 0.22f;
        draw->AddTriangleFilled(ImVec2(c.x - r * 0.75f, c.y - r), ImVec2(c.x - r * 0.75f, c.y + r), ImVec2(c.x + r, c.y), IM_COL32_WHITE);
    }

    // A card to pick: hover and selected states, a check badge when selected. paint draws inside.
    template <typename Paint>
    bool ChoiceCard(const char* id, ImVec2 size, bool selected, Paint paint) {
        const ImVec2 at = ImGui::GetCursorScreenPos();
        const bool clicked = ImGui::InvisibleButton(id, size);
        const bool hovered = ImGui::IsItemHovered();
        auto* draw = ImGui::GetWindowDrawList();
        const float rounding = 12.0f * scale_;
        const ImVec2 end(at.x + size.x, at.y + size.y);
        draw->AddRectFilled(at, end, ImGui::GetColorU32(hovered ? CardHover : Card), rounding);
        paint(draw, at, end);
        if (selected) {
            draw->AddRect(at, end, ImGui::GetColorU32(Accent), rounding, 0, 2.0f * scale_);
            const float badge = 20.0f * scale_;
            const ImVec2 c(end.x - badge * 0.5f - 8.0f * scale_, at.y + badge * 0.5f + 8.0f * scale_);
            draw->AddCircleFilled(c, badge * 0.5f, ImGui::GetColorU32(Accent));
            DrawCheck(draw, c, badge * 0.8f, IM_COL32_WHITE, 2.0f * scale_);
        } else {
            draw->AddRect(at, end, ImGui::GetColorU32(hovered ? Fade(Accent, 0.5f) : ImGui::GetStyle().Colors[ImGuiCol_Border]), rounding, 0, 1.0f * scale_);
        }
        return clicked;
    }

    // A small window in a palette: title bar, a side panel and three tiles.
    void DrawMiniWindow(ImDrawList* draw, ImVec2 at, ImVec2 end, const Palette& p) {
        const float s = scale_;
        draw->AddRectFilled(at, end, Col(p.background), 6.0f * s);
        const float w = end.x - at.x;
        const float h = end.y - at.y;
        draw->AddRectFilled(ImVec2(at.x + 8 * s, at.y + 8 * s), ImVec2(at.x + w * 0.42f, at.y + 14 * s), Col(p.frame), 3.0f * s);
        draw->AddCircleFilled(ImVec2(end.x - 12 * s, at.y + 11 * s), 3.5f * s, Col(p.accent));
        const float tileTop = at.y + 22 * s;
        const float tile = std::min((w - 8 * s * 4) / 3.0f, h - 30 * s);
        for (int i = 0; i < 3; ++i) {
            const float x = at.x + 8 * s + static_cast<float>(i) * (tile + 8 * s);
            draw->AddRectFilled(ImVec2(x, tileTop), ImVec2(x + tile, tileTop + tile), Col(i == 0 ? p.accent : p.card, i == 0 ? 0.85f : 1.0f), 4.0f * s);
        }
    }

    void DrawSetupSidebar(float width) {
        const auto& style = ImGui::GetStyle();
        ImGui::BeginChild("setup-side", ImVec2(width, 0), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoBackground);
        auto* draw = ImGui::GetWindowDrawList();
        const ImVec2 origin = ImGui::GetWindowPos();
        const ImVec2 size = ImGui::GetWindowSize();
        const float s = scale_;
        const ImVec2 end(origin.x + size.x, origin.y + size.y);
        draw->AddRectFilled(origin, end, ImGui::GetColorU32(Card), 14.0f * s);
        // A soft wash of the accent from the top.
        const ImU32 wash = ImGui::GetColorU32(Fade(Accent, 0.16f));
        const ImU32 clear = ImGui::GetColorU32(Fade(Accent, 0.0f));
        draw->AddRectFilledMultiColor(ImVec2(origin.x, origin.y + 14 * s), ImVec2(end.x, origin.y + size.y * 0.45f), wash, wash, clear, clear);
        draw->AddRectFilled(origin, ImVec2(end.x, origin.y + 14 * s), wash, 14.0f * s, ImDrawFlags_RoundCornersTop);

        const float pad = 22.0f * s;
        const float logo = 40.0f * s;
        DrawLogo(draw, ImVec2(origin.x + pad, origin.y + pad), logo);
        ImGui::SetCursorScreenPos(ImVec2(origin.x + pad + logo + 12 * s, origin.y + pad + 1 * s));
        ImGui::PushFont(nullptr, style.FontSizeBase * 1.15f);
        ImGui::TextUnformatted("AnyPS5 Launcher");
        ImGui::PopFont();
        ImGui::SetCursorScreenPos(ImVec2(origin.x + pad + logo + 12 * s, ImGui::GetCursorScreenPos().y - 2 * s));
        ImGui::TextColored(Dim, "%s", "First-time setup");

        // The steps, one under the other, joined by a line.
        const float radius = 14.0f * s;
        const float rowHeight = 64.0f * s;
        const float top = origin.y + pad + logo + 40.0f * s;
        for (int i = 0; i < SetupSteps; ++i) {
            const ImVec2 c(origin.x + pad + radius, top + static_cast<float>(i) * rowHeight + radius);
            if (i + 1 < SetupSteps) {
                draw->AddLine(ImVec2(c.x, c.y + radius + 4 * s), ImVec2(c.x, c.y + rowHeight - radius - 4 * s),
                              ImGui::GetColorU32(i < setupStep_ ? Accent : Fade(Dim, 0.35f)), 2.0f * s);
            }
            const bool done = i < setupStep_;
            const bool current = i == setupStep_;
            if (done) {
                draw->AddCircleFilled(c, radius, ImGui::GetColorU32(Accent));
                DrawCheck(draw, c, radius * 1.3f, IM_COL32_WHITE, 2.2f * s);
            } else if (current) {
                draw->AddCircleFilled(c, radius + 4 * s, ImGui::GetColorU32(Fade(Accent, 0.22f)));
                draw->AddCircleFilled(c, radius, ImGui::GetColorU32(Accent));
            } else {
                draw->AddCircle(c, radius, ImGui::GetColorU32(Fade(Dim, 0.6f)), 0, 1.6f * s);
            }
            if (!done) {
                const std::string number = std::to_string(i + 1);
                const ImVec2 text = ImGui::CalcTextSize(number.c_str());
                draw->AddText(ImVec2(c.x - text.x * 0.5f, c.y - text.y * 0.5f), current ? IM_COL32_WHITE : ImGui::GetColorU32(Dim), number.c_str());
            }
            const float textX = c.x + radius + 14 * s;
            draw->AddText(ImVec2(textX, c.y - ImGui::GetFontSize() * 0.95f), ImGui::GetColorU32(current || done ? ImGuiCol_Text : ImGuiCol_TextDisabled),
                          SetupStepTexts[i].title);
            draw->AddText(ImVec2(textX, c.y + 1 * s), ImGui::GetColorU32(Dim), SetupStepTexts[i].subtitle);
            // Done steps can be clicked to go back to them.
            if (done) {
                ImGui::SetCursorScreenPos(ImVec2(origin.x + pad * 0.5f, c.y - rowHeight * 0.4f));
                ImGui::PushID(i);
                if (ImGui::InvisibleButton("step", ImVec2(size.x - pad, rowHeight * 0.8f))) setupStep_ = i;
                if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                ImGui::PopID();
            }
        }
        const char* foot = "Everything here can be changed later in Settings.";
        ImGui::SetCursorScreenPos(ImVec2(origin.x + pad, end.y - pad - ImGui::GetTextLineHeight() * 2.2f));
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + size.x - pad * 2);
        ImGui::TextColored(Dim, "%s", foot);
        ImGui::PopTextWrapPos();
        ImGui::EndChild();
    }

    // Counts the games in the chosen folder in the background, so the step can say what it found.
    void CountSetupGames() {
        // Wait until the path stops changing, so typing it doesn't scan every prefix.
        if (draft_.gamesFolder != setupSeen_) {
            setupSeen_ = draft_.gamesFolder;
            setupSeenAt_ = SDL_GetTicks();
        }
        if (SDL_GetTicks() - setupSeenAt_ < 600) return;
        if (draft_.gamesFolder == setupCounted_ || setupCounter_.Running()) return;
        setupCounted_ = draft_.gamesFolder;
        setupGameCount_.reset();
        if (draft_.gamesFolder.empty()) return;
        setupCounter_.Start([folder = FromUtf8(draft_.gamesFolder)] {
            std::error_code error;
            if (!std::filesystem::is_directory(folder, error)) return -1;
            try {
                ScanOptions options;
                options.roots = {folder};
                options.maxIconSize = 0;
                return static_cast<int>(Scan(options).games.size());
            } catch (const std::exception&) {
                return -1;
            }
        });
    }

    void DrawSetupStep(float width) {
        const auto& style = ImGui::GetStyle();
        const float s = scale_;
        const auto title = [&](const char* text, const char* note) {
            ImGui::PushFont(nullptr, style.FontSizeBase * 1.9f);
            ImGui::TextUnformatted(text);
            ImGui::PopFont();
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextColored(Dim, "%s", note);
            ImGui::PopTextWrapPos();
            ImGui::Dummy(ImVec2(0, 14 * s));
        };
        const auto label = [&](const char* text) {
            ImGui::PushFont(nullptr, style.FontSizeBase * 1.05f);
            ImGui::TextUnformatted(text);
            ImGui::PopFont();
            ImGui::Dummy(ImVec2(0, 2 * s));
        };
        bool canGoOn = true;
        if (setupStep_ == 0) {
            title("Welcome", "Your PS5 game dumps, converted with AnyPS5 and ready to play, with saves, trophies and controls per profile. Four quick steps; first, how it should look.");
            label("Theme");
            const float gap = 12.0f * s;
            const float cardWidth = std::min(190.0f * s, (width - gap * 2) / 3.0f);
            const ImVec2 card(cardWidth, cardWidth * 0.50f + ImGui::GetTextLineHeight() + 20 * s);
            const char* names[] = {"Follow Windows", "Dark", "Light"};
            const char* values[] = {"system", "dark", "light"};
            for (int i = 0; i < 3; ++i) {
                if (i > 0) ImGui::SameLine(0, gap);
                ImGui::PushID(i);
                const bool picked = draft_.theme == values[i];
                if (ChoiceCard("theme", card, picked, [&](ImDrawList* draw, ImVec2 at, ImVec2 end) {
                        const ImVec2 previewAt(at.x + 10 * s, at.y + 10 * s);
                        const ImVec2 previewEnd(end.x - 10 * s, at.y + 10 * s + cardWidth * 0.50f - 10 * s);
                        if (i == 0) {
                            // Half dark, half light.
                            const float mid = (previewAt.x + previewEnd.x) * 0.5f;
                            draw->PushClipRect(previewAt, ImVec2(mid, previewEnd.y), true);
                            DrawMiniWindow(draw, previewAt, previewEnd, DarkPalette);
                            draw->PopClipRect();
                            draw->PushClipRect(ImVec2(mid, previewAt.y), previewEnd, true);
                            DrawMiniWindow(draw, previewAt, previewEnd, LightPalette);
                            draw->PopClipRect();
                        } else {
                            DrawMiniWindow(draw, previewAt, previewEnd, i == 1 ? DarkPalette : LightPalette);
                        }
                        draw->AddText(ImVec2(at.x + 12 * s, end.y - ImGui::GetTextLineHeight() - 9 * s), ImGui::GetColorU32(ImGuiCol_Text), names[i]);
                    })) {
                    draft_.theme = values[i];
                    auto& imguiStyle = ImGui::GetStyle();
                    ApplyTheme(imguiStyle, WantDark(draft_.theme));
                    imguiStyle.ScaleAllSizes(scale_);
                    imguiStyle.FontScaleDpi = scale_;
                }
                ImGui::PopID();
            }
            ImGui::Dummy(ImVec2(0, 14 * s));
            label("Library");
            const float viewWidth = std::min(290.0f * s, (width - gap) / 2.0f);
            const float previewHeight = 72.0f * s;
            const ImVec2 viewCard(viewWidth, previewHeight + 20 * s + ImGui::GetTextLineHeightWithSpacing() * 2 + 8 * s);
            const char* viewNames[] = {"Grid", "Home row"};
            const char* viewNotes[] = {"Covers in a grid, details on the side", "Big artwork, one game at a time"};
            for (int i = 0; i < 2; ++i) {
                if (i > 0) ImGui::SameLine(0, gap);
                ImGui::PushID(10 + i);
                const bool picked = (draft_.view == "home") == (i == 1);
                if (ChoiceCard("view", viewCard, picked, [&](ImDrawList* draw, ImVec2 at, ImVec2 end) {
                        const ImU32 tileColor = ImGui::GetColorU32(Fade(Accent, 0.75f));
                        const ImU32 faint = ImGui::GetColorU32(Fade(Dim, 0.30f));
                        const ImVec2 art(at.x + 10 * s, at.y + 10 * s);
                        const ImVec2 artEnd(end.x - 10 * s, at.y + 10 * s + previewHeight - 10 * s);
                        const float w = artEnd.x - art.x;
                        const float h = artEnd.y - art.y;
                        draw->AddRectFilled(art, artEnd, ImGui::GetColorU32(Fade(WindowColor, 0.9f)), 6 * s);
                        const float m = 7 * s;  // margin inside the preview
                        if (i == 0) {
                            const float panel = w * 0.28f;
                            const float t = std::min((h - m * 3) / 2.0f, (w - panel - m * 5) / 3.0f);
                            for (int r = 0; r < 2; ++r) {
                                for (int c = 0; c < 3; ++c) {
                                    const ImVec2 a(art.x + m + static_cast<float>(c) * (t + m), art.y + m + static_cast<float>(r) * (t + m));
                                    draw->AddRectFilled(a, ImVec2(a.x + t, a.y + t), r == 0 && c == 0 ? tileColor : faint, 4 * s);
                                }
                            }
                            draw->AddRectFilled(ImVec2(artEnd.x - m - panel, art.y + m), ImVec2(artEnd.x - m, artEnd.y - m), faint, 4 * s);
                        } else {
                            draw->AddRectFilledMultiColor(art, artEnd, ImGui::GetColorU32(Fade(Accent, 0.35f)), ImGui::GetColorU32(Fade(Accent, 0.10f)),
                                                          ImGui::GetColorU32(Fade(Accent, 0.02f)), ImGui::GetColorU32(Fade(Accent, 0.20f)));
                            const float big = (h - m * 2) * 0.72f;
                            const float small = big * 0.62f;
                            draw->AddRectFilled(ImVec2(art.x + m, art.y + m), ImVec2(art.x + m + big, art.y + m + big), tileColor, 5 * s);
                            for (int c = 0; c < 4; ++c) {
                                const float x = art.x + m * 2 + big + static_cast<float>(c) * (small + 5 * s);
                                if (x + small > artEnd.x - m) break;
                                draw->AddRectFilled(ImVec2(x, art.y + m + big - small), ImVec2(x + small, art.y + m + big), faint, 4 * s);
                            }
                            draw->AddRectFilled(ImVec2(art.x + m, artEnd.y - m - 5 * s), ImVec2(art.x + m + big * 1.3f, artEnd.y - m), faint, 3 * s);
                        }
                        const float textY = at.y + previewHeight + 10 * s;
                        draw->AddText(ImVec2(at.x + 12 * s, textY), ImGui::GetColorU32(ImGuiCol_Text), viewNames[i]);
                        draw->AddText(ImVec2(at.x + 12 * s, textY + ImGui::GetTextLineHeightWithSpacing()), ImGui::GetColorU32(Dim), viewNotes[i]);
                    })) {
                    draft_.view = i == 1 ? "home" : "grid";
                }
                ImGui::PopID();
            }
        } else if (setupStep_ == 1) {
            title("Your games", "The folder with your game dumps: game folders and .zar archives, in it or in its subfolders. Nothing is copied, and .zar files stay as they are.");
            CountSetupGames();
            if (auto counted = setupCounter_.Take()) setupGameCount_ = *counted;
            // A large drop-zone-like card with the folder field inside.
            const ImVec2 at = ImGui::GetCursorScreenPos();
            const float cardHeight = 150.0f * s;
            auto* draw = ImGui::GetWindowDrawList();
            const ImVec2 end(at.x + width, at.y + cardHeight);
            draw->AddRectFilled(at, end, ImGui::GetColorU32(Card), 12 * s);
            // Dashed outline.
            const ImU32 dash = ImGui::GetColorU32(Fade(Dim, 0.45f));
            for (float x = at.x + 14 * s; x < end.x - 14 * s; x += 14 * s) {
                draw->AddLine(ImVec2(x, at.y), ImVec2(std::min(x + 7 * s, end.x - 14 * s), at.y), dash, 1.2f * s);
                draw->AddLine(ImVec2(x, end.y), ImVec2(std::min(x + 7 * s, end.x - 14 * s), end.y), dash, 1.2f * s);
            }
            for (float y = at.y + 14 * s; y < end.y - 14 * s; y += 14 * s) {
                draw->AddLine(ImVec2(at.x, y), ImVec2(at.x, std::min(y + 7 * s, end.y - 14 * s)), dash, 1.2f * s);
                draw->AddLine(ImVec2(end.x, y), ImVec2(end.x, std::min(y + 7 * s, end.y - 14 * s)), dash, 1.2f * s);
            }
            // A folder.
            const float fw = 46 * s;
            const float fh = 34 * s;
            const ImVec2 f(at.x + 24 * s, at.y + 24 * s);
            draw->AddRectFilled(f, ImVec2(f.x + fw * 0.45f, f.y + fh * 0.3f), ImGui::GetColorU32(AccentActive), 4 * s);
            draw->AddRectFilled(ImVec2(f.x, f.y + fh * 0.18f), ImVec2(f.x + fw, f.y + fh), ImGui::GetColorU32(Accent), 5 * s);
            ImGui::SetCursorScreenPos(ImVec2(f.x + fw + 16 * s, at.y + 22 * s));
            ImGui::BeginGroup();
            ImGui::TextUnformatted("Games folder");
            ImGui::TextColored(Dim, "%s", "For example D:\\Games\\PS5");
            ImGui::EndGroup();
            ImGui::SetCursorScreenPos(ImVec2(at.x + 24 * s, f.y + fh + 22 * s));
            const float browse = ImGui::CalcTextSize("Browse...").x + style.FramePadding.x * 2;
            ImGui::SetNextItemWidth(width - 48 * s - browse - style.ItemSpacing.x);
            ImGui::InputTextWithHint("##setup-games", "Paste a path or choose one", &draft_.gamesFolder);
            ImGui::SameLine();
            ImGui::BeginDisabled(Picker().IsOpen());
            if (PrimaryButton("Browse...")) Picker().Open(window_, &draft_.gamesFolder);
            ImGui::EndDisabled();
            ImGui::SetCursorScreenPos(ImVec2(at.x, end.y + 14 * s));
            // What's in it.
            if (draft_.gamesFolder.empty()) {
                ImGui::TextColored(Dim, "%s", "Choose a folder to go on.");
            } else if (setupCounter_.Running() || draft_.gamesFolder != setupCounted_) {
                ImGui::TextColored(Dim, "%s", "Looking for games...");
            } else if (setupGameCount_ && *setupGameCount_ > 0) {
                ImGui::TextColored(Green, "Found %d game%s", *setupGameCount_, *setupGameCount_ == 1 ? "" : "s");
            } else if (setupGameCount_ && *setupGameCount_ < 0) {
                ImGui::TextColored(Red, "%s", "That folder can't be read.");
            } else {
                ImGui::TextColored(Amber, "%s", "No games there yet. You can still go on and add them later.");
            }
            canGoOn = !draft_.gamesFolder.empty();
        } else if (setupStep_ == 2) {
            title("AnyPS5", "The relinker that converts games, and the libraries they run on. Get the official release, or point to a build of your own. More versions can be added later.");
            const bool busy = job_.Busy() || releasesLoader_.Running();
            canGoOn = !busy;  // a download started here finishes before the setup goes on
            // Where things stand, in one row.
            {
                const ImVec2 at = ImGui::GetCursorScreenPos();
                const float h = ImGui::GetFrameHeight() + 18 * s;
                auto* draw = ImGui::GetWindowDrawList();
                const ImVec4 tone = anyps5_.probe.runtime ? Green : busy ? Accent : Dim;
                draw->AddRectFilled(at, ImVec2(at.x + width, at.y + h), ImGui::GetColorU32(Fade(tone, 0.12f)), 10 * s);
                const ImVec2 c(at.x + 20 * s, at.y + h * 0.5f);
                draw->AddCircleFilled(c, 9 * s, ImGui::GetColorU32(tone));
                if (anyps5_.probe.runtime) DrawCheck(draw, c, 12 * s, IM_COL32_WHITE, 2.0f * s);
                ImGui::SetCursorScreenPos(ImVec2(at.x + 38 * s, at.y + (h - ImGui::GetTextLineHeight()) * 0.5f));
                if (anyps5_.probe.runtime) ImGui::TextColored(Green, "Ready: AnyPS5 %s", CurrentVersionLabel().c_str());
                else if (busy) ImGui::TextUnformatted(job_.Busy() ? job_.Title().c_str() : "Looking up the latest release...");
                else ImGui::TextColored(Dim, "%s", "Not set up yet. You can skip this and do it later.");
                if (job_.Busy()) {
                    const auto& control = job_.Control();
                    const float fraction = control.Total() == 0 ? 0.0f : static_cast<float>(control.Done()) / static_cast<float>(control.Total());
                    ImGui::SetCursorScreenPos(ImVec2(at.x + width * 0.55f, at.y + (h - ImGui::GetTextLineHeight()) * 0.5f));
                    ImGui::ProgressBar(fraction, ImVec2(width * 0.42f, ImGui::GetTextLineHeight()));
                }
                ImGui::SetCursorScreenPos(ImVec2(at.x, at.y + h + 14 * s));
            }
            // Two ways, as cards.
            const float gap = 12.0f * s;
            const float cardWidth = (width - gap) / 2.0f;
            const struct {
                const char* title;
                const char* text;
                const char* button;
                bool primary;
            } ways[2] = {{"Official release", "Downloads the newest AnyPS5 from github.com/boykopovar/AnyPS5 and checks it.", "Download", true},
                         {"My own build", "An AnyPS5 folder you built or unpacked, with the relinker and its libs folder.", "Choose a folder...", false}};
            float textHeight = 0.0f;
            for (const auto& way : ways) textHeight = std::max(textHeight, ImGui::CalcTextSize(way.text, nullptr, false, cardWidth - 32 * s).y);
            const float cardHeight = 14 * s + ImGui::GetFrameHeight() + 2 * s + textHeight + 14 * s + ImGui::GetFrameHeight() * 1.2f + 14 * s;
            for (int i = 0; i < 2; ++i) {
                if (i > 0) ImGui::SameLine(0, gap);
                const ImVec2 at = ImGui::GetCursorScreenPos();
                auto* draw = ImGui::GetWindowDrawList();
                draw->AddRectFilled(at, ImVec2(at.x + cardWidth, at.y + cardHeight), ImGui::GetColorU32(Card), 12 * s);
                draw->AddRect(at, ImVec2(at.x + cardWidth, at.y + cardHeight), ImGui::GetColorU32(ways[i].primary ? Fade(Accent, 0.6f) : style.Colors[ImGuiCol_Border]),
                              12 * s, 0, 1.0f * s);
                ImGui::BeginGroup();
                ImGui::SetCursorScreenPos(ImVec2(at.x + 16 * s, at.y + 14 * s));
                ImGui::TextUnformatted(ways[i].title);
                if (ways[i].primary) {
                    ImGui::SameLine();
                    Pill("Recommended", Accent);
                }
                ImGui::SetCursorScreenPos(ImVec2(at.x + 16 * s, ImGui::GetCursorScreenPos().y + 2 * s));
                ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + cardWidth - 32 * s);
                ImGui::TextColored(Dim, "%s", ways[i].text);
                ImGui::PopTextWrapPos();
                ImGui::SetCursorScreenPos(ImVec2(at.x + 16 * s, at.y + cardHeight - ImGui::GetFrameHeight() * 1.2f - 14 * s));
                ImGui::BeginDisabled(busy || Picker().IsOpen());
                ImGui::PushID(i);
                const ImVec2 buttonSize(cardWidth - 32 * s, ImGui::GetFrameHeight() * 1.2f);
                if (i == 0 ? PrimaryButton(ways[i].button, buttonSize) : ImGui::Button(ways[i].button, buttonSize)) {
                    if (i == 0) LoadReleases(OfficialRepository, true);
                    else Picker().Open(window_, &localPick_);
                }
                ImGui::PopID();
                ImGui::EndDisabled();
                ImGui::SetCursorScreenPos(at);
                ImGui::Dummy(ImVec2(cardWidth, cardHeight));
                ImGui::EndGroup();
            }
            if (hostIntel_) {
                // Only for Intel: an AMD PC needs nothing here.
                ImGui::Dummy(ImVec2(0, 8 * s));
                const ImVec2 at = ImGui::GetCursorScreenPos();
                auto* draw = ImGui::GetWindowDrawList();
                const std::string head = cpuName_.empty() ? std::string("Intel CPU") : cpuName_;
                const char* text = "Games will be converted with the Intel flag (--to-intel), which Intel CPUs need. You can turn it off in Settings under Converting games.";
                const float wrap = width - 56 * s;
                const float h = ImGui::GetTextLineHeightWithSpacing() + ImGui::CalcTextSize(text, nullptr, false, wrap).y + 24 * s;
                draw->AddRectFilled(at, ImVec2(at.x + width, at.y + h), ImGui::GetColorU32(Fade(Accent, 0.12f)), 10 * s);
                draw->AddRectFilled(at, ImVec2(at.x + 4 * s, at.y + h), ImGui::GetColorU32(Accent), 2 * s);
                const ImVec2 c(at.x + 24 * s, at.y + 12 * s + ImGui::GetTextLineHeight() * 0.5f);
                draw->AddCircleFilled(c, 9 * s, ImGui::GetColorU32(Accent));
                draw->AddText(ImVec2(c.x - ImGui::CalcTextSize("i").x * 0.5f, c.y - ImGui::GetTextLineHeight() * 0.5f), IM_COL32_WHITE, "i");
                ImGui::SetCursorScreenPos(ImVec2(at.x + 42 * s, at.y + 12 * s));
                ImGui::BeginGroup();
                ImGui::TextUnformatted(head.c_str());
                ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + wrap);
                ImGui::TextColored(Dim, "%s", text);
                ImGui::PopTextWrapPos();
                ImGui::EndGroup();
                ImGui::SetCursorScreenPos(ImVec2(at.x, at.y + h));
                ImGui::Dummy(ImVec2(width, 0));
            }
        } else {
            title("Your profile", "A profile keeps its own saves, trophies, controls, screenshots and console language. Add more later from the profile button in the top bar.");
            const float avatar = 72.0f * s;
            DrawProfileAvatar(setupProfile_.empty() ? std::string("?") : setupProfile_, ImGui::GetCursorScreenPos(), avatar);
            ImGui::Dummy(ImVec2(avatar, avatar));
            ImGui::SameLine(0, 18.0f * s);
            ImGui::BeginGroup();
            ImGui::Dummy(ImVec2(0, 6 * s));
            ImGui::TextUnformatted("Name");
            ImGui::SetNextItemWidth(std::min(width - avatar - 18 * s, 320.0f * s));
            if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
            ImGui::InputText("##setup-profile", &setupProfile_);
            ImGui::EndGroup();
            canGoOn = IsValidProfileName(setupProfile_);
            if (!canGoOn) ImGui::TextColored(Red, "%s", "Use a name that can be a folder name (no \\ / : * ? \" < > |)");
            // What was chosen, to check at a glance.
            ImGui::Dummy(ImVec2(0, 14 * s));
            label("Ready to go");
            const ImVec2 at = ImGui::GetCursorScreenPos();
            const float row = ImGui::GetTextLineHeightWithSpacing() + 10 * s;
            const std::string games = draft_.gamesFolder.empty() ? std::string("not set") : draft_.gamesFolder;
            const std::string found = setupGameCount_ && *setupGameCount_ >= 0 && setupCounted_ == draft_.gamesFolder
                                          ? " (" + std::to_string(*setupGameCount_) + (*setupGameCount_ == 1 ? " game)" : " games)")
                                          : std::string();
            const std::string version = anyps5_.probe.runtime ? CurrentVersionLabel() : std::string("not yet, set it up later under AnyPS5 versions");
            const std::string look = std::string(draft_.theme == "dark" ? "Dark" : draft_.theme == "light" ? "Light" : "Follows Windows") + ", " +
                                     (draft_.view == "home" ? "home row" : "grid");
            const std::pair<const char*, std::string> rows[] = {{"Games", games + found}, {"AnyPS5", version}, {"Look", look}};
            auto* draw = ImGui::GetWindowDrawList();
            draw->AddRectFilled(at, ImVec2(at.x + width, at.y + row * 3 + 12 * s), ImGui::GetColorU32(Card), 12 * s);
            for (int i = 0; i < 3; ++i) {
                const float y = at.y + 6 * s + static_cast<float>(i) * row;
                const bool ok = i != 1 || anyps5_.probe.runtime;
                const ImVec2 c(at.x + 22 * s, y + row * 0.5f);
                draw->AddCircleFilled(c, 8 * s, ImGui::GetColorU32(ok ? Fade(Green, 0.9f) : Fade(Amber, 0.9f)));
                if (ok) DrawCheck(draw, c, 11 * s, IM_COL32_WHITE, 1.8f * s);
                else draw->AddText(ImVec2(c.x - ImGui::CalcTextSize("!").x * 0.5f, c.y - ImGui::GetTextLineHeight() * 0.5f), IM_COL32_WHITE, "!");
                const float textY = y + (row - ImGui::GetTextLineHeight()) * 0.5f;
                draw->AddText(ImVec2(at.x + 40 * s, textY), ImGui::GetColorU32(Dim), rows[i].first);
                const float valueX = at.x + 120 * s;
                draw->PushClipRect(ImVec2(valueX, y), ImVec2(at.x + width - 12 * s, y + row), true);
                draw->AddText(ImVec2(valueX, textY), ImGui::GetColorU32(ImGuiCol_Text), rows[i].second.c_str());
                draw->PopClipRect();
            }
            ImGui::Dummy(ImVec2(width, row * 3 + 12 * s));
        }
        setupCanGoOn_ = canGoOn;
    }

    void DrawSetup() {
        if (openSetup_) {
            ImGui::OpenPopup("Welcome");
            openSetup_ = false;
        }
        FollowWindow(0.76f, 0.86f);
        if (!BeginDialog("Welcome")) return;
        const auto& style = ImGui::GetStyle();
        const float s = scale_;
        const float side = std::clamp(ImGui::GetContentRegionAvail().x * 0.30f, 220.0f * s, 300.0f * s);
        DrawSetupSidebar(side);
        ImGui::SameLine(0, 26.0f * s);

        ImGui::BeginGroup();
        const float width = ImGui::GetContentRegionAvail().x;
        const float buttonHeight = ImGui::GetFrameHeight() * 1.35f;
        const float footer = buttonHeight + 10.0f * s + ImGui::GetTextLineHeight() + style.ItemSpacing.y * 4 + 2.0f;
        ImGui::BeginChild("setup-body", ImVec2(width, -footer), ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
        DrawSetupStep(ImGui::GetContentRegionAvail().x - style.ScrollbarSize * 0.5f);
        ImGui::EndChild();

        // A thin progress bar over the buttons.
        {
            const ImVec2 at = ImGui::GetCursorScreenPos();
            auto* draw = ImGui::GetWindowDrawList();
            const float h = 4.0f * s;
            draw->AddRectFilled(at, ImVec2(at.x + width, at.y + h), ImGui::GetColorU32(Fade(Dim, 0.2f)), h);
            const float done = static_cast<float>(setupStep_ + 1) / static_cast<float>(SetupSteps);
            draw->AddRectFilled(at, ImVec2(at.x + width * done, at.y + h), ImGui::GetColorU32(Accent), h);
            ImGui::Dummy(ImVec2(width, h + 6 * s));
        }
        if (!setupError_.empty()) ImGui::TextColored(Red, "%s", Elide(setupError_, width).c_str());
        else if (setupStep_ == 2 && (job_.Busy() || releasesLoader_.Running())) ImGui::TextColored(Dim, "%s", "Downloading AnyPS5; Continue unlocks when it's done.");
        else ImGui::TextColored(Dim, "Step %d of %d", setupStep_ + 1, SetupSteps);
        const float buttonWidth = ImGui::GetFontSize() * 7.0f;
        if (setupStep_ > 0) {
            if (ImGui::Button("Back", ImVec2(buttonWidth, buttonHeight))) --setupStep_;
            ImGui::SameLine();
        }
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, width - buttonWidth - (setupStep_ > 0 ? buttonWidth + style.ItemSpacing.x : 0.0f)));
        ImGui::BeginDisabled(!setupCanGoOn_ || Picker().IsOpen());
        const bool last = setupStep_ == SetupSteps - 1;
        const bool enter = !last && setupCanGoOn_ && !ImGui::IsAnyItemActive() && ImGui::IsKeyPressed(ImGuiKey_Enter, false);
        if (PrimaryButton(last ? "Start playing" : "Continue", ImVec2(buttonWidth, buttonHeight)) || enter) {
            if (!last) {
                ++setupStep_;
            } else {
                try {
                    draft_.anyps5Folder = settings_.anyps5Folder;  // chosen in step 3 meanwhile
                    draft_.anyps5Version = settings_.anyps5Version;
                    settings_ = draft_;
                    ApplySettings();
                    if (setupProfile_ != layout_.profile) {
                        const bool fresh = profiles_.size() <= 1;
                        if (fresh && std::find(profiles_.begin(), profiles_.end(), layout_.profile) != profiles_.end()) {
                            RenameProfile(layout_, setupProfile_);
                        }
                        layout_.profile = setupProfile_;
                    }
                    CreateProfile(layout_);
                    settings_.profile = layout_.profile;
                    SaveSettings(settingsFile_, settings_);
                    profiles_ = ListProfiles(layout_.userdata);
                    LoadCurrentProfile();
                    ApplyCurrentTheme();
                    StartScan();
                    ImGui::CloseCurrentPopup();
                } catch (const std::exception& failure) {
                    setupError_ = ReadableText(failure.what());
                }
            }
        }
        ImGui::EndDisabled();
        ImGui::EndGroup();
        ImGui::EndPopup();
    }

    // ---- home view: a row of tiles, the selected one large ---------------------------

    void DrawHome() {
        const auto& style = ImGui::GetStyle();
        const float height = -FooterHeight();
        ImGui::BeginChild("home", ImVec2(0, height), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar);
        const ImVec2 origin = ImGui::GetWindowPos();
        const ImVec2 size = ImGui::GetWindowSize();
        auto* draw = ImGui::GetWindowDrawList();
        const auto* selected = SelectedGame();
        if (selected == nullptr && !result_.games.empty()) {
            selected_ = ToUtf8(result_.games.front().location);
            selected = SelectedGame();
        }
        // The selected game's artwork fills the view, fading into the window colour.
        if (selected != nullptr) {
            const auto art = art_.find(GameKey(*selected));
            if (art != art_.end() && !art->second.empty()) {
                if (auto* texture = icons_->Get(selected->location / "pic0.png", art->second)) {
                    float w = 0;
                    float h = 0;
                    SDL_GetTextureSize(texture, &w, &h);
                    const float aspect = size.x / size.y;
                    ImVec2 uv0(0, 0);
                    ImVec2 uv1(1, 1);
                    if (w / h > aspect) {
                        const float cut = (1.0f - aspect * h / w) * 0.5f;
                        uv0.x = cut;
                        uv1.x = 1.0f - cut;
                    } else {
                        const float cut = (1.0f - w / (aspect * h)) * 0.5f;
                        uv0.y = cut;
                        uv1.y = 1.0f - cut;
                    }
                    const bool dark = WantDark(settings_.theme);
                    draw->AddImageRounded(static_cast<ImTextureID>(reinterpret_cast<std::intptr_t>(texture)), origin, ImVec2(origin.x + size.x, origin.y + size.y), uv0, uv1,
                                          ImGui::GetColorU32(ImVec4(1, 1, 1, dark ? 0.55f : 0.45f)), 12.0f * scale_);
                    // Darker to the left and toward the bottom, where the text is; no hard edges.
                    const ImU32 clear = ImGui::GetColorU32(Fade(WindowColor, 0.0f));
                    const ImU32 half = ImGui::GetColorU32(Fade(WindowColor, 0.55f));
                    const ImU32 solid = ImGui::GetColorU32(Fade(WindowColor, 0.92f));
                    const ImVec2 end(origin.x + size.x, origin.y + size.y);
                    draw->AddRectFilledMultiColor(origin, end, solid, clear, clear, solid);
                    draw->AddRectFilledMultiColor(origin, end, clear, clear, half, half);
                }
            }
        }

        // The row: small tiles, the selected one larger with its name under it.
        const float small = std::clamp(size.y * 0.16f, 64.0f * scale_, 120.0f * scale_);
        const float large = small * 1.55f;
        const float gap = 14.0f * scale_;
        const float halo = 14.0f * scale_;  // room for the selected game's glow, so the row does not cut it off
        ImGui::SetCursorPos(ImVec2(style.WindowPadding.x + 8.0f * scale_ - halo, 16.0f * scale_ - halo));
        ImGui::BeginChild("home-row", ImVec2(0, large + ImGui::GetTextLineHeightWithSpacing() * 1.5f + halo), ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar);
        ImGui::SetCursorPos(ImVec2(halo, halo));
        int index = 0;
        for (const auto& game : result_.games) {
            if (!ContainsFolded(game.metadata.name, search_) && !ContainsFolded(game.metadata.titleId, search_)) continue;
            const auto key = ToUtf8(game.location);
            const bool isSelected = key == selected_;
            const float tile = isSelected ? large : small;
            if (index++ > 0) ImGui::SameLine(0, gap);
            ImGui::PushID(key.c_str());
            const ImVec2 at = ImGui::GetCursorScreenPos();
            ImGui::InvisibleButton("tile", ImVec2(tile, large));
            if (ImGui::IsItemClicked()) selected_ = key;
            if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) ActivateGame(game);
            if (ImGui::IsItemFocused() && ImGui::GetIO().NavVisible) {
                if (selected_ != key) selected_ = key;
                if (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceDown, false) || ImGui::IsKeyPressed(ImGuiKey_Enter, false)) ActivateGame(game);
                if (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceUp, false)) ImGui::OpenPopup("actions");
            }
            if (isSelected && ImGui::IsWindowAppearing()) ImGui::SetScrollHereX(0.3f);
            if (ImGui::BeginPopupContextItem("actions")) {
                DrawActionItems(game);
                ImGui::EndPopup();
            }
            auto* rowDraw = ImGui::GetWindowDrawList();
            const ImVec2 top(at.x, at.y + (large - tile));
            const float rounding = (isSelected ? 14.0f : 10.0f) * scale_;
            if (isSelected) {
                // A soft glow marks the selected game instead of a ring, which read as a stray line around the icon.
                for (int ring = 6; ring >= 1; --ring) {
                    const float grow = static_cast<float>(ring) * 2.0f * scale_;
                    rowDraw->AddRectFilled(ImVec2(top.x - grow, top.y - grow), ImVec2(top.x + tile + grow, top.y + tile + grow), ImGui::GetColorU32(Fade(Accent, 0.07f)), rounding + grow);
                }
            }
            if (auto* texture = icons_->Get(game.location, game.icon)) {
                rowDraw->AddImageRounded(static_cast<ImTextureID>(reinterpret_cast<std::intptr_t>(texture)), top, ImVec2(top.x + tile, top.y + tile), ImVec2(0, 0), ImVec2(1, 1),
                                         IM_COL32_WHITE, rounding);
            } else {
                rowDraw->AddRectFilled(top, ImVec2(top.x + tile, top.y + tile), ImGui::GetColorU32(Placeholder), rounding);
            }
            if (isSelected) DrawPlatformTag(rowDraw, game, ImVec2(top.x + tile - 8.0f * scale_, top.y + 8.0f * scale_), ImGui::GetTextLineHeight() + 6.0f * scale_);
            const auto badge = BadgeFor(game);
            if (badge.text != nullptr && !isSelected) rowDraw->AddCircleFilled(ImVec2(top.x + tile - 9 * scale_, top.y + 9 * scale_), 5 * scale_, ImGui::GetColorU32(badge.color));
            ImGui::PopID();
        }
        if (index == 0) ImGui::TextColored(Dim, "%s", scanned_ ? "No games found. Choose the games folder in Settings." : "Looking for games...");
        ImGui::EndChild();

        // The selected game: name, facts and what to do.
        if (selected != nullptr) {
            ImGui::SetCursorPosX(style.WindowPadding.x + 16.0f * scale_);
            ImGui::BeginGroup();
            ImGui::PushFont(nullptr, style.FontSizeBase * 2.4f);
            ImGui::TextUnformatted(selected->metadata.name.c_str());
            ImGui::PopFont();
            bool loading = false;
            const auto* set = TrophiesOf(*selected, loading);
            std::string facts = selected->metadata.titleId + "  \xC2\xB7  v" + selected->metadata.contentVersion + "  \xC2\xB7  " + std::string(ToString(selected->format));
            const auto region = RegionOf(selected->metadata.contentId);
            if (!region.empty()) facts += "  \xC2\xB7  " + region;
            ImGui::TextColored(Dim, "%s", facts.c_str());
            const auto badge = BadgeFor(*selected);
            if (badge.text != nullptr) Pill(badge.text, badge.color);
            ImGui::Spacing();
            ImGui::Spacing();
            const float button = ImGui::GetFrameHeight() * 1.5f;
            const auto* manifest = InstallFor(*selected);
            if (IsRunning(*selected)) {
                if (PrimaryButton("Stop", ImVec2(ImGui::GetFontSize() * 9, button))) session_->Stop();
            } else if (manifest != nullptr) {
                const auto reason = PlayBlocker(*selected);
                ImGui::BeginDisabled(!reason.empty());
                if (PrimaryButton("Play", ImVec2(ImGui::GetFontSize() * 9, button))) Play(*selected);
                ImGui::EndDisabled();
                if (!reason.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", reason.c_str());
            } else {
                const auto reason = InstallBlocker(*selected);
                ImGui::BeginDisabled(!reason.empty());
                if (PrimaryButton("Install", ImVec2(ImGui::GetFontSize() * 9, button))) StartInstall(*selected);
                ImGui::EndDisabled();
                if (!reason.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", reason.c_str());
            }
            ImGui::SameLine();
            const std::string trophies = set == nullptr ? std::string("Trophies")
                                         : set->encrypted ? std::to_string(set->lockedCount)
                                                          : std::to_string(set->Count().earned) + "/" + std::to_string(set->Count().total);
            const ImVec2 cupAt = ImGui::GetCursorScreenPos();
            if (ImGui::Button(("     " + trophies + "##home-trophies").c_str(), ImVec2(0, button))) OpenTrophies();
            {
                // A cup inside the button, before the count.
                const float cup = ImGui::GetFontSize() * 1.1f;
                DrawTrophyCup(ImGui::GetWindowDrawList(), ImVec2(cupAt.x + style.FramePadding.x, cupAt.y + (button - cup) * 0.5f), TrophyGrade::Gold, cup);
            }
            ImGui::SameLine();
            if (ImGui::Button("Controls", ImVec2(0, button))) OpenControls(*selected);
            ImGui::SameLine();
            if (ImGui::Button("Game settings", ImVec2(0, button))) OpenGameSettings(*selected);
            ImGui::SameLine();
            if (ImGui::Button("Screenshots", ImVec2(0, button))) OpenScreenshots(*selected);
            ImGui::SameLine();
            if (ImGui::Button("...", ImVec2(button, button))) ImGui::OpenPopup("home-actions");
            if (ImGui::BeginPopup("home-actions")) {
                DrawActionItems(*selected);
                ImGui::EndPopup();
            }
            ImGui::Spacing();
            ImGui::PushTextWrapPos(size.x * 0.6f);
            DrawStatus(*selected);
            ImGui::PopTextWrapPos();
            ImGui::EndGroup();
        }
        ImGui::EndChild();
    }

    // ---- links, requests from a second launcher, self-update, issues -----------------

public:
    bool QuitRequested() const { return quitRequested_; }

    void SetStartRequest(std::optional<LaunchRequest> request) { pendingRequest_ = std::move(request); }

    // Started by an update: say so once.
    void SetJustUpdated() {
#ifdef LAUNCHER_VERSION
        message_ = Message{Green, "Launcher updated", std::string("You're on AnyPS5 Launcher ") + LAUNCHER_VERSION + "."};
#endif
    }


private:
    std::filesystem::path RequestFile() const { return launcherFolder_ / "anyps5-launcher.request"; }

    // A link opened while the launcher runs reaches it through a small file the
    // second launcher writes (it then exits); checked twice a second.
    void HandleRequests() {
        const Uint64 now = SDL_GetTicks();
        if (now >= nextRequestCheck_) {
            nextRequestCheck_ = now + 500;
            std::error_code error;
            if (std::filesystem::exists(RequestFile(), error)) {
                std::ifstream stream(RequestFile(), std::ios::binary);
                std::string line;
                std::getline(stream, line);
                stream.close();
                std::filesystem::remove(RequestFile(), error);
                if (auto request = ParseRequest(line)) pendingRequest_ = request;
            }
        }
        if (!pendingRequest_ || !scanned_) return;
        const auto request = *pendingRequest_;
        pendingRequest_.reset();
        const GameEntry* found = nullptr;
        for (const auto& game : result_.games) {
            if (game.metadata.titleId != request.titleId) continue;
            if (found == nullptr || (InstallFor(*found) == nullptr && InstallFor(game) != nullptr)) found = &game;  // the installed copy first
        }
        if (found == nullptr) {
            message_ = Message{Amber, "Link", request.titleId + " isn't in the games folder"};
            return;
        }
        selected_ = ToUtf8(found->location);
        if (request.kind == LaunchRequest::Kind::Play && !session_) {
            if (const auto reason = PlayBlocker(*found); reason.empty()) Play(*found);
            else message_ = Message{Amber, "Can't play " + found->metadata.name, reason};
        }
    }

    void CopyLink(const GameEntry& game, LaunchRequest::Kind kind) {
        const auto link = LinkFor({kind, game.metadata.titleId});
        SDL_SetClipboardText(link.c_str());
        message_ = Message{Green, "Link copied", link + " (paste it in a shortcut, Steam, a browser...)"};
    }

    void CheckLauncherUpdate() {
        if (launcherReleases_.Running()) return;
        launcherChecked_ = false;
        launcherReleases_.Start([] {
            ReleaseList list;
            list.repository = LauncherRepository;
            try {
                list.releases = ParseReleases(HttpGetText(ReleasesUrl(LauncherRepository)));
            } catch (const std::exception& failure) {
                list.error = ReadableText(failure.what());
            }
            return list;
        });
    }

    std::filesystem::path LauncherExe() const {
#ifdef _WIN32
        wchar_t path[1024];
        win::ExePath(path, 1024);
        return std::filesystem::path(path);
#else
        return launcherFolder_ / "anyps5-launcher";
#endif
    }

    // Opens the update dialog; the download starts (or starts again) if it isn't there yet.
    void StartLauncherUpdate() {
        if (!launcherUpdate_) return;
        if (stagedLauncher_ && !stagedLauncher_->error.empty()) stagedLauncher_.reset();
        StageLauncher();
        openLauncherUpdate_ = true;
    }

    // Downloads and unpacks the update in the background, off the job queue, so it never
    // gets in the way of installs or games.
    void StageLauncher() {
#ifdef _WIN32
        if (!launcherUpdate_ || launcherStager_.Running() || updateApplied_) return;
        if (stagedLauncher_ && stagedLauncher_->tag == launcherUpdate_->release.tag) return;  // done, or failed: retried on click
        stageControl_ = std::make_shared<JobControl>();
        launcherStager_.Start([update = *launcherUpdate_, folder = launcherFolder_, control = stageControl_] {
            StagedLauncher staged;
            staged.tag = update.release.tag;
            try {
                staged.exe = StageLauncherUpdate(update, folder, HttpDownload, RunToFile, *control);
            } catch (const std::exception& failure) {
                staged.error = ReadableText(failure.what());
            }
            return staged;
        });
#endif
    }

    bool LauncherUpdateReady() const {
        return launcherUpdate_ && stagedLauncher_ && stagedLauncher_->tag == launcherUpdate_->release.tag && !stagedLauncher_->exe.empty();
    }

    int StagePercent() const {
        if (!stageControl_ || stageControl_->Total() == 0) return 0;
        return static_cast<int>(std::min<std::uint64_t>(100, stageControl_->Done() * 100 / stageControl_->Total()));
    }

    // Puts the downloaded exe in place of this one; with restart, starts it where this window is.
    void ApplyLauncherUpdate(bool restart) {
#ifdef _WIN32
        if (updateApplied_ || !LauncherUpdateReady()) return;
        try {
            SwapLauncherExe(LauncherExe(), stagedLauncher_->exe);
            updateApplied_ = true;
        } catch (const std::exception& failure) {
            if (restart) message_ = Message{Red, "Can't update the launcher", ReadableText(failure.what())};
            return;
        }
        if (restart) RestartLauncher();
#else
        (void)restart;
#endif
    }

    // Starts the (new) launcher in this window's place, on the same game, and closes this one.
    void RestartLauncher() {
        std::vector<std::string> arguments{ToUtf8(LauncherExe()), "--updated"};
        WindowPlacement placement;
        SDL_GetWindowPosition(window_, &placement.x, &placement.y);
        SDL_GetWindowSize(window_, &placement.width, &placement.height);
        placement.maximized = (SDL_GetWindowFlags(window_) & SDL_WINDOW_MAXIMIZED) != 0;
        arguments.push_back(FormatWindowArgument(placement));
        for (const auto& game : result_.games) {
            if (ToUtf8(game.location) == selected_ && !game.metadata.titleId.empty()) {
                arguments.push_back("--game");
                arguments.push_back(game.metadata.titleId);
            }
        }
        std::vector<const char*> pointers;
        for (const auto& argument : arguments) pointers.push_back(argument.c_str());
        pointers.push_back(nullptr);
        SDL_PropertiesID properties = SDL_CreateProperties();
        SDL_SetPointerProperty(properties, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, const_cast<char**>(pointers.data()));
        SDL_SetBooleanProperty(properties, SDL_PROP_PROCESS_CREATE_BACKGROUND_BOOLEAN, true);
        SDL_Process* process = SDL_CreateProcessWithProperties(properties);
        SDL_DestroyProperties(properties);
        if (process == nullptr) {
            message_ = Message{Amber, "Updated", "Start the launcher again to use the new version."};
            return;
        }
        SDL_DestroyProcess(process);  // it runs on by itself
        quitRequested_ = true;
    }

    void DrawLauncherUpdate() {
        if (openLauncherUpdate_) {
            ImGui::OpenPopup("Launcher update");
            openLauncherUpdate_ = false;
        }
        FollowWindow(0.50f, 0.62f);
        if (!BeginDialog("Launcher update")) return;
        if (!launcherUpdate_) {
            ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
            return;
        }
        const auto& release = launcherUpdate_->release;
        const auto& style = ImGui::GetStyle();
        std::string note = "You have ";
#ifdef LAUNCHER_VERSION
        note += std::string("v") + LAUNCHER_VERSION;
#endif
        if (release.publishedAt.size() >= 10) note += ". Released " + release.publishedAt.substr(0, 10);
        note += ".";
        bool close = DialogHeader(("AnyPS5 Launcher " + release.tag).c_str(), note.c_str());
        const float footer = ImGui::GetFrameHeight() * 1.3f + style.ItemSpacing.y * 3 + ImGui::GetTextLineHeightWithSpacing();
        ImGui::BeginChild("update-notes", ImVec2(0, -footer), ImGuiChildFlags_Borders);
        const auto lines = ReleaseNoteLines(release.notes);
        ImGui::PushTextWrapPos(0.0f);
        if (lines.empty()) ImGui::TextColored(Dim, "%s", "No notes for this release.");
        for (const auto& line : lines) {
            if (line.text.empty()) {
                ImGui::Spacing();
            } else if (line.heading) {
                // "New", "Fixed": larger and in the accent colour, so the groups stand out.
                ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.15f);
                ImGui::TextColored(Accent, "%s", line.text.c_str());
                ImGui::PopFont();
            } else {
                ImGui::TextUnformatted(line.text.c_str());
            }
        }
        ImGui::PopTextWrapPos();
        ImGui::Spacing();
        if (ImGui::SmallButton("All changes on GitHub")) {
            SDL_OpenURL(("https://github.com/" + std::string(LauncherRepository) + "/releases/tag/" + release.tag).c_str());
        }
        ImGui::EndChild();

        // What happens next, in one line.
        if (LauncherUpdateReady()) {
            ImGui::TextColored(session_ ? Amber : Green, "%s",
                               session_ ? "Ready. Restart after the game closes, or it goes in place when you close the launcher." : "Ready. Restarting takes a second; you come back to the same game.");
        } else if (launcherStager_.Running()) {
            const float fraction = static_cast<float>(StagePercent()) / 100.0f;
            ImGui::ProgressBar(fraction, ImVec2(-FLT_MIN, ImGui::GetTextLineHeight()), "Downloading...");
        } else if (stagedLauncher_ && !stagedLauncher_->error.empty()) {
            ImGui::TextColored(Red, "%s", Elide("Download failed: " + stagedLauncher_->error, ImGui::GetContentRegionAvail().x).c_str());
        } else {
            ImGui::TextColored(Dim, "%s", "Getting it ready...");
        }
        ImGui::Spacing();
        const float buttonWidth = ImGui::GetFontSize() * 7.5f;
        const float buttonHeight = ImGui::GetFrameHeight() * 1.3f;
        if (stagedLauncher_ && !stagedLauncher_->error.empty() && !launcherStager_.Running()) {
            if (ImGui::Button("Try again", ImVec2(buttonWidth, buttonHeight))) {
                stagedLauncher_.reset();
                StageLauncher();
            }
            ImGui::SameLine();
        }
        ImGui::SameLine(ImGui::GetContentRegionMax().x - buttonWidth * 2 - style.ItemSpacing.x);
        if (ImGui::Button("Later", ImVec2(buttonWidth, buttonHeight))) close = true;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Keep going; the update goes in place when you close the launcher.");
        ImGui::SameLine();
        ImGui::BeginDisabled(!LauncherUpdateReady() || session_ != nullptr || job_.Busy());
        if (PrimaryButton("Restart now", ImVec2(buttonWidth, buttonHeight))) {
            ApplyLauncherUpdate(true);
            close = true;
        }
        ImGui::EndDisabled();
        if (close) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    static std::string UrlEncode(std::string_view text) {
        std::string out;
        for (const unsigned char c : text) {
            if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
                out += static_cast<char>(c);
            } else {
                char hex[4];
                std::snprintf(hex, sizeof(hex), "%%%02X", c);
                out += hex;
            }
        }
        return out;
    }

    // Opens a new issue on upstream AnyPS5 with the run's facts filled in; you read it and submit it.
    void OpenIssue(const GameEntry& game) {
        const auto& serial = game.metadata.titleId;
        const auto found = diagnoses_.find(serial);
        const std::string headline = found != diagnoses_.end() ? found->second.headline : "a problem";
        const std::string title = game.metadata.name + " (" + serial + ") v" + game.metadata.contentVersion + ": " + headline;
        std::string body = "**What happened**\n\n" + headline + "\n";
        if (found != diagnoses_.end() && !found->second.detail.empty()) body += "\n```\n" + found->second.detail + "\n```\n";
        body += "\n**Game**: " + game.metadata.name + " (" + serial + ") v" + game.metadata.contentVersion + ", " + std::string(ToString(game.format)) + "\n";
        body += "**AnyPS5**: " + VersionLabelFor(serial) + (StateFor(serial).version.empty() ? std::string() : " (relinker " + StateFor(serial).version + ")") + "\n";
        body += "**System**: " + SystemDescription() + "\n";
        const auto env = GameSettingsEnvironment(LoadGameSettings(layout_.GameSettingsFile(serial)));
        if (!env.empty()) {
            body += "**Settings**:";
            for (const auto& [name, value] : env) body += " `" + name + "=" + value + "`";
            body += "\n";
        }
        body += "\n_Sent from AnyPS5 Launcher. Attach the report file from \"Make a report\" (library\\reports) for the log._\n";
        const auto url = "https://github.com/boykopovar/AnyPS5/issues/new?title=" + UrlEncode(RedactPersonal(title)) + "&body=" + UrlEncode(RedactPersonal(body));
        SDL_OpenURL(url.c_str());
    }

    // ---- game settings, saves, screenshots, reports ---------------------------------

    void StartBackUpSaves(const std::string& serial) {
        if (session_ || job_.Busy()) return;
        const auto layout = layout_;
        job_.Start("Back up saves: " + serial, false, [layout, serial](JobControl& control) {
            const auto zip = BackUpSaves(layout, serial, control);
            return "Saves and trophies of profile " + layout.profile + " are in " + ToUtf8(zip);
        });
    }

    void StartRestoreSaves(const std::string& serial, const std::filesystem::path& backup) {
        if (session_ || job_.Busy()) return;
        const auto layout = layout_;
        job_.Start("Restore saves: " + serial, false, [layout, serial, backup](JobControl& control) {
            const auto kept = RestoreSaves(layout, serial, backup, control);
            return "Restored " + ToUtf8(backup.filename()) + (kept ? "; the saves you had are kept in " + ToUtf8(kept->filename()) : std::string());
        });
    }

    void ClearShaderCache(const std::string& serial) {
        try {
            std::filesystem::remove_all(layout_.ShaderCache(serial));  // rebuilt by AnyPS5; can be gigabytes, so not to the Recycle Bin
            message_ = Message{Green, "Shader cache cleared", serial + " builds it again at its next start."};
        } catch (const std::exception& failure) {
            message_ = Message{Red, "Can't clear the shader cache", ReadableText(failure.what())};
        }
    }

    static std::string FolderSize(const std::filesystem::path& folder) {
        std::uint64_t total = 0;
        std::error_code error;
        for (std::filesystem::recursive_directory_iterator it(folder, error), end; !error && it != end; it.increment(error)) {
            if (it->is_regular_file(error)) total += it->file_size(error);
        }
        return FormatBytes(total);
    }

    // One text file with what a bug report needs, in <library>\reports.
    void MakeReport(const GameEntry& game) {
        const auto& serial = game.metadata.titleId;
        std::string text = "AnyPS5 Launcher report\n======================\n\n";
        const auto now = std::time(nullptr);
        char stamp[32];
        std::strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", std::localtime(&now));
        text += "Created:   " + std::string(stamp) + "\n";
#ifdef LAUNCHER_VERSION
        text += "Launcher:  " + std::string(LAUNCHER_VERSION) + "\n";
#endif
        text += "System:    " + SystemDescription() + ", " + std::to_string(SDL_GetNumLogicalCPUCores()) + " CPU threads, " + std::to_string(SDL_GetSystemRAM()) + " MiB RAM\n";
        text += "AnyPS5:    " + CurrentVersionLabel() + (anyps5_.version.empty() ? std::string() : " (relinker " + anyps5_.version + ")") + "\n\n";
        text += "Game:      " + game.metadata.name + " (" + serial + ") v" + game.metadata.contentVersion + ", " + std::string(ToString(game.format)) + "\n";
        if (const auto* manifest = InstallFor(game)) {
            text += "Installed: " + manifest->installedAt + " with relinker " + manifest->relinkerVersion + "\n";
            std::string arguments;
            for (const auto& argument : manifest->arguments) arguments += " " + argument;
            text += "Relinker arguments:" + arguments + "\n";
        } else {
            text += "Installed: no\n";
        }
        text += "eboot.bin: " + std::string(ToString(game.executable.kind)) + "; modules: " + std::to_string(game.modules.elf) + " ELF, " + std::to_string(game.modules.self) + " SELF\n";
        text += "Profile language: " + LanguageLabel(profileSettings_.language) + "\n";
        const auto settings = LoadGameSettings(layout_.GameSettingsFile(serial));
        text += "Game settings:";
        const auto env = GameSettingsEnvironment(settings);
        if (env.empty()) text += " AnyPS5's defaults";
        for (const auto& [name, value] : env) text += " " + name + "=" + value;
        text += "\n";
        text += "Controllers:";
        int count = 0;
        if (SDL_JoystickID* pads = SDL_GetGamepads(&count)) {
            for (int i = 0; i < count; ++i) {
                const char* name = SDL_GetGamepadNameForID(pads[i]);
                text += std::string(i == 0 ? " " : ", ") + (name != nullptr ? name : "a gamepad");
            }
            SDL_free(pads);
        }
        if (count == 0) text += " none";
        text += "\n\n";
        if (const auto found = diagnoses_.find(serial); found != diagnoses_.end()) {
            text += "Last run:  " + found->second.headline + "\n";
            if (!found->second.detail.empty()) text += "           " + found->second.detail + "\n";
        }
        const auto log = LatestRunLog(layout_, serial);
        if (log) {
            auto reportFile = *log;
            reportFile.replace_extension(".report.json");
            std::ifstream report(reportFile, std::ios::binary);
            if (report) {
                text += "\n--- " + ToUtf8(reportFile.filename()) + " ---\n";
                text += std::string(std::istreambuf_iterator<char>(report), std::istreambuf_iterator<char>()) + "\n";
            }
        }
        for (const auto& file : {layout_.InputConfig(), layout_.GameInputConfig(serial)}) {
            std::ifstream input(file, std::ios::binary);
            if (!input) continue;
            text += "\n--- " + ToUtf8(file.filename()) + " ---\n" + std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()) + "\n";
        }
        if (log) {
            text += "\n--- end of " + ToUtf8(log->filename()) + " ---\n" + ReadLogTail(*log, 96 * 1024) + "\n";
        }
        try {
            const auto folder = layout_.library / "reports";
            std::filesystem::create_directories(folder);
            std::string name = serial + "-" + std::string(stamp) + ".txt";
            std::replace(name.begin(), name.end(), ':', '-');
            std::replace(name.begin(), name.end(), ' ', '_');
            const auto file = folder / FromUtf8(name);
            WriteFileAtomically(file, RedactPersonal(text));
            message_ = Message{Green, "Report saved", ToUtf8(file) + " (read it before you share it)"};
            OpenPath(file);
        } catch (const std::exception& failure) {
            message_ = Message{Red, "Can't write the report", ReadableText(failure.what())};
        }
    }

    // Asks for the game settings' disk facts again; StartGameFacts reads them off the UI thread.
    void RefreshGameFacts() {
        factsWanted_ = true;
        factsReady_ = false;
        StartGameFacts();
    }

    void StartGameFacts() {
        if (!factsWanted_ || settingsSerial_.empty() || factsLoader_.Running()) return;
        factsWanted_ = false;
        factsLoader_.Start([layout = layout_, serial = settingsSerial_] {
            GameFacts facts;
            facts.serial = serial;
            try {
                const auto cache = layout.ShaderCache(serial);
                if (PathExists(cache)) facts.cacheSize = FolderSize(cache);
                facts.hasSaves = HasSaves(layout, serial);
                facts.backups = ListSaveBackups(layout, serial);
            } catch (const std::exception&) {
            }
            return facts;
        });
    }

    void OpenGameSettings(const GameEntry& game) {
        settingsSerial_ = game.metadata.titleId;
        settingsName_ = game.metadata.name;
        gameSettings_ = LoadGameSettings(layout_.GameSettingsFile(settingsSerial_));
        gamePin_ = versions_.pins.count(settingsSerial_) ? versions_.pins.at(settingsSerial_) : std::string();
        cacheSize_.clear();
        hasSaves_ = false;
        backups_.clear();
        factsReady_ = false;
        RefreshGameFacts();
        gameSettingsError_.clear();
        openGameSettings_ = true;
    }

    // A labelled row of the game settings: the label and its note on the left, the control on the right.
    template <typename Control>
    void SettingRow(const char* label, const char* note, Control control) {
        ImGui::PushID(label);
        const float split = ImGui::GetContentRegionAvail().x * 0.42f;
        const float top = ImGui::GetCursorPosY();
        ImGui::BeginGroup();
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + split - ImGui::GetFontSize());
        ImGui::TextUnformatted(label);
        if (note != nullptr && note[0] != 0) ImGui::TextColored(Dim, "%s", note);
        ImGui::PopTextWrapPos();
        ImGui::EndGroup();
        const float bottom = ImGui::GetCursorPosY();
        ImGui::SetCursorPos(ImVec2(ImGui::GetCursorPosX() + split, top));
        ImGui::BeginGroup();
        ImGui::SetNextItemWidth(-FLT_MIN);
        control();
        ImGui::EndGroup();
        ImGui::SetCursorPosY(std::max(bottom, ImGui::GetCursorPosY()));
        ImGui::Spacing();
        ImGui::PopID();
    }

    // A choice from a few values, stored as AnyPS5 wants it.
    static void ChoiceCombo(const char* id, std::string& value, std::initializer_list<std::pair<const char*, const char*>> choices) {
        const char* shown = choices.begin()->second;
        for (const auto& [stored, label] : choices) {
            if (value == stored) shown = label;
        }
        if (ImGui::BeginCombo(id, shown)) {
            for (const auto& [stored, label] : choices) {
                if (ImGui::Selectable(label, value == stored)) value = stored;
            }
            ImGui::EndCombo();
        }
    }

    void DrawGameSettings() {
        if (openGameSettings_) {
            ImGui::OpenPopup("Game settings");
            openGameSettings_ = false;
        }
        FollowWindow(0.70f, 0.88f);
        if (!BeginDialog("Game settings")) return;
        const auto& style = ImGui::GetStyle();
        auto& g = gameSettings_;
        const std::string title = "Game settings: " + settingsName_;
        const std::string note = "Applied at each start of " + settingsSerial_ + " in profile " + layout_.profile + ". Unchanged settings use AnyPS5's defaults.";
        bool close = DialogHeader(title.c_str(), note.c_str());
        const float footer = ImGui::GetFrameHeight() * 1.3f + style.ItemSpacing.y * 3 + ImGui::GetTextLineHeightWithSpacing();
        ImGui::BeginChild("game-settings-body", ImVec2(0, -footer));

        Section("display", "Display", nullptr, [&] {
            SettingRow("Screen", "F11 switches while playing.", [&] {
                int screen = g.screen.empty() ? 0 : 1;
                const char* labels[] = {"Window", "Full screen"};
                if (Segmented("screen", labels, 2, screen)) g.screen = screen == 0 ? "" : "borderless";
            });
            int displays = 0;
            SDL_DisplayID* ids = SDL_GetDisplays(&displays);
            if (displays > 1) {
                SettingRow("Display", "Where the window opens.", [&] {
                    std::string shown = g.displayIndex.empty() ? std::string("First display") : "Display " + g.displayIndex;
                    if (ImGui::BeginCombo("##display", shown.c_str())) {
                        if (ImGui::Selectable("First display", g.displayIndex.empty())) g.displayIndex.clear();
                        for (int i = 0; i < displays; ++i) {
                            const char* name = SDL_GetDisplayName(ids[i]);
                            const std::string label = std::to_string(i) + ": " + (name != nullptr ? name : "display");
                            if (ImGui::Selectable(label.c_str(), g.displayIndex == std::to_string(i))) g.displayIndex = std::to_string(i);
                        }
                        ImGui::EndCombo();
                    }
                });
            }
            SDL_free(ids);
            SettingRow("Resolution class the game sees", "Some games pick their render size from it. The window size never changes it.", [&] {
                ChoiceCombo("##resolution", g.outputResolution, {{"", "The display's"}, {"1920x1080", "1080p"}, {"3840x2160", "2160p (4K)"}});
            });
            SettingRow("Vertical sync", "Off can tear; the two middle ones keep whole frames without waiting.", [&] {
                ChoiceCombo("##present", g.presentMode, {{"", "On"}, {"fifo_latest_ready", "Latest frame, no wait"}, {"mailbox", "Mailbox, no wait"}, {"immediate", "Off"}});
            });
            SettingRow("Frame rate limit", "Frames per second, like 30 or 59.94. Empty: no limit.", [&] {
                ImGui::InputTextWithHint("##fps", "no limit", &g.fpsLimit, ImGuiInputTextFlags_CharsDecimal);
                if (!ValidFpsLimit(g.fpsLimit)) ImGui::TextColored(Red, "From 0.5 to 1000000, at most three decimals");
            });
            SettingRow("Upscaling", "FSR 1 sharpens a picture smaller than the window; otherwise it does nothing.", [&] {
                int fsr = g.upscale == "fsr" ? 1 : 0;
                const char* labels[] = {"Bilinear", "FSR 1"};
                if (Segmented("upscale", labels, 2, fsr)) g.upscale = fsr == 1 ? "fsr" : "";
                if (fsr == 1) {
                    float sharpness = g.fsrSharpness.empty() ? 0.2f : static_cast<float>(std::atof(g.fsrSharpness.c_str()));
                    ImGui::SetNextItemWidth(-FLT_MIN);
                    if (ImGui::SliderFloat("##sharpness", &sharpness, 0.0f, 2.0f, "sharpness %.1f (0 sharpest)")) {
                        char text[16];
                        std::snprintf(text, sizeof(text), "%.1f", sharpness);
                        g.fsrSharpness = text;
                    }
                }
            });
        });

        Section("version", "AnyPS5 version", "Install and play this game with a version other than the selected one, for example one it's known to run on.", [&] {
            const auto* pinned = FindVersion(versions_, gamePin_);
            const std::string shown = pinned == nullptr ? "The selected one (" + CurrentVersionLabel() + ")" : pinned->Label();
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::BeginCombo("##pin", shown.c_str())) {
                if (ImGui::Selectable(("The selected one (" + CurrentVersionLabel() + ")").c_str(), pinned == nullptr)) gamePin_.clear();
                for (const auto& entry : versions_.versions) {
                    if (ImGui::Selectable(entry.Label().c_str(), entry.id == gamePin_)) gamePin_ = entry.id;
                }
                ImGui::EndCombo();
            }
            const auto current = versions_.pins.count(settingsSerial_) ? versions_.pins.at(settingsSerial_) : std::string();
            if (gamePin_ != current) ImGui::TextColored(Amber, "%s", "After Save the game shows Reinstall: install it again to use this version.");
        });

        Section("overlay", "Performance overlay", "Frames per second, frame times, memory; F10 cycles it while playing.", [&] {
            SettingRow("At start", nullptr, [&] {
                int page = g.overlay.empty() ? 0 : g.overlay == "1" ? 1 : 2;
                const char* labels[] = {"Off", "Page 1", "Page 2"};
                if (Segmented("overlay", labels, 3, page)) g.overlay = page == 0 ? "" : std::to_string(page);
            });
            SettingRow("Corner", nullptr, [&] {
                ChoiceCombo("##corner", g.overlayPosition, {{"", "Top left"}, {"tr", "Top right"}, {"bl", "Bottom left"}, {"br", "Bottom right"}});
            });
        });

        Section("sound", "Sound, vibration and language", nullptr, [&] {
            SettingRow("Volume", "F5 and F6 change it while playing.", [&] {
                int volume = g.volume.empty() ? 100 : std::atoi(g.volume.c_str());
                if (ImGui::SliderInt("##volume", &volume, 0, 100, "%d%%")) g.volume = volume == 100 ? "" : std::to_string(volume / 5 * 5);
            });
            SettingRow("Mute when the game isn't in front", nullptr, [&] { ImGui::Checkbox("##mute", &g.muteOnFocusLoss); });
            SettingRow("Controller vibration", "0 turns it off.", [&] {
                int rumble = g.rumble.empty() ? 100 : std::atoi(g.rumble.c_str());
                if (ImGui::SliderInt("##rumble", &rumble, 0, 100, "%d%%")) g.rumble = rumble == 100 ? "" : std::to_string(rumble);
            });
            SettingRow("Game language", "Instead of the profile's console language.", [&] {
                const std::string shown = g.language.empty() ? "Profile's (" + LanguageLabel(profileSettings_.language) + ")" : LanguageLabel(g.language);
                if (ImGui::BeginCombo("##language", shown.c_str())) {
                    if (ImGui::Selectable("Profile's", g.language.empty())) g.language.clear();
                    if (ImGui::Selectable(LanguageLabel("auto").c_str(), g.language == "auto")) g.language = "auto";
                    for (const auto& item : ConsoleLanguages()) {
                        const auto value = std::to_string(item.number);
                        if (ImGui::Selectable(LanguageLabel(value).c_str(), g.language == value)) g.language = value;
                    }
                    ImGui::EndCombo();
                }
            });
        });

        Section("cache", "Shader cache", "Compiled shaders, so later starts don't stutter. Safe to clear; it's rebuilt.", [&] {
            SettingRow("Keep a shader cache", nullptr, [&] { ImGui::Checkbox("##cache", &g.shaderCache); });
            const auto cache = layout_.ShaderCache(settingsSerial_);
            const bool exists = !cacheSize_.empty();
            ImGui::BeginDisabled(!exists || session_ != nullptr);
            if (ImGui::Button("Clear shader cache")) {
                ClearShaderCache(settingsSerial_);
                RefreshGameFacts();
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::TextColored(Dim, "%s", !factsReady_ ? "measuring..." : exists ? cacheSize_.c_str() : "empty");
        });

        Section("saves", "Saves and trophies", nullptr, [&] {
            const bool busy = session_ != nullptr || job_.Busy();
            ImGui::BeginDisabled(busy || !hasSaves_);
            if (ImGui::Button("Back up now")) StartBackUpSaves(settingsSerial_);
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::Button("Open backups folder")) OpenFolder(SaveBackupFolder(layout_, settingsSerial_));
            ImGui::SameLine();
            if (ImGui::Button("Refresh")) RefreshGameFacts();
            if (!factsReady_) ImGui::TextColored(Dim, "Looking for backups...");
            else if (backups_.empty()) ImGui::TextColored(Dim, "No backups yet.");
            for (const auto& backup : backups_) {
                ImGui::PushID(ToUtf8(backup.file).c_str());
                ImGui::AlignTextToFramePadding();
                ImGui::Text("%s", backup.created.c_str());
                ImGui::SameLine();
                ImGui::TextColored(Dim, "%s", FormatBytes(backup.size).c_str());
                ImGui::SameLine(ImGui::GetContentRegionMax().x - ImGui::CalcTextSize("Restore").x - ImGui::GetStyle().FramePadding.x * 2);
                ImGui::BeginDisabled(busy);
                if (ImGui::SmallButton("Restore")) ImGui::OpenPopup("restore?");
                ImGui::EndDisabled();
                if (ImGui::BeginPopup("restore?")) {
                    ImGui::Text("Put the saves and trophies of %s back?", backup.created.c_str());
                    ImGui::TextColored(Dim, "What the profile has now is backed up first.");
                    if (PrimaryButton("Restore")) {
                        StartRestoreSaves(settingsSerial_, backup.file);
                        ImGui::CloseCurrentPopup();
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
                    ImGui::EndPopup();
                }
                ImGui::PopID();
            }
        });

        Section("share", "Share", "One file with this game's settings and its own controls, to keep or give to someone.", [&] {
            ImGui::BeginDisabled(Picker().IsOpen());
            if (ImGui::Button("Export...")) {
                exportPick_ = settingsSerial_ + ".anyps5game";
                Picker().SaveFile(window_, &exportPick_, "AnyPS5 Launcher game settings", "anyps5game");
            }
            ImGui::SameLine();
            if (ImGui::Button("Import...")) Picker().OpenFile(window_, &importPick_, "AnyPS5 Launcher game settings", "anyps5game");
            ImGui::EndDisabled();
        });
        ImGui::EndChild();

        if (!gameSettingsError_.empty()) ImGui::TextColored(Red, "%s", Elide(gameSettingsError_, ImGui::GetContentRegionAvail().x).c_str());
        else ImGui::TextColored(Dim, "%s", Elide(ToUtf8(layout_.GameSettingsFile(settingsSerial_)), ImGui::GetContentRegionAvail().x).c_str());
        ImGui::Spacing();
        if (ImGui::Button("Reset all")) {
            const bool cache = g.shaderCache;
            g = {};
            g.shaderCache = cache;
        }
        const float buttonWidth = ImGui::GetFontSize() * 5.5f;
        const float buttonHeight = ImGui::GetFrameHeight() * 1.3f;
        ImGui::SameLine(ImGui::GetContentRegionMax().x - buttonWidth * 2 - style.ItemSpacing.x);
        if (ImGui::Button("Cancel", ImVec2(buttonWidth, buttonHeight))) close = true;
        ImGui::SameLine();
        if (PrimaryButton("Save", ImVec2(buttonWidth, buttonHeight))) {
            try {
                if (!ValidFpsLimit(g.fpsLimit)) throw std::runtime_error("the frame rate limit isn't a number AnyPS5 takes");
                SaveGameSettings(layout_.GameSettingsFile(settingsSerial_), g);
                const auto current = versions_.pins.count(settingsSerial_) ? versions_.pins.at(settingsSerial_) : std::string();
                if (gamePin_ != current) {
                    if (gamePin_.empty() || gamePin_ == settings_.anyps5Version) versions_.pins.erase(settingsSerial_);
                    else versions_.pins[settingsSerial_] = gamePin_;
                    SaveVersions(versionsRoot_, versions_);
                }
                message_ = Message{Green, "Game settings saved", settingsName_ + " uses them from its next start."};
                close = true;
            } catch (const std::exception& failure) {
                gameSettingsError_ = ReadableText(failure.what());
            }
        }
        if (close) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    // Writes the game's settings and own controls as one JSON file.
    void ExportGameSettings(const std::filesystem::path& file) {
        try {
            auto target = file;
            if (target.extension() != ".anyps5game") target += ".anyps5game";
            std::ifstream controls(layout_.GameInputConfig(settingsSerial_), std::ios::binary);
            const std::string controlsText{std::istreambuf_iterator<char>(controls), std::istreambuf_iterator<char>()};
            const auto settingsFile = layout_.library / "export.tmp.json";
            SaveGameSettings(settingsFile, gameSettings_);
            std::ifstream settingsStream(settingsFile, std::ios::binary);
            const auto settings = nlohmann::json::parse(std::string(std::istreambuf_iterator<char>(settingsStream), std::istreambuf_iterator<char>()));
            settingsStream.close();
            std::error_code error;
            std::filesystem::remove(settingsFile, error);
            const nlohmann::json document = {{"format", "anyps5-launcher-game"}, {"version", 1}, {"titleId", settingsSerial_}, {"name", settingsName_},
                                             {"settings", settings}, {"controls", controlsText}};
            WriteFileAtomically(target, document.dump(2) + "\n");
            message_ = Message{Green, "Exported", ToUtf8(target)};
        } catch (const std::exception& failure) {
            gameSettingsError_ = "Can't export: " + ReadableText(failure.what());
        }
    }

    // Reads an exported file into the open window (Save keeps it) and the game's controls.
    void ImportGameSettings(const std::filesystem::path& file) {
        try {
            std::ifstream stream(file, std::ios::binary);
            const auto document = nlohmann::json::parse(std::string(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()), nullptr, false);
            if (document.is_discarded() || !document.is_object() || document.value("format", "") != "anyps5-launcher-game") {
                throw std::runtime_error(ToUtf8(file.filename()) + " isn't an AnyPS5 Launcher game settings file");
            }
            const auto from = document.value("titleId", "");
            if (document.contains("settings") && document["settings"].is_object()) {
                const auto temp = layout_.library / "import.tmp.json";
                WriteFileAtomically(temp, document["settings"].dump());
                gameSettings_ = LoadGameSettings(temp);
                std::error_code error;
                std::filesystem::remove(temp, error);
            }
            if (document.contains("controls") && document["controls"].is_string() && !document["controls"].get<std::string>().empty()) {
                const auto parsed = ParseInputFile(document["controls"].get<std::string>());
                controlsName_ = settingsName_;
                controlsSerial_ = settingsSerial_;
                WriteInputFile(layout_.GameInputConfig(settingsSerial_), parsed, GameInputHeader());
            }
            gameSettingsError_.clear();
            message_ = Message{Green, "Imported", (from.empty() || from == settingsSerial_ ? std::string() : "From " + from + ". ") + "The controls are saved; press Save to keep the settings."};
        } catch (const std::exception& failure) {
            gameSettingsError_ = "Can't import: " + ReadableText(failure.what());
        }
    }

    void OpenScreenshots(const GameEntry& game) {
        settingsSerial_ = game.metadata.titleId;
        settingsName_ = game.metadata.name;
        shots_.clear();
        std::error_code error;
        for (std::filesystem::directory_iterator it(layout_.Screenshots(settingsSerial_), error), end; !error && it != end; it.increment(error)) {
            if (it->is_regular_file(error) && it->path().extension() == ".png") shots_.push_back(it->path());
        }
        std::sort(shots_.begin(), shots_.end(), [](const auto& a, const auto& b) { return a.filename() > b.filename(); });
        shotViewed_ = -1;
        openScreenshots_ = true;
    }

    // A screenshot as a texture, read from disk the first time it's needed (at most a few per frame).
    SDL_Texture* ShotTexture(const std::filesystem::path& file, int& budget) {
        static const std::vector<std::uint8_t> none;
        if (shotLoaded_.count(file) == 0) {
            if (budget <= 0) return nullptr;
            --budget;
            std::ifstream stream(file, std::ios::binary);
            const std::vector<std::uint8_t> png{std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
            shotLoaded_.insert(file);
            return icons_->Get(file, png);
        }
        return icons_->Get(file, none);
    }

    void DrawScreenshots() {
        if (openScreenshots_) {
            ImGui::OpenPopup("Screenshots");
            openScreenshots_ = false;
        }
        FollowWindow(0.86f, 0.90f);
        if (!BeginDialog("Screenshots")) return;
        const std::string title = "Screenshots: " + settingsName_;
        const std::string note = "F12 while playing saves one. " + std::to_string(shots_.size()) + (shots_.size() == 1 ? " picture" : " pictures") + " in profile " + layout_.profile + ".";
        bool close = DialogHeader(title.c_str(), note.c_str());
        if (ImGui::Button("Open folder")) OpenFolder(layout_.Screenshots(settingsSerial_));
        ImGui::Spacing();
        int budget = 2;
        if (shotViewed_ >= 0 && shotViewed_ < static_cast<int>(shots_.size())) {
            // One picture, as large as fits; arrows (or the d-pad) move, Esc goes back.
            const auto& file = shots_[static_cast<std::size_t>(shotViewed_)];
            ImGui::TextUnformatted(ToUtf8(file.filename()).c_str());
            ImGui::SameLine(ImGui::GetContentRegionMax().x - ImGui::GetFontSize() * 16);
            if (ImGui::Button("<")) shotViewed_ = std::max(0, shotViewed_ - 1);
            ImGui::SameLine();
            if (ImGui::Button(">")) shotViewed_ = std::min(static_cast<int>(shots_.size()) - 1, shotViewed_ + 1);
            ImGui::SameLine();
            if (ImGui::Button("Open")) OpenPath(file);
            ImGui::SameLine();
            if (ImGui::Button("Delete")) {
                try {
                    MoveToBin(file);
                    shots_.erase(shots_.begin() + shotViewed_);
                    if (shotViewed_ >= static_cast<int>(shots_.size())) shotViewed_ = static_cast<int>(shots_.size()) - 1;
                } catch (const std::exception& failure) {
                    message_ = Message{Red, "Can't delete", ReadableText(failure.what())};
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("All")) shotViewed_ = -1;
            if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow) || ImGui::IsKeyPressed(ImGuiKey_GamepadDpadLeft)) shotViewed_ = std::max(0, shotViewed_ - 1);
            if (ImGui::IsKeyPressed(ImGuiKey_RightArrow) || ImGui::IsKeyPressed(ImGuiKey_GamepadDpadRight)) shotViewed_ = std::min(static_cast<int>(shots_.size()) - 1, shotViewed_ + 1);
            if (ImGui::IsKeyPressed(ImGuiKey_Escape) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight)) shotViewed_ = -1;
            if (shotViewed_ >= 0 && shotViewed_ < static_cast<int>(shots_.size())) {
                const ImVec2 room = ImGui::GetContentRegionAvail();
                budget = 1;
                if (auto* texture = ShotTexture(shots_[static_cast<std::size_t>(shotViewed_)], budget)) {
                    float w = 0;
                    float h = 0;
                    SDL_GetTextureSize(texture, &w, &h);
                    const float fit = std::min(room.x / std::max(w, 1.0f), room.y / std::max(h, 1.0f));
                    const ImVec2 size(w * fit, h * fit);
                    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (room.x - size.x) * 0.5f);
                    ImGui::Image(static_cast<ImTextureID>(reinterpret_cast<std::intptr_t>(texture)), size);
                } else {
                    ImGui::TextColored(Dim, "Loading...");
                }
            }
        } else if (shots_.empty()) {
            ImGui::TextColored(Dim, "No screenshots yet. Press F12 while the game plays.");
        } else {
            ImGui::BeginChild("shots");
            const float gap = 10.0f * scale_;
            const float avail = ImGui::GetContentRegionAvail().x;
            const int columns = std::max(1, static_cast<int>((avail + gap) / (240.0f * scale_ + gap)));
            const float width = (avail - gap * static_cast<float>(columns - 1)) / static_cast<float>(columns);
            const float height = width * 9.0f / 16.0f;
            for (std::size_t i = 0; i < shots_.size(); ++i) {
                if (i % static_cast<std::size_t>(columns) != 0) ImGui::SameLine(0, gap);
                ImGui::PushID(static_cast<int>(i));
                const ImVec2 at = ImGui::GetCursorScreenPos();
                // Only pictures on screen are read.
                const bool visible = ImGui::IsRectVisible(ImVec2(width, height));
                if (ImGui::InvisibleButton("shot", ImVec2(width, height))) shotViewed_ = static_cast<int>(i);
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", ToUtf8(shots_[i].filename()).c_str());
                auto* draw = ImGui::GetWindowDrawList();
                SDL_Texture* texture = visible ? ShotTexture(shots_[i], budget) : nullptr;
                if (texture != nullptr) {
                    draw->AddImageRounded(static_cast<ImTextureID>(reinterpret_cast<std::intptr_t>(texture)), at, ImVec2(at.x + width, at.y + height), ImVec2(0, 0), ImVec2(1, 1),
                                          IM_COL32_WHITE, 8.0f * scale_);
                } else {
                    draw->AddRectFilled(at, ImVec2(at.x + width, at.y + height), ImGui::GetColorU32(Placeholder), 8.0f * scale_);
                }
                if (ImGui::IsItemHovered() || ImGui::IsItemFocused()) draw->AddRect(at, ImVec2(at.x + width, at.y + height), ImGui::GetColorU32(Accent), 8.0f * scale_, 0, 2.0f * scale_);
                ImGui::PopID();
            }
            ImGui::EndChild();
        }
        if (close) {
            for (const auto& file : shotLoaded_) icons_->Forget(file);
            shotLoaded_.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    // "Starting <game>" until its window shows (or 25 s, where the window can't be found).
    void DrawLaunchScreen() {
        if (!launching_) return;
        if (!session_) {
            launching_ = false;
            return;
        }
        const float elapsed = static_cast<float>(SDL_GetTicks() - launchStarted_) / 1000.0f;
        int x = 0;
        int y = 0;
        int w = 0;
        int h = 0;
        const bool shown = session_->WindowRect(x, y, w, h) && w > 0;
        if (shown || elapsed > 25.0f) {
            launching_ = false;
            if (shown && settings_.hideWhilePlaying) {
                SDL_MinimizeWindow(window_);
                minimizedForGame_ = true;
            }
            return;
        }
        if (!ImGui::IsPopupOpen("Starting")) ImGui::OpenPopup("Starting");
        ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(28.0f * scale_, 24.0f * scale_));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 16.0f * scale_);
        const bool open = ImGui::BeginPopupModal("Starting", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove);
        ImGui::PopStyleVar(2);
        if (!open) return;
        const GameEntry* game = nullptr;
        for (const auto& entry : result_.games) {
            if (IsRunning(entry)) game = &entry;
        }
        const float art = ImGui::GetFontSize() * 9;
        if (game != nullptr) {
            if (auto* texture = icons_->Get(game->location, game->icon)) {
                const ImVec2 at = ImGui::GetCursorScreenPos();
                ImGui::GetWindowDrawList()->AddImageRounded(static_cast<ImTextureID>(reinterpret_cast<std::intptr_t>(texture)), at, ImVec2(at.x + art, at.y + art), ImVec2(0, 0),
                                                            ImVec2(1, 1), IM_COL32_WHITE, 14.0f * scale_);
                ImGui::Dummy(ImVec2(art, art));
            }
        }
        ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.3f);
        ImGui::TextUnformatted(session_->Name().c_str());
        ImGui::PopFont();
        // Three dots that take turns.
        const int dots = static_cast<int>(elapsed * 3.0f) % 4;
        ImGui::TextColored(Dim, "Starting%s  %.0f s", std::string(static_cast<std::size_t>(dots), '.').c_str(), static_cast<double>(elapsed));
        ImGui::TextColored(Dim, "%s", "The first start of a game takes longest: shaders are compiled.");
        ImGui::Spacing();
        if (ImGui::Button("Hide")) launching_ = false;
        ImGui::SameLine();
        if (ImGui::Button("Stop")) session_->Stop();
        if (!launching_) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
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
                    runTrophies_ = LoadTrophies(*package, TrophyLanguages(profileSettings_.language), ParseTrophyKey(settings_.ps4TrophyKey));
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
            const auto ps4Key = ParseTrophyKey(settings_.ps4TrophyKey);
            trophyLoader_.Start([copy, key, records, languages, ps4Key] {
                TrophyLoad load{key, std::nullopt};
                try {
                    auto package = OpenGamePackage(copy);
                    load.set = LoadTrophies(*package, languages, ps4Key);
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
            ImGui::TextColored(Dim, "No trophy list in the game files (sce_sys/trophy2 or sce_sys/trophy).");
            return;
        }
        if (set->encrypted) {
            DrawLockedTrophies(*set);
            return;
        }
        const auto count = set->Count();
        ImGui::Text("%d of %d earned  \xC2\xB7  %d%%", count.earned, count.total, set->Percent());
        ProgressBar(static_cast<float>(set->Percent()) / 100.0f, ImGui::GetContentRegionAvail().x, Accent);
        DrawGradeCounts(*set);
    }

    // A PS4 list without the key: how many there are, and where the key goes.
    void DrawLockedTrophies(const TrophySet& set) {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::Text("%d trophies", set.lockedCount);
        const bool badKey = !settings_.ps4TrophyKey.empty();
        ImGui::TextColored(badKey ? Amber : Dim, "%s",
                           badKey ? "The PS4 trophy key in Settings doesn't open this list. Check that it's your console's key (32 hex digits)."
                                  : "This PS4 game's trophy list is encrypted. Add your console's PS4 trophy key in Settings > Games to see the names and grades.");
        ImGui::PopTextWrapPos();
        if (ImGui::SmallButton("Open Settings")) {
            ImGui::CloseCurrentPopup();
            settingsPage_ = SettingsPage::Games;
            OpenSettings();
        }
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
        const std::string note = set == nullptr || set->encrypted ? std::string() : std::to_string(set->Count().earned) + " of " + std::to_string(set->Count().total) +
                                                                  " earned, " + std::to_string(set->Percent()) + "%. Kept per profile (" + layout_.profile +
                                                                  "); the list updates when the game closes.";
        if (DialogHeader(title.c_str(), note.c_str())) ImGui::CloseCurrentPopup();
        if (set == nullptr) {
            ImGui::TextColored(Dim, "%s", loading ? "Reading the trophy list..." : "This game has no trophy list in its files (sce_sys/trophy2 or sce_sys/trophy).");
            ImGui::EndPopup();
            return;
        }
        if (set->encrypted) {
            DrawLockedTrophies(*set);
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
        if (ImGui::Button("Screenshots", ImVec2(half, 0))) OpenScreenshots(game);
        if (ImGui::Button("Controls", ImVec2(half, 0))) OpenControls(game);
        ImGui::SameLine();
        if (ImGui::Button("Game settings", ImVec2(half, 0))) OpenGameSettings(game);
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
        } else if (JobOn(game)) {
            ImGui::TextColored(Amber, "Installing...");
        } else if (const auto* manifest = InstallFor(game)) {
            const auto reason = ReinstallReason(game, *manifest, StateFor(serial).version);
            if (reason.empty()) ImGui::TextColored(Green, "Installed %s", manifest->installedAt.substr(0, 10).c_str());
            else ImGui::TextColored(Amber, "Reinstall: %s", reason.c_str());
        } else {
            ImGui::TextColored(Dim, "Not installed");
        }
        if (const auto played = PlayedSeconds(serial); played > 0 || playtimes_.count(serial) != 0) {
            std::string text = played < 60 ? std::string("Played less than a minute") : "Played " + FormatPlaytime(played);
            if (const auto found = playtimes_.find(serial); found != playtimes_.end() && found->second.lastPlayed > 0 && !IsRunning(game)) {
                text += "  \xC2\xB7  last " + LocalDate(found->second.lastPlayed).substr(0, 10);
            }
            ImGui::TextColored(Dim, "%s", text.c_str());
            if (ImGui::IsItemHovered()) {
                const auto found = playtimes_.find(serial);
                const int sessions = found == playtimes_.end() ? 0 : found->second.sessions;
                ImGui::SetTooltip("%d session%s with profile %s, counted from when the game's window shows", sessions, sessions == 1 ? "" : "s", layout_.profile.c_str());
            }
        }
        if (const auto found = diagnoses_.find(serial); found != diagnoses_.end() && !IsRunning(game)) {
            const auto& diagnosis = found->second;
            ImGui::PushTextWrapPos(0.0f);
            const bool problem = diagnosis.level == RunDiagnosis::Level::Problem;
            ImGui::TextColored(problem ? Red : Dim, "Last run: %s", diagnosis.headline.c_str());
            if (problem && !diagnosis.detail.empty()) ImGui::TextColored(Dim, "%s", diagnosis.detail.c_str());
            if (problem && !diagnosis.hint.empty()) ImGui::TextColored(Amber, "%s", diagnosis.hint.c_str());
            ImGui::PopTextWrapPos();
            if (problem) {
                if (ImGui::SmallButton("Make a report")) MakeReport(game);
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("One text file for a GitHub issue: system, versions, settings, the reason and the end of the log (your user name and home folder removed)");
                ImGui::SameLine();
                if (ImGui::SmallButton("Open an issue")) OpenIssue(game);
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Opens a new issue on github.com/boykopovar/AnyPS5 with the facts filled in; you check it and submit it");
                ImGui::SameLine();
                if (ImGui::SmallButton("Open log")) OpenLatestLog(serial);
            }
        }
    }

    // The last run of a game, from its newest log and that run's report.
    void Diagnose(const std::string& serial) {
        const auto log = LatestRunLog(layout_, serial);
        if (!log) {
            diagnoses_.erase(serial);
            return;
        }
        auto reportFile = *log;
        reportFile.replace_extension(".report.json");
        diagnoses_[serial] = DiagnoseRun(ReadRunReport(reportFile), ReadLogTail(*log, 128 * 1024), std::nullopt, false);
    }

    void OpenLatestLog(const std::string& serial) {
        if (const auto log = LatestRunLog(layout_, serial)) OpenPath(*log);
        else OpenFolder(layout_.Logs(serial));
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
        if (ImGui::MenuItem("Game settings...")) OpenGameSettings(game);
        if (ImGui::BeginMenu("Copy link")) {
            if (ImGui::MenuItem(("Play: " + LinkFor({LaunchRequest::Kind::Play, serial})).c_str())) CopyLink(game, LaunchRequest::Kind::Play);
            if (ImGui::MenuItem(("Show: " + LinkFor({LaunchRequest::Kind::Select, serial})).c_str())) CopyLink(game, LaunchRequest::Kind::Select);
            ImGui::EndMenu();
        }
        if (ImGui::MenuItem("Screenshots...")) OpenScreenshots(game);
        {
            const bool hasSaves = HasSaves(layout_, serial);
            if (MenuAction("Back up saves", hasSaves ? (session_ ? "Not while a game runs" : "") : "No saves or trophies of this game in profile " + layout_.profile + " yet",
                           "Zips the saves and trophies of profile " + layout_.profile + " into " + ToUtf8(SaveBackupFolder(layout_, serial)))) {
                StartBackUpSaves(serial);
            }
        }
        if (MenuAction("Open last log", PathExists(layout_.Logs(serial)) ? "" : "No logs yet", ToUtf8(layout_.Logs(serial)))) OpenLatestLog(serial);
        if (MenuAction("Make a report", PathExists(layout_.Logs(serial)) ? "" : "Run the game once first",
                       "One text file for a GitHub issue: system, versions, settings, the last run's reason and the end of its log, with your user name and home folder removed")) {
            MakeReport(game);
        }
        if (MenuAction("Clear shader cache", PathExists(layout_.ShaderCache(serial)) ? (IsRunning(game) ? "Not while the game runs" : "") : "No shader cache",
                       "Deletes " + ToUtf8(layout_.ShaderCache(serial)) + "; AnyPS5 builds it again (the next start is slower)")) {
            ClearShaderCache(serial);
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
                           "First stores the originals in " + ToUtf8(OriginalsZipFor(game, layout_.library)) + ", with their MD5, and checks the zip.\n"
                           "Then replaces eboot.bin, the SELF modules (sce_module, sce_modules, prx, the title's module folders) and extra executables with their ELF.\n"
                           "Same conversion as AnyPS5's unwrap_self.py. Install needs this done first.")) {
                StartConvertSelf(game);
            }
        }
        {
            const auto zip = OriginalsZipFor(game, layout_.library);
            std::string reason = busy;
            if (reason.empty() && game.format != PackageFormat::Folder) reason = "Only for game folders";
            if (reason.empty() && !PathExists(zip)) reason = "No " + FileName(zip) + " in " + ToUtf8(zip.parent_path());
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
        if (settings_.anyps5Folder.empty()) return "none";
        const auto leaf = ToUtf8(FromUtf8(settings_.anyps5Folder).filename());
        return leaf.empty() ? std::string("your own build") : leaf;
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
                    if (entry.Local() && renamingVersion_ == entry.id) {
                        // Naming it in place: Enter or clicking away keeps the name, Esc drops it.
                        ImGui::SetNextItemWidth(std::min(ImGui::GetContentRegionAvail().x, ImGui::GetFontSize() * 16.0f));
                        if (ImGui::IsWindowAppearing() || renameFocus_) {
                            ImGui::SetKeyboardFocusHere();
                            renameFocus_ = false;
                        }
                        const bool entered = ImGui::InputTextWithHint("##name", "A name for this build", &renameText_, ImGuiInputTextFlags_EnterReturnsTrue);
                        if (ImGui::IsItemActive() && ImGui::IsKeyPressed(ImGuiKey_Escape)) {
                            renamingVersion_.clear();
                        } else if (entered || ImGui::IsItemDeactivated()) {
                            try {
                                RenameVersion(versions_, entry.id, renameText_);
                                SaveVersions(versionsRoot_, versions_);
                            } catch (const std::exception& failure) {
                                versionsError_ = failure.what();
                            }
                            renamingVersion_.clear();
                        }
                    } else {
                        ImGui::PushFont(nullptr, style.FontSizeBase * 1.08f);
                        ImGui::TextUnformatted(entry.Label().c_str());
                        ImGui::PopFont();
                        if (entry.Local() && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", ToUtf8(entry.folder).c_str());
                    }
                    if (entry.Local()) {
                        ImGui::TextColored(Dim, "%s", "Your own build");
                        ImGui::SameLine();
                        ImGui::BeginDisabled(!renamingVersion_.empty());
                        if (ImGui::SmallButton("Rename")) {
                            renamingVersion_ = entry.id;
                            renameText_ = entry.name.empty() ? entry.Label() : entry.name;
                            renameFocus_ = true;
                        }
                        ImGui::EndDisabled();
                        ImGui::SameLine();
                        if (ImGui::SmallButton("Open folder")) OpenFolder(entry.folder);
                    } else {
                        ImGui::TextColored(Dim, "%s", Elide(entry.repository, ImGui::GetContentRegionAvail().x).c_str());
                    }
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
        {
            const auto outdated = GamesToReinstall();
            ImGui::BeginDisabled(outdated.empty() || busy || !reinstallQueue_.empty());
            const std::string label = outdated.empty() ? std::string("Reinstall all") : "Reinstall all (" + std::to_string(outdated.size()) + ")";
            if (ImGui::Button(label.c_str(), ImVec2(0, ImGui::GetFrameHeight() * 1.3f))) ReinstallAll();
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                ImGui::SetTooltip("%s", outdated.empty() ? "Every installed game uses the relinker it was converted with" : "Installs again, one after another, every game converted with another AnyPS5 than the one it uses now");
            }
            ImGui::SameLine();
        }
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

    // width: the whole row, field and Browse... together; 0: up to the window's edge.
    void FolderField(const char* label, const char* note, const char* id, const std::string& hint, std::string& value, float width = 0.0f) {
        ImGui::TextUnformatted(label);
        if (note != nullptr) ImGui::TextColored(Dim, "%s", note);
        const auto& style = ImGui::GetStyle();
        const float browse = ImGui::CalcTextSize("Browse...").x + style.FramePadding.x * 2 + style.ItemSpacing.x;
        if (width > 0.0f) ImGui::SetNextItemWidth(width - browse);
        else ImGui::SetNextItemWidth(-browse);
        ImGui::InputTextWithHint(id, hint.c_str(), &value);
        ImGui::SameLine();
        ImGui::PushID(id);
        ImGui::BeginDisabled(Picker().IsOpen());
        if (ImGui::Button("Browse...")) Picker().Open(window_, &value);
        ImGui::EndDisabled();
        ImGui::PopID();
    }

    // ---- settings: pages in a sidebar, each setting a row with its control on the right ----

    enum class SettingsPage { Library, Games, Appearance, Controls, Saves, Playing, Launcher };

    struct SettingsPageText {
        SettingsPage page;
        const char* title;
        const char* note;
    };

    static constexpr SettingsPageText SettingsPages[] = {
        {SettingsPage::Library, "Library", "Where your games are, and where the launcher keeps its own files."},
        {SettingsPage::Games, "Games", "How games are converted, and what the launcher looks up about them."},
        {SettingsPage::Appearance, "Appearance", "How the launcher looks. A theme shows at once; Save keeps it."},
        {SettingsPage::Controls, "Controls", "Keys for every game of this profile, and the pads the launcher sees."},
        {SettingsPage::Saves, "Saves", "Keep saves and trophies the same on your other PCs."},
        {SettingsPage::Playing, "While playing", "What the launcher does while a game runs."},
        {SettingsPage::Launcher, "About", "This launcher's version and its updates."},
    };

    // A small line icon for each page, drawn in code like the top bar's.
    void DrawPageGlyph(ImDrawList* draw, SettingsPage page, ImVec2 c, float r, ImU32 ink) {
        const float s = scale_;
        const float line = 1.6f * s;
        switch (page) {
        case SettingsPage::Library:  // a folder
            draw->AddRect(ImVec2(c.x - r, c.y - r * 0.55f), ImVec2(c.x + r, c.y + r * 0.75f), ink, 2 * s, 0, line);
            draw->AddLine(ImVec2(c.x - r, c.y - r * 0.55f), ImVec2(c.x - r * 0.15f, c.y - r * 0.55f), ink, line * 1.8f);
            break;
        case SettingsPage::Games:  // a play triangle in a circle
            draw->AddCircle(c, r, ink, 0, line);
            draw->AddTriangleFilled(ImVec2(c.x - r * 0.3f, c.y - r * 0.45f), ImVec2(c.x - r * 0.3f, c.y + r * 0.45f), ImVec2(c.x + r * 0.5f, c.y), ink);
            break;
        case SettingsPage::Appearance:  // half a moon in a circle
            draw->AddCircle(c, r, ink, 0, line);
            draw->PathArcTo(c, r, -1.5707963f, 1.5707963f, 16);
            draw->PathFillConvex(ink);
            break;
        case SettingsPage::Controls:  // a pad: body and two buttons
            draw->AddRect(ImVec2(c.x - r * 1.1f, c.y - r * 0.55f), ImVec2(c.x + r * 1.1f, c.y + r * 0.6f), ink, r * 0.5f, 0, line);
            draw->AddLine(ImVec2(c.x - r * 0.75f, c.y), ImVec2(c.x - r * 0.25f, c.y), ink, line);
            draw->AddLine(ImVec2(c.x - r * 0.5f, c.y - r * 0.25f), ImVec2(c.x - r * 0.5f, c.y + r * 0.25f), ink, line);
            draw->AddCircleFilled(ImVec2(c.x + r * 0.5f, c.y), r * 0.15f, ink);
            break;
        case SettingsPage::Saves: {  // two arrows going round
            draw->PathArcTo(c, r * 0.85f, 3.4f, 5.9f, 12);
            draw->PathStroke(ink, 0, line);
            draw->PathArcTo(c, r * 0.85f, 0.25f, 2.75f, 12);
            draw->PathStroke(ink, 0, line);
            const ImVec2 a(c.x + std::cos(5.9f) * r * 0.85f, c.y + std::sin(5.9f) * r * 0.85f);
            const ImVec2 b(c.x + std::cos(2.75f) * r * 0.85f, c.y + std::sin(2.75f) * r * 0.85f);
            draw->AddTriangleFilled(ImVec2(a.x - r * 0.3f, a.y - r * 0.15f), ImVec2(a.x + r * 0.25f, a.y - r * 0.2f), ImVec2(a.x, a.y + r * 0.3f), ink);
            draw->AddTriangleFilled(ImVec2(b.x + r * 0.3f, b.y + r * 0.15f), ImVec2(b.x - r * 0.25f, b.y + r * 0.2f), ImVec2(b.x, b.y - r * 0.3f), ink);
            break;
        }
        case SettingsPage::Playing:  // a trophy cup
            draw->AddRectFilled(ImVec2(c.x - r * 0.6f, c.y - r * 0.8f), ImVec2(c.x + r * 0.6f, c.y + r * 0.05f), ink, r * 0.5f, ImDrawFlags_RoundCornersBottom);
            draw->AddLine(ImVec2(c.x, c.y), ImVec2(c.x, c.y + r * 0.55f), ink, line);
            draw->AddLine(ImVec2(c.x - r * 0.45f, c.y + r * 0.7f), ImVec2(c.x + r * 0.45f, c.y + r * 0.7f), ink, line * 1.3f);
            break;
        case SettingsPage::Launcher:  // an i in a circle
            draw->AddCircle(c, r, ink, 0, line);
            draw->AddCircleFilled(ImVec2(c.x, c.y - r * 0.42f), r * 0.13f, ink);
            draw->AddLine(ImVec2(c.x, c.y - r * 0.1f), ImVec2(c.x, c.y + r * 0.5f), ink, line * 1.2f);
            break;
        }
    }

    // A square button with an eye: open when shown is true, struck through when not.
    bool EyeButton(const char* id, bool shown) {
        const float size = ImGui::GetFrameHeight();
        const ImVec2 at = ImGui::GetCursorScreenPos();
        const bool pressed = ImGui::InvisibleButton(id, ImVec2(size, size));
        const bool hovered = ImGui::IsItemHovered();
        if (hovered) ImGui::SetTooltip("%s", shown ? "Hide the key" : "Show the key");
        auto* draw = ImGui::GetWindowDrawList();
        const auto& colors = ImGui::GetStyle().Colors;
        draw->AddRectFilled(at, ImVec2(at.x + size, at.y + size), ImGui::GetColorU32(hovered ? colors[ImGuiCol_ButtonHovered] : WindowColor), ImGui::GetStyle().FrameRounding);
        const ImVec2 c(at.x + size * 0.5f, at.y + size * 0.5f);
        const float w = size * 0.32f;
        const float h = size * 0.18f;
        const ImU32 ink = ImGui::GetColorU32(hovered ? colors[ImGuiCol_Text] : Dim);
        const float line = std::max(1.3f, 1.5f * scale_);
        // The eye's outline: two curves meeting at the corners.
        draw->AddBezierQuadratic(ImVec2(c.x - w, c.y), ImVec2(c.x, c.y - h * 2.0f), ImVec2(c.x + w, c.y), ink, line, 16);
        draw->AddBezierQuadratic(ImVec2(c.x - w, c.y), ImVec2(c.x, c.y + h * 2.0f), ImVec2(c.x + w, c.y), ink, line, 16);
        draw->AddCircleFilled(c, h * 0.75f, ink);
        if (!shown) {
            draw->AddLine(ImVec2(c.x - w * 0.9f, c.y + h * 1.6f), ImVec2(c.x + w * 0.9f, c.y - h * 1.6f), ImGui::GetColorU32(WindowColor), line * 3.0f);
            draw->AddLine(ImVec2(c.x - w * 0.9f, c.y + h * 1.6f), ImVec2(c.x + w * 0.9f, c.y - h * 1.6f), ink, line);
        }
        if (hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        return pressed;
    }

    // An on/off switch with a short slide. Returns true when flipped.
    bool Toggle(const char* id, bool* value) {
        const float s = scale_;
        const float h = 22.0f * s;
        const float w = 40.0f * s;
        const ImVec2 at = ImGui::GetCursorScreenPos();
        const float frame = ImGui::GetFrameHeight();
        const bool pressed = ImGui::InvisibleButton(id, ImVec2(w, std::max(h, frame)));
        if (pressed) *value = !*value;
        const bool hovered = ImGui::IsItemHovered();
        const bool disabled = (ImGui::GetItemFlags() & ImGuiItemFlags_Disabled) != 0;
        auto* storage = ImGui::GetStateStorage();
        const ImGuiID key = ImGui::GetItemID();
        float t = storage->GetFloat(key, *value ? 1.0f : 0.0f);
        const float target = *value ? 1.0f : 0.0f;
        t += (target - t) * std::min(1.0f, ImGui::GetIO().DeltaTime * 18.0f);
        if (std::fabs(target - t) < 0.01f) t = target;
        storage->SetFloat(key, t);
        auto* draw = ImGui::GetWindowDrawList();
        const float top = at.y + (std::max(h, frame) - h) * 0.5f;
        const ImVec4 off = Fade(Dim, hovered ? 0.6f : 0.45f);
        ImVec4 track(off.x + (Accent.x - off.x) * t, off.y + (Accent.y - off.y) * t, off.z + (Accent.z - off.z) * t, 1.0f);
        if (disabled) track.w = 0.45f;
        draw->AddRectFilled(ImVec2(at.x, top), ImVec2(at.x + w, top + h), ImGui::GetColorU32(track), h * 0.5f);
        const float knob = h * 0.5f - 3.0f * s;
        const ImVec2 kc(at.x + h * 0.5f + (w - h) * t, top + h * 0.5f);
        draw->AddCircleFilled(ImVec2(kc.x, kc.y + 1.0f * s), knob, IM_COL32(0, 0, 0, 40));
        draw->AddCircleFilled(kc, knob, disabled ? IM_COL32(255, 255, 255, 150) : IM_COL32_WHITE);
        if (hovered && !disabled) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        return pressed;
    }

    // A rounded card behind whatever body draws, as wide as the page.
    template <typename Body>
    void CardBlock(const char* id, Body body) {
        const float s = scale_;
        const float pad = 16.0f * s;
        auto* draw = ImGui::GetWindowDrawList();
        const ImVec2 at = ImGui::GetCursorScreenPos();
        const float width = ImGui::GetContentRegionAvail().x;
        ImGui::PushID(id);
        draw->ChannelsSplit(2);
        draw->ChannelsSetCurrent(1);
        ImGui::SetCursorScreenPos(ImVec2(at.x + pad, at.y + pad));
        ImGui::BeginGroup();
        ImGui::PushItemWidth(width - pad * 2);
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + width - pad * 2);
        ImGui::PushStyleColor(ImGuiCol_FrameBg, WindowColor);
        ImGui::PushStyleColor(ImGuiCol_Button, WindowColor);
        body(width - pad * 2);
        ImGui::PopStyleColor(2);
        ImGui::PopTextWrapPos();
        ImGui::PopItemWidth();
        ImGui::EndGroup();
        const float bottom = ImGui::GetItemRectMax().y + pad;
        draw->ChannelsSetCurrent(0);
        draw->AddRectFilled(at, ImVec2(at.x + width, bottom), ImGui::GetColorU32(CardHover), 12.0f * s);
        draw->ChannelsMerge();
        ImGui::PopID();
        ImGui::SetCursorScreenPos(ImVec2(at.x, bottom));
        ImGui::Dummy(ImVec2(width, 10.0f * s));
    }

    // One setting: its name and a line about it on the left, its control on the right.
    template <typename Control>
    void SettingRow(const char* id, const char* title, const char* note, float controlWidth, Control control) {
        const float s = scale_;
        const float pad = 16.0f * s;
        auto* draw = ImGui::GetWindowDrawList();
        const ImVec2 at = ImGui::GetCursorScreenPos();
        const float width = ImGui::GetContentRegionAvail().x;
        ImGui::PushID(id);
        draw->ChannelsSplit(2);
        draw->ChannelsSetCurrent(1);
        const float textWidth = width - pad * 3 - controlWidth;
        const float textHeight = ImGui::GetTextLineHeight() + (note != nullptr && note[0] != 0 ? ImGui::CalcTextSize(note, nullptr, false, textWidth).y + 3.0f * s : 0.0f);
        const float inner = std::max(textHeight, ImGui::GetFrameHeight());
        ImGui::SetCursorScreenPos(ImVec2(at.x + pad, at.y + pad + (inner - textHeight) * 0.5f));
        ImGui::BeginGroup();
        ImGui::TextUnformatted(title);
        if (note != nullptr && note[0] != 0) {
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + textWidth);
            ImGui::TextColored(Dim, "%s", note);
            ImGui::PopTextWrapPos();
        }
        ImGui::EndGroup();
        ImGui::SetCursorScreenPos(ImVec2(at.x + width - pad - controlWidth, at.y + pad + (inner - ImGui::GetFrameHeight()) * 0.5f));
        ImGui::BeginGroup();
        ImGui::PushStyleColor(ImGuiCol_FrameBg, WindowColor);
        ImGui::PushStyleColor(ImGuiCol_Button, WindowColor);
        control();
        ImGui::PopStyleColor(2);
        ImGui::EndGroup();
        const float bottom = at.y + pad * 2 + inner;
        draw->ChannelsSetCurrent(0);
        draw->AddRectFilled(at, ImVec2(at.x + width, bottom), ImGui::GetColorU32(CardHover), 12.0f * s);
        draw->ChannelsMerge();
        ImGui::PopID();
        ImGui::SetCursorScreenPos(ImVec2(at.x, bottom));
        ImGui::Dummy(ImVec2(width, 8.0f * s));
    }

    void ToggleRow(const char* id, const char* title, const char* note, bool* value) {
        SettingRow(id, title, note, 40.0f * scale_, [&] { Toggle("##toggle", value); });
    }

    // A label above a group of rows.
    void GroupLabel(const char* text) {
        ImGui::Dummy(ImVec2(0, 4.0f * scale_));
        ImGui::TextColored(Dim, "%s", text);
        ImGui::Dummy(ImVec2(0, 2.0f * scale_));
    }

    void DrawSettingsNav(float width) {
        ImGui::BeginChild("settings-nav", ImVec2(width, 0), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoBackground);
        auto* draw = ImGui::GetWindowDrawList();
        const ImVec2 origin = ImGui::GetWindowPos();
        const ImVec2 size = ImGui::GetWindowSize();
        const float s = scale_;
        draw->AddRectFilled(origin, ImVec2(origin.x + size.x, origin.y + size.y), ImGui::GetColorU32(Card), 14.0f * s);
        const float pad = 14.0f * s;
        ImGui::SetCursorScreenPos(ImVec2(origin.x + pad + 6 * s, origin.y + pad + 4 * s));
        ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.35f);
        ImGui::TextUnformatted("Settings");
        ImGui::PopFont();
        ImGui::SetCursorScreenPos(ImVec2(origin.x + pad + 6 * s, ImGui::GetCursorScreenPos().y));
        ImGui::TextColored(Dim, "Profile %s", layout_.profile.c_str());
        float y = ImGui::GetCursorScreenPos().y + 12 * s;
        const float rowHeight = 38.0f * s;
        for (const auto& page : SettingsPages) {
            const ImVec2 at(origin.x + pad * 0.6f, y);
            const ImVec2 end(origin.x + size.x - pad * 0.6f, y + rowHeight);
            ImGui::SetCursorScreenPos(at);
            ImGui::PushID(static_cast<int>(page.page));
            const bool clicked = ImGui::InvisibleButton("page", ImVec2(end.x - at.x, rowHeight));
            const bool hovered = ImGui::IsItemHovered();
            ImGui::PopID();
            if (clicked) settingsPage_ = page.page;
            const bool current = settingsPage_ == page.page;
            if (current) {
                draw->AddRectFilled(at, end, ImGui::GetColorU32(Fade(Accent, 0.18f)), 9 * s);
                draw->AddRectFilled(ImVec2(at.x, at.y + rowHeight * 0.25f), ImVec2(at.x + 3 * s, at.y + rowHeight * 0.75f), ImGui::GetColorU32(Accent), 2 * s);
            } else if (hovered) {
                draw->AddRectFilled(at, end, ImGui::GetColorU32(CardHover), 9 * s);
            }
            const ImU32 ink = ImGui::GetColorU32(current ? Accent : Dim);
            DrawPageGlyph(draw, page.page, ImVec2(at.x + 22 * s, at.y + rowHeight * 0.5f), 7.5f * s, ink);
            draw->AddText(ImVec2(at.x + 42 * s, at.y + (rowHeight - ImGui::GetTextLineHeight()) * 0.5f), ImGui::GetColorU32(current ? ImGuiCol_Text : ImGuiCol_Text),
                          page.title);
            y += rowHeight + 2 * s;
        }
        ImGui::EndChild();
    }

    // One width for the About page's buttons: the widest label fits.
    static float AboutButtonWidth() {
        float widest = ImGui::GetFontSize() * 9.0f;
        for (const char* label : {"Check for updates", "Launcher on GitHub", "AnyPS5 on GitHub", "Update to v00.00.00"}) {
            widest = std::max(widest, ImGui::CalcTextSize(label).x + ImGui::GetStyle().FramePadding.x * 4);
        }
        return widest;
    }

    void DrawSettingsPage(float width) {
        const auto& style = ImGui::GetStyle();
        const float s = scale_;
        const SettingsPageText* text = &SettingsPages[0];
        for (const auto& page : SettingsPages) {
            if (page.page == settingsPage_) text = &page;
        }
        ImGui::PushFont(nullptr, style.FontSizeBase * 1.7f);
        ImGui::TextUnformatted(text->title);
        ImGui::PopFont();
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored(Dim, "%s", text->note);
        ImGui::PopTextWrapPos();
        ImGui::Dummy(ImVec2(0, 10 * s));
        const float button = ImGui::GetFontSize() * 9.0f;

        switch (settingsPage_) {
        case SettingsPage::Library:
            CardBlock("folders", [&](float inner) {
                FolderField("PS5 games", "Scanned for game folders and .zar archives.", "##games", "the folder that holds your PS5 games", draft_.gamesFolder, inner);
                ImGui::Spacing();
                FolderField("PS4 games", "Optional: a second folder, for your PS4 games. Both show in the library together.", "##ps4games", "the folder that holds your PS4 games",
                            draft_.ps4GamesFolder, inner);
                ImGui::Spacing();
                FolderField("Library", "Logs and shader caches; safe to delete, it is rebuilt.", "##library", ToUtf8(launcherFolder_ / "library"), draft_.libraryFolder, inner);
                ImGui::Spacing();
                FolderField("Userdata", "Profiles, saves and trophies: back this up. Keep it on the same drive as your games, so saves move instantly for each run.",
                            "##userdata", ToUtf8(launcherFolder_ / "userdata"), draft_.userdataFolder, inner);
            });
            break;
        case SettingsPage::Games: {
            GroupLabel("Converting");
            SettingRow("versions", "AnyPS5 version", ("Converts games at Install. Now: " + CurrentVersionLabel() + ".").c_str(), button, [&] {
                if (ImGui::Button("Manage...", ImVec2(button, 0))) {
                    openVersions_ = true;
                    versionsInSettings_ = true;
                }
            });
            bool intel = UseIntelFlag(draft_, hostIntel_);
            const std::string cpu = cpuName_.empty() ? std::string(hostIntel_ ? "an Intel CPU" : "a CPU that isn't Intel") : cpuName_;
            const std::string note = "This PC has " + cpu + ". " + (hostIntel_ ? "On by default for Intel CPUs." : "Off by default; Intel CPUs need it.") +
                                     (draft_.intelFlag ? " Changed from the default." : "") + " Applies to games you install from now on.";
            SettingRow("intel", "Intel flag (--to-intel)", note.c_str(), 40.0f * s, [&] {
                if (Toggle("##intel", &intel)) draft_.intelFlag = intel == hostIntel_ ? std::nullopt : std::optional<bool>(intel);
            });
            GroupLabel("PS4 trophies");
            {
                const bool valid = draft_.ps4TrophyKey.empty() || ParseTrophyKey(draft_.ps4TrophyKey).has_value();
                const char* note = draft_.ps4TrophyKey.empty() ? "PS4 trophy lists are encrypted. Your console's trophy key (32 hex digits) opens them; the launcher doesn't include one."
                                   : valid                    ? "Set. PS4 trophy lists show their names and grades."
                                                              : "Not a key: it needs 32 hex digits (0-9, A-F).";
                const float field = ImGui::GetFontSize() * 17.0f;
                SettingRow("ps4key", "PS4 trophy key", note, field, [&] {
                    const float eye = ImGui::GetFrameHeight();
                    ImGui::SetNextItemWidth(field - eye - ImGui::GetStyle().ItemSpacing.x);
                    ImGui::InputTextWithHint("##ps4key", "32 hex digits", &draft_.ps4TrophyKey, showTrophyKey_ ? ImGuiInputTextFlags_None : ImGuiInputTextFlags_Password);
                    ImGui::SameLine();
                    if (EyeButton("##eye", showTrophyKey_)) showTrophyKey_ = !showTrophyKey_;
                });
            }
            GroupLabel("Online");
            ToggleRow("online", "Look up game updates", "Asks PROSPEROPatches.com for the selected game's region, publisher and newest update. Only the title ID is sent; answers are kept for a day.",
                      &draft_.onlineInfo);
            break;
        }
        case SettingsPage::Appearance: {
            GroupLabel("Theme");
            const float gap = 12.0f * s;
            const float cardWidth = std::min(190.0f * s, (width - gap * 2) / 3.0f);
            const ImVec2 card(cardWidth, cardWidth * 0.50f + ImGui::GetTextLineHeight() + 20 * s);
            const char* names[] = {"Follow Windows", "Dark", "Light"};
            const char* values[] = {"system", "dark", "light"};
            for (int i = 0; i < 3; ++i) {
                if (i > 0) ImGui::SameLine(0, gap);
                ImGui::PushID(i);
                if (ChoiceCard("theme", card, draft_.theme == values[i], [&](ImDrawList* draw, ImVec2 at, ImVec2 end) {
                        const ImVec2 previewAt(at.x + 10 * s, at.y + 10 * s);
                        const ImVec2 previewEnd(end.x - 10 * s, at.y + cardWidth * 0.50f);
                        if (i == 0) {
                            const float mid = (previewAt.x + previewEnd.x) * 0.5f;
                            draw->PushClipRect(previewAt, ImVec2(mid, previewEnd.y), true);
                            DrawMiniWindow(draw, previewAt, previewEnd, DarkPalette);
                            draw->PopClipRect();
                            draw->PushClipRect(ImVec2(mid, previewAt.y), previewEnd, true);
                            DrawMiniWindow(draw, previewAt, previewEnd, LightPalette);
                            draw->PopClipRect();
                        } else {
                            DrawMiniWindow(draw, previewAt, previewEnd, i == 1 ? DarkPalette : LightPalette);
                        }
                        draw->AddText(ImVec2(at.x + 12 * s, end.y - ImGui::GetTextLineHeight() - 9 * s), ImGui::GetColorU32(ImGuiCol_Text), names[i]);
                    })) {
                    draft_.theme = values[i];
                    auto& imguiStyle = ImGui::GetStyle();
                    ApplyTheme(imguiStyle, WantDark(draft_.theme));
                    imguiStyle.ScaleAllSizes(scale_);
                    imguiStyle.FontScaleDpi = scale_;
                }
                ImGui::PopID();
            }
            ImGui::Dummy(ImVec2(0, 6 * s));
            GroupLabel("Library view");
            const float viewWidth = std::min(290.0f * s, (width - gap) / 2.0f);
            const char* viewNames[] = {"Grid", "Home row"};
            const char* viewNotes[] = {"Covers in a grid, details on the side", "Big artwork, one game at a time"};
            const ImVec2 viewCard(viewWidth, ImGui::GetTextLineHeightWithSpacing() * 2 + 26 * s);
            for (int i = 0; i < 2; ++i) {
                if (i > 0) ImGui::SameLine(0, gap);
                ImGui::PushID(10 + i);
                if (ChoiceCard("view", viewCard, (draft_.view == "home") == (i == 1), [&](ImDrawList* draw, ImVec2 at, ImVec2) {
                        draw->AddText(ImVec2(at.x + 14 * s, at.y + 13 * s), ImGui::GetColorU32(ImGuiCol_Text), viewNames[i]);
                        draw->AddText(ImVec2(at.x + 14 * s, at.y + 13 * s + ImGui::GetTextLineHeightWithSpacing()), ImGui::GetColorU32(Dim), viewNotes[i]);
                    })) {
                    draft_.view = i == 1 ? "home" : "grid";
                }
                ImGui::PopID();
            }
            break;
        }
        case SettingsPage::Controls:
            GroupLabel("Keyboard and mouse");
            SettingRow("keys", "Keys for all games", "Every game of this profile starts from these. A game can change some of them for itself under Controls in its details.", button, [&] {
                if (ImGui::Button("Edit...", ImVec2(button, 0))) OpenProfileControls();
            });
            GroupLabel("Controllers");
            CardBlock("pads", [&](float) {
                ImGui::TextColored(Dim, "%s", "Pads the launcher sees now. Games read them through AnyPS5; the launcher can be used with one too (\xE2\x9C\x95 plays, \xE2\x96\xB3 opens a game's actions).");
                ImGui::Spacing();
                DrawControllers();
            });
            break;
        case SettingsPage::Saves:
            CardBlock("sync", [&](float inner) {
                ImGui::TextUnformatted("Save sync");
                ImGui::TextColored(Dim, "%s",
                                   "Through a folder your PCs all see (OneDrive, Dropbox, a USB stick, a network share). Before a game starts newer saves come in; after it ends this PC's go out. Whatever gets replaced is kept in the save backups first.");
                ImGui::Spacing();
                FolderField("Sync folder", "Empty: off.", "##sync", "e.g. C:\\Users\\you\\OneDrive\\AnyPS5 saves", draft_.syncFolder, inner);
                ImGui::Spacing();
                if (draft_.syncFolder.empty()) ImGui::TextColored(Dim, "%s", "Off");
                else ImGui::TextColored(Green, "%s", "On: saves of each profile go to <folder>\\<profile>\\<title ID>");
            });
            GroupLabel("Backup");
            CardBlock("allsaves", [&](float) {
                ImGui::TextUnformatted("Back up saves to a zip");
                ImGui::TextColored(Dim, "%s",
                                   "One zip with the saves and trophies of every game, in a folder per profile and game (<profile>\\<title ID>\\_sd, _sd_mem, _trophy), "
                                   "the same layout as a game's own backups. Keep it anywhere: another drive, a USB stick, the cloud.");
                ImGui::Spacing();
                const std::string mine = "This profile (" + layout_.profile + ")";
                const std::string all = "All profiles (" + std::to_string(profiles_.size()) + ")";
                const char* labels[] = {mine.c_str(), all.c_str()};
                int scope = backupAllProfiles_ ? 1 : 0;
                if (Segmented("backup-scope", labels, 2, scope)) backupAllProfiles_ = scope == 1;
                ImGui::Spacing();
                ImGui::BeginDisabled(job_.Busy() || session_ != nullptr || Picker().IsOpen());
                if (PrimaryButton("Back up to a zip...")) {
                    std::string day = LocalDate(UnixNow()).substr(0, 10);
                    backupPick_ = "AnyPS5 saves " + (backupAllProfiles_ ? std::string("all profiles") : layout_.profile) + " " + day + ".zip";
                    Picker().SaveFile(window_, &backupPick_, "Zip archive", "zip");
                }
                ImGui::EndDisabled();
                if (session_ != nullptr) {
                    ImGui::SameLine();
                    ImGui::AlignTextToFramePadding();
                    ImGui::TextColored(Dim, "%s", "After the game closes: its saves are with it while it runs.");
                } else if (job_.Busy() && job_.Title().rfind("Back up saves", 0) == 0) {
                    ImGui::SameLine();
                    ImGui::AlignTextToFramePadding();
                    ImGui::TextColored(Dim, "%s", "Backing up...");
                }
            });
            break;
        case SettingsPage::Playing:
            ToggleRow("hide", "Minimize the launcher", "Once the game's window shows. It comes back when the game ends.", &draft_.hideWhilePlaying);
            GroupLabel("Trophies");
            ToggleRow("cards", "Trophy cards", "A card at the top of the game's window when a trophy is earned. It never takes the keyboard or the mouse from the game.", &draft_.trophyPopups);
            ImGui::BeginDisabled(!draft_.trophyPopups);
            ToggleRow("sound", "Play a sound with it", nullptr, &draft_.trophySound);
            ImGui::EndDisabled();
            break;
        case SettingsPage::Launcher: {
            CardBlock("about", [&](float inner) {
                const ImVec2 at = ImGui::GetCursorScreenPos();
                const float logo = 52.0f * s;
                DrawLogo(ImGui::GetWindowDrawList(), at, logo);
                ImGui::SetCursorScreenPos(ImVec2(at.x + logo + 16 * s, at.y + 4 * s));
                ImGui::BeginGroup();
                ImGui::PushFont(nullptr, style.FontSizeBase * 1.3f);
                ImGui::TextUnformatted("AnyPS5 Launcher");
                ImGui::PopFont();
#ifdef LAUNCHER_VERSION
                ImGui::TextColored(Dim, "Version %s  \xC2\xB7  GPL-2.0", LAUNCHER_VERSION);
#endif
                ImGui::EndGroup();
                // The update state on the right.
                const float right = at.x + inner;
                const float buttonWidth = AboutButtonWidth();
                ImGui::SetCursorScreenPos(ImVec2(right - buttonWidth, at.y + (logo - ImGui::GetFrameHeight()) * 0.5f));
                if (launcherUpdate_) {
                    ImGui::BeginDisabled(job_.Busy() || session_ != nullptr);
                    if (PrimaryButton((LauncherUpdateReady() ? "Update to " + launcherUpdate_->release.tag : "See " + launcherUpdate_->release.tag).c_str(), ImVec2(buttonWidth, 0))) {
                        StartLauncherUpdate();
                    }
                    ImGui::EndDisabled();
                } else {
                    ImGui::BeginDisabled(launcherReleases_.Running());
                    if (ImGui::Button(launcherReleases_.Running() ? "Checking..." : "Check for updates", ImVec2(buttonWidth, 0))) CheckLauncherUpdate();
                    ImGui::EndDisabled();
                }
                ImGui::SetCursorScreenPos(ImVec2(at.x, at.y + logo + 10 * s));
                if (launcherUpdate_) ImGui::TextColored(Green, "%s is out.", launcherUpdate_->release.tag.c_str());
                else if (!launcherCheckError_.empty()) ImGui::TextColored(Amber, "%s", Elide(launcherCheckError_, inner).c_str());
                else if (launcherChecked_) ImGui::TextColored(Green, "%s", "Up to date.");
                else ImGui::TextColored(Dim, "%s", "Not checked yet.");
            });
            ToggleRow("check", "Check for updates at start", "Looks at github.com/leskal/AnyPS5-Launcher/releases and downloads a new version in the background.", &draft_.checkUpdates);
            GroupLabel("Links");
            CardBlock("links", [&](float) {
                ImGui::TextColored(Dim, "%s", "Links like anyps5://play/PPSA01234 start a game from anywhere: a shortcut, a browser, another app. Copy link is in a game's actions.");
                ImGui::Spacing();
                const ImVec2 linkButton(AboutButtonWidth(), 0);  // the size of Check for updates
                if (ImGui::Button("Launcher on GitHub", linkButton)) SDL_OpenURL(("https://github.com/" + std::string(LauncherRepository)).c_str());
                ImGui::SameLine();
                if (ImGui::Button("AnyPS5 on GitHub", linkButton)) SDL_OpenURL("https://github.com/boykopovar/AnyPS5");
            });
            break;
        }
        }
    }

    void DrawSettings() {
        if (openSettings_) {
            ImGui::OpenPopup("Settings");
            openSettings_ = false;
        }
        // Follows the launcher's window every frame: resizing or maximising it resizes
        // Settings, and the page scrolls when there isn't room.
        FollowWindow(0.8f, 0.86f);
        if (!BeginDialog("Settings")) return;
        const auto& style = ImGui::GetStyle();
        const float s = scale_;
        bool cancel = false;
        const float side = std::clamp(ImGui::GetContentRegionAvail().x * 0.24f, 190.0f * s, 250.0f * s);
        DrawSettingsNav(side);
        ImGui::SameLine(0, 22.0f * s);

        ImGui::BeginGroup();
        const float pageLeft = ImGui::GetCursorScreenPos().x;
        const float width = ImGui::GetContentRegionAvail().x;
        const float buttonHeight = ImGui::GetFrameHeight() * 1.3f;
        const float footer = buttonHeight + style.ItemSpacing.y * 3 + 4.0f + (saveError_.empty() ? 0.0f : ImGui::GetTextLineHeightWithSpacing());
        ImGui::BeginChild("settings-page", ImVec2(width, -footer), ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
        DrawSettingsPage(ImGui::GetContentRegionAvail().x - style.ScrollbarSize * 0.5f);
        ImGui::EndChild();

        if (!saveError_.empty()) ImGui::TextColored(Red, "%s", Elide(saveError_, width).c_str());
        ImGui::Spacing();
        const float buttonWidth = ImGui::GetFontSize() * 6.5f;
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(Dim, "%s", "Changes take effect when you save.");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Saved in %s", ToUtf8(settingsFile_).c_str());
        ImGui::SameLine();
        ImGui::SetCursorScreenPos(ImVec2(pageLeft + width - buttonWidth * 2 - style.ItemSpacing.x, ImGui::GetCursorScreenPos().y));
        ImGui::BeginDisabled(Picker().IsOpen());
        if (ImGui::Button("Cancel", ImVec2(buttonWidth, buttonHeight)) || (ImGui::IsKeyPressed(ImGuiKey_Escape) && !ImGui::IsAnyItemActive())) cancel = true;
        ImGui::SameLine();
        if (PrimaryButton("Save", ImVec2(buttonWidth, buttonHeight))) {
            try {
                const auto library = EffectiveLibrary(draft_, launcherFolder_);
                const auto userdata = EffectiveUserdata(draft_, launcherFolder_);
                for (const auto& games : GameFolders(draft_)) {
                    for (const auto& [label, folder] : {std::pair{"library", library}, std::pair{"userdata", userdata}}) {
                        const auto relative = folder.lexically_normal().lexically_relative(games);
                        if (!relative.empty() && ToUtf8(*relative.begin()) != "..") {
                            throw std::runtime_error(std::string("the ") + label + " folder is inside a games folder (" + ToUtf8(games) + "); choose another place");
                        }
                    }
                }
                // The AnyPS5 version is chosen in "AnyPS5 versions", not here: keep whatever is current.
                draft_.anyps5Folder = settings_.anyps5Folder;
                draft_.anyps5Version = settings_.anyps5Version;
                SaveSettings(settingsFile_, draft_);
                const bool gamesChanged = draft_.gamesFolder != settings_.gamesFolder || draft_.ps4GamesFolder != settings_.ps4GamesFolder;
                if (draft_.ps4TrophyKey != settings_.ps4TrophyKey) {
                    trophies_.clear();  // read the lists again with the new key
                    trophyAsked_.clear();
                }
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
        ImGui::EndGroup();
        if (controlsScope_ == 1) DrawControls();
        // Opened from the Games page: on top of Settings, so the unsaved changes stay.
        if (versionsInSettings_) {
            DrawVersions();
            if (!openVersions_ && !ImGui::IsPopupOpen("AnyPS5 versions")) versionsInSettings_ = false;
        }
        ImGui::EndPopup();
    }

    SDL_Window* window_;
    std::unique_ptr<IconCache> icons_;
    float scale_;
    std::filesystem::path launcherFolder_;
    std::filesystem::path settingsFile_;
    Settings settings_;
    const bool hostIntel_ = HostIsIntel();
    const std::string cpuName_ = HostCpuName();
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
    std::string renamingVersion_;  // the local version being named in AnyPS5 versions
    std::string renameText_;
    bool renameFocus_ = false;
    SettingsPage settingsPage_ = SettingsPage::Library;
    bool versionsInSettings_ = false;
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
    std::map<std::string, RunDiagnosis> diagnoses_;
    mutable std::map<std::string, AnyPS5State> pinnedStates_;  // versions games are pinned to, by id
    std::vector<std::string> reinstallQueue_;                  // game locations, installed one after another
    std::string pendingPlay_;                                  // a game waiting for its saves to sync
    std::optional<LaunchRequest> pendingRequest_;              // from a link, handled once the games are scanned
    Background<ReleaseList> launcherReleases_;
    std::optional<LauncherUpdate> launcherUpdate_;
    Background<StagedLauncher> launcherStager_;
    std::shared_ptr<JobControl> stageControl_;      // the background download's progress
    std::optional<StagedLauncher> stagedLauncher_;  // ready, or failed
    bool updateApplied_ = false;                    // the new exe is in place
    bool openLauncherUpdate_ = false;
    bool quitRequested_ = false;
    bool launcherChecked_ = false;
    bool openSetup_ = false;
    int setupStep_ = 0;
    std::string setupProfile_;
    std::string setupError_;
    bool setupCanGoOn_ = true;
    Background<int> setupCounter_;     // games found in the chosen folder; -1: can't read it
    std::string setupCounted_;         // the folder it counted
    std::optional<int> setupGameCount_;
    std::string setupSeen_;            // the path as last typed, and when
    std::uint64_t setupSeenAt_ = 0;
    std::string launcherCheckError_;
    Uint64 nextRequestCheck_ = 0;  // the last run of each game, by serial
    // Game settings window.
    bool openGameSettings_ = false;
    std::string settingsSerial_;
    std::string settingsName_;
    GameSettings gameSettings_;
    std::vector<SaveBackup> backups_;
    std::string gameSettingsError_;
    std::string gamePin_;
    std::string cacheSize_;  // read in the background when the window opens
    bool hasSaves_ = false;
    bool factsReady_ = false;
    bool factsWanted_ = false;
    Background<GameFacts> factsLoader_;
    std::string exportPick_;
    std::string backupPick_;          // where Back up to a zip writes
    bool backupAllProfiles_ = false;
    bool showTrophyKey_ = false;
    std::string importPick_;
    // Screenshots window.
    bool openScreenshots_ = false;
    std::vector<std::filesystem::path> shots_;
    std::set<std::filesystem::path> shotLoaded_;
    int shotViewed_ = -1;
    // The launch screen, until the game's window shows.
    bool launching_ = false;
    Uint64 launchStarted_ = 0;
    bool minimizedForGame_ = false;
    // Controllers in Settings.
    std::map<SDL_JoystickID, SDL_Gamepad*> pads_;
    SDL_JoystickID padTest_ = 0;
    bool padTestDrawn_ = false;  // the test was on screen in the last frame
    Background<OnlineLookup> onlineLoader_;
    std::map<std::string, TitleOnlineInfo> online_;
    std::map<std::string, std::string> onlineErrors_;
    std::set<std::string> onlineAsked_;  // titles looked up (or being looked up) this session
    std::string onlineRefresh_;          // a title whose cached answer should be ignored
    Background<ScanResult> scan_;
    BackgroundJob job_;
    std::string jobSerial_;
    std::string jobLocation_;  // the copy the job works on; empty: every copy of jobSerial_
    std::unique_ptr<GameSession> session_;
    std::string sessionLocation_;  // the copy that runs (its location), as two copies share a title ID
    Playtimes playtimes_;           // the current profile's
    std::uint64_t sessionStartedAt_ = 0;  // SDL ticks
    std::uint64_t windowSeenAt_ = 0;      // 0: no window yet
    std::uint64_t windowCheckedAt_ = 0;
    std::uint64_t countedUntil_ = 0;      // counted up to here; 0: nothing yet
    bool sessionCounted_ = false;         // this session is in the session count
    ScanResult result_;
    bool scanned_ = false;
    bool rescanWanted_ = false;
    std::optional<Message> message_;
};

}

int main(int argc, char** argv) {
    const std::vector<std::string> arguments(argv, argv + argc);
    const auto request = launcher::ParseArguments(arguments);
#ifdef _WIN32
    {
        // One launcher per folder: a second one (from a link) hands its request over and leaves.
        const char* base = SDL_GetBasePath();
        const std::string folder = base != nullptr ? base : "";
        const auto name = L"AnyPS5Launcher-" + std::to_wstring(std::hash<std::string>{}(folder));
        // Started by a self-update: the old launcher is still closing, so wait for it
        // rather than handing over to it and leaving with nothing running.
        const bool updated = std::find(arguments.begin(), arguments.end(), "--updated") != arguments.end();
        if (!launcher::win::FirstInstance(name.c_str(), updated ? 20000 : 0)) {
            if (request) {
                std::ofstream(launcher::FromUtf8(folder) / "anyps5-launcher.request", std::ios::binary | std::ios::trunc) << launcher::FormatRequest(*request);
            }
            launcher::win::ShowWindowTitled(L"AnyPS5 Launcher");
            return 0;
        }
    }
#endif
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
    // Rounded pictures are drawn with an anti-aliased edge whose texture coordinates go
    // a hair past 0..1; the default (wrap there) would paint the opposite edge as a thin line.
    SDL_SetRenderTextureAddressMode(renderer, SDL_TEXTURE_ADDRESS_CLAMP, SDL_TEXTURE_ADDRESS_CLAMP);
    // After an update the new launcher opens where the old one was.
    if (const auto placement = launcher::ParseWindowArgument(arguments)) {
        SDL_SetWindowSize(window, placement->width, placement->height);
        SDL_SetWindowPosition(window, placement->x, placement->y);
        if (placement->maximized) SDL_MaximizeWindow(window);
    } else {
        SDL_SetWindowPosition(window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    }
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
    LoadPlatformFont(io);  // the main window's atlas only; the trophy card doesn't show it
    ImGui_ImplSDL3_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer3_Init(renderer);

    {
        LauncherApp app(window, renderer, scale);
        app.SetStartRequest(request);
        if (std::find(arguments.begin(), arguments.end(), "--updated") != arguments.end()) app.SetJustUpdated();
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
            if (app.QuitRequested()) done = true;
            if (SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED) {
                SDL_Delay(10);
                continue;
            }
            ImGui_ImplSDLRenderer3_NewFrame();
            ImGui_ImplSDL3_NewFrame();
            app.BeforeFrame();
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
