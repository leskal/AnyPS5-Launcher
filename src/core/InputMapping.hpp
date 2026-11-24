#pragma once

#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace launcher {

// AnyPS5's anyps5-input.ini (docs/user/INPUT_MAPPING.md in AnyPS5): lines of
// "Action = TYPE:Value" (KEY:<SDL key name>, MOUSE:Left|Middle|Right|X1|X2,
// WHEEL:Up|Down|Left|Right, PAD:<button>+...) and "Setting = value". The first
// line of an action replaces AnyPS5's built-in keys for it; later lines add to it.
// The profile's file is read first, then anyps5-input.<serial>.ini beside it,
// which overrides only what it lists.

struct InputActionInfo {
    const char* name;   // as AnyPS5 spells it
    const char* group;  // "Buttons", "D-pad", "Left stick", ...
    const char* label;  // for people
    std::vector<std::string> defaults;
    bool keysOnly = false;  // emulator hotkeys take KEY: (and PAD:) only
};

const std::vector<InputActionInfo>& InputActions();
const InputActionInfo* FindInputAction(std::string_view name);  // case-insensitive

struct InputSettingInfo {
    const char* name;
    const char* label;
    const char* note;
    double minimum = 0;
    double maximum = 0;
    double fallback = 0;  // AnyPS5's default
    bool integer = false;
    std::vector<std::string> choices;  // non-empty: one of these instead of a number
};

const std::vector<InputSettingInfo>& InputSettings();
const InputSettingInfo* FindInputSetting(std::string_view name);

struct InputFile {
    std::map<std::string, std::vector<std::string>> bindings;  // action -> sources; only actions the file lists
    std::map<std::string, std::string> settings;               // setting -> value
    std::vector<std::string> other;                            // lines kept as written (SDLButton:, ToggleActions, comments, ...)

    bool Empty() const { return bindings.empty() && settings.empty() && other.empty(); }
};

// Reads the file's text. Header comments (before the first entry) are dropped;
// they are written again by FormatInputFile.
InputFile ParseInputFile(std::string_view text);
std::string FormatInputFile(const InputFile& file, std::string_view header);

InputFile ReadInputFile(const std::filesystem::path& path);  // empty when missing
void WriteInputFile(const std::filesystem::path& path, const InputFile& file, std::string_view header);

// What an action does after the built-in keys, the profile file and the game file.
std::vector<std::string> EffectiveBindings(const InputActionInfo& action, const InputFile& profile, const InputFile& game);

// "KEY:Left Shift" -> "Left Shift", "MOUSE:X1" -> "Mouse 4", "WHEEL:Up" -> "Wheel up".
std::string DescribeInputSource(std::string_view source);

// Sources used by more than one action (case-insensitive), with the actions using each.
std::map<std::string, std::vector<std::string>> InputConflicts(const InputFile& profile, const InputFile& game);

// Settings: "12", "2.5"; trailing zeros trimmed.
std::string FormatSettingValue(const InputSettingInfo& setting, double value);

}
