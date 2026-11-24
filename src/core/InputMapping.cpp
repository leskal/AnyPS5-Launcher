#include "core/InputMapping.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <system_error>

#include "core/FileOps.hpp"

namespace launcher {

namespace {

std::string Lower(std::string_view text) {
    std::string out(text);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c); });
    return out;
}

std::string_view Trim(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t' || text.front() == '\r' || text.front() == '\n')) text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r' || text.back() == '\n')) text.remove_suffix(1);
    return text;
}

// "key : Left Shift" -> "KEY:Left Shift".
std::string NormalizeSource(std::string_view source) {
    const auto colon = source.find(':');
    if (colon == std::string_view::npos) return std::string(Trim(source));
    std::string type(Trim(source.substr(0, colon)));
    std::transform(type.begin(), type.end(), type.begin(), [](unsigned char c) { return static_cast<char>(c >= 'a' && c <= 'z' ? c - 'a' + 'A' : c); });
    return type + ":" + std::string(Trim(source.substr(colon + 1)));
}

}

const std::vector<InputActionInfo>& InputActions() {
    static const std::vector<InputActionInfo> actions{
        {"Cross", "Buttons", "Cross", {"KEY:Return", "KEY:Space"}, false},
        {"Circle", "Buttons", "Circle", {"KEY:C"}, false},
        {"Square", "Buttons", "Square", {"MOUSE:Left"}, false},
        {"Triangle", "Buttons", "Triangle", {"KEY:I"}, false},
        {"L1", "Buttons", "L1", {"KEY:Q"}, false},
        {"R1", "Buttons", "R1", {"KEY:E", "KEY:Left Alt", "KEY:Right Alt"}, false},
        {"L2", "Buttons", "L2", {"KEY:R", "MOUSE:X1"}, false},
        {"R2", "Buttons", "R2", {"MOUSE:Right"}, false},
        {"L3", "Buttons", "L3", {"KEY:Left Shift", "KEY:Right Shift"}, false},
        {"R3", "Buttons", "R3", {"KEY:Left Ctrl", "KEY:Right Ctrl"}, false},
        {"Options", "Buttons", "Options", {"KEY:Escape"}, false},
        {"Up", "D-pad", "Up", {"KEY:Up", "WHEEL:Up"}, false},
        {"Down", "D-pad", "Down", {"KEY:Down", "WHEEL:Down"}, false},
        {"Left", "D-pad", "Left", {"KEY:Left"}, false},
        {"Right", "D-pad", "Right", {"KEY:Right"}, false},
        {"LeftStickUp", "Left stick", "Up", {"KEY:W"}, false},
        {"LeftStickDown", "Left stick", "Down", {"KEY:S"}, false},
        {"LeftStickLeft", "Left stick", "Left", {"KEY:A"}, false},
        {"LeftStickRight", "Left stick", "Right", {"KEY:D"}, false},
        {"RightStickUp", "Right stick", "Up", {"KEY:T"}, false},
        {"RightStickDown", "Right stick", "Down", {"KEY:G"}, false},
        {"RightStickLeft", "Right stick", "Left", {"KEY:F"}, false},
        {"RightStickRight", "Right stick", "Right", {"KEY:H"}, false},
        {"TouchLeft", "Touchpad and motion", "Touchpad left", {"KEY:Backspace"}, false},
        {"TouchRight", "Touchpad and motion", "Touchpad right", {"KEY:Tab"}, false},
        {"Shake", "Touchpad and motion", "Shake", {"KEY:X"}, false},
        {"StickModifier", "Touchpad and motion", "Slow stick (held)", {}, false},
        {"ToggleMouse", "AnyPS5", "Mouse moves the stick", {"MOUSE:Middle", "KEY:F7"}, false},
        {"ToggleFullscreen", "AnyPS5", "Full screen", {"KEY:F11"}, true},
        {"TogglePerformanceOverlay", "AnyPS5", "Performance overlay", {"KEY:F10"}, true},
        {"Screenshot", "AnyPS5", "Screenshot", {"KEY:F12"}, true},
        {"ShowPadStatus", "AnyPS5", "Controller status", {"KEY:F9"}, true},
        {"VolumeDown", "AnyPS5", "Volume down", {"KEY:F5"}, true},
        {"VolumeUp", "AnyPS5", "Volume up", {"KEY:F6"}, true},
        {"ToggleMute", "AnyPS5", "Mute", {"KEY:F8"}, true},
        {"ReloadInputMapping", "AnyPS5", "Reload these controls", {"KEY:F1"}, true},
    };
    return actions;
}

const InputActionInfo* FindInputAction(std::string_view name) {
    const auto wanted = Lower(Trim(name));
    for (const auto& action : InputActions()) {
        if (Lower(action.name) == wanted) return &action;
    }
    return nullptr;
}

const std::vector<InputSettingInfo>& InputSettings() {
    static const std::vector<InputSettingInfo> settings{
        {"LeftStickDeadzone", "Left stick dead zone (%)", "How much of a controller stick's travel reads as centered. Raise it for a drifting stick.", 0, 90, 0, true, {}},
        {"RightStickDeadzone", "Right stick dead zone (%)", "The same for the right stick.", 0, 90, 0, true, {}},
        {"MouseSensitivity", "Mouse sensitivity", "How fast the mouse turns the stick while \"Mouse moves the stick\" is on.", 0.05, 10, 1, false, {}},
        {"MouseVerticalScale", "Mouse vertical speed", "Multiplies up and down mouse movement, for games that pitch slower or faster than they turn.", 0.1, 10, 1, false, {}},
        {"MouseCurve", "Mouse response curve", "Above 1 gives finer control at low speed; below 1 reaches large tilts sooner.", 0.2, 5, 1, false, {}},
        {"MouseDeadzoneOffset", "Mouse minimum tilt", "The smallest tilt any mouse movement gives. Set it just above the game's own dead zone.", 0, 0.9, 0.2, false, {}},
        {"MouseMaxSpeed", "Mouse maximum speed", "The fastest tracked speed, as a multiple of full tilt.", 0.1, 10, 1, false, {}},
        {"MouseSmoothingMs", "Mouse smoothing (ms)", "0 follows every mouse report directly.", 0, 500, 10, true, {}},
        {"MouseDecayMs", "Mouse stop delay (ms)", "How soon the stick centers after the mouse stops.", 1, 1000, 30, true, {}},
        {"MouseStickTarget", "Mouse drives", "Which stick the mouse moves.", 0, 0, 0, false, {"Right", "Left"}},
        {"StickModifierScale", "Slow stick tilt", "How far keyboard stick keys tilt while \"Slow stick\" is held.", 0.05, 1, 0.5, false, {}},
        {"KeyboardRampMs", "Keyboard stick ramp (ms)", "How long a key takes to tilt a stick fully. 0 tilts at once.", 0, 2000, 0, true, {}},
        {"TriggerRampMs", "Trigger ramp (ms)", "How long a key takes to press L2 or R2 fully, for racing games.", 0, 2000, 0, true, {}},
    };
    return settings;
}

const InputSettingInfo* FindInputSetting(std::string_view name) {
    const auto wanted = Lower(Trim(name));
    for (const auto& setting : InputSettings()) {
        if (Lower(setting.name) == wanted) return &setting;
    }
    return nullptr;
}

InputFile ParseInputFile(std::string_view text) {
    InputFile file;
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF && static_cast<unsigned char>(text[1]) == 0xBB && static_cast<unsigned char>(text[2]) == 0xBF) {
        text.remove_prefix(3);
    }
    bool header = true;
    std::size_t start = 0;
    while (start < text.size()) {
        auto end = text.find('\n', start);
        if (end == std::string_view::npos) end = text.size();
        const auto raw = text.substr(start, end - start);
        start = end + 1;
        const auto line = Trim(raw);
        if (line.empty()) continue;
        if (line.front() == '#' || line.front() == ';') {
            if (!header) file.other.emplace_back(line);
            continue;
        }
        header = false;
        auto content = line;
        if (const auto comment = content.find_first_of("#;"); comment != std::string_view::npos) content = Trim(content.substr(0, comment));
        const auto equals = content.find('=');
        if (equals == std::string_view::npos) {
            file.other.emplace_back(line);
            continue;
        }
        const auto key = Trim(content.substr(0, equals));
        const auto value = Trim(content.substr(equals + 1));
        if (const auto* action = FindInputAction(key); action != nullptr && !value.empty()) {
            auto& sources = file.bindings[action->name];
            const auto source = NormalizeSource(value);
            if (std::none_of(sources.begin(), sources.end(), [&](const std::string& kept) { return Lower(kept) == Lower(source); })) sources.push_back(source);
        } else if (const auto* setting = FindInputSetting(key); setting != nullptr && !value.empty()) {
            file.settings[setting->name] = std::string(value);
        } else {
            file.other.emplace_back(line);
        }
    }
    return file;
}

std::string FormatInputFile(const InputFile& file, std::string_view header) {
    std::string text(header);
    if (!text.empty() && text.back() != '\n') text += '\n';
    if (!file.settings.empty()) {
        text += "\n";
        for (const auto& setting : InputSettings()) {
            if (const auto found = file.settings.find(setting.name); found != file.settings.end()) text += std::string(setting.name) + " = " + found->second + "\n";
        }
    }
    if (!file.bindings.empty()) {
        text += "\n";
        for (const auto& action : InputActions()) {
            const auto found = file.bindings.find(action.name);
            // An action with no source keeps AnyPS5's keys: the file has no way to say "none".
            if (found == file.bindings.end()) continue;
            for (const auto& source : found->second) text += std::string(action.name) + " = " + source + "\n";
        }
    }
    if (!file.other.empty()) {
        text += "\n";
        for (const auto& line : file.other) text += line + "\n";
    }
    return text;
}

InputFile ReadInputFile(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) return {};
    const std::string text{std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
    return ParseInputFile(text);
}

void WriteInputFile(const std::filesystem::path& path, const InputFile& file, std::string_view header) {
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    WriteFileAtomically(path, FormatInputFile(file, header));
}

std::vector<std::string> EffectiveBindings(const InputActionInfo& action, const InputFile& profile, const InputFile& game) {
    if (const auto found = game.bindings.find(action.name); found != game.bindings.end()) return found->second;
    if (const auto found = profile.bindings.find(action.name); found != profile.bindings.end()) return found->second;
    return action.defaults;
}

std::string DescribeInputSource(std::string_view source) {
    const auto colon = source.find(':');
    if (colon == std::string_view::npos) return std::string(source);
    const auto type = Lower(source.substr(0, colon));
    const std::string value(source.substr(colon + 1));
    const auto lower = Lower(value);
    if (type == "key") return value;
    if (type == "mouse") {
        if (lower == "left") return "Left click";
        if (lower == "right") return "Right click";
        if (lower == "middle") return "Middle click";
        if (lower == "x1") return "Mouse 4";
        if (lower == "x2") return "Mouse 5";
        return "Mouse " + value;
    }
    if (type == "wheel") return "Wheel " + lower;
    if (type == "pad") return "Pad " + value;
    return std::string(source);
}

std::map<std::string, std::vector<std::string>> InputConflicts(const InputFile& profile, const InputFile& game) {
    std::map<std::string, std::vector<std::string>> users;
    for (const auto& action : InputActions()) {
        for (const auto& source : EffectiveBindings(action, profile, game)) {
            users[Lower(source)].push_back(action.name);
        }
    }
    std::map<std::string, std::vector<std::string>> conflicts;
    for (auto& [source, actions] : users) {
        if (actions.size() > 1) conflicts.emplace(source, std::move(actions));
    }
    return conflicts;
}

std::string FormatSettingValue(const InputSettingInfo& setting, double value) {
    value = std::clamp(value, setting.minimum, setting.maximum);
    if (setting.integer) return std::to_string(static_cast<long long>(std::lround(value)));
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%.2f", value);
    std::string text(buffer);
    while (!text.empty() && text.back() == '0') text.pop_back();
    if (!text.empty() && text.back() == '.') text.pop_back();
    return text;
}

}
