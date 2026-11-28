#include "TextScan.hpp"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>

namespace texts {

namespace {

bool IdentifierChar(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

int HexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Skips spaces, newlines and comments.
std::size_t SkipSpace(std::string_view source, std::size_t at) {
    while (at < source.size()) {
        const char c = source[at];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            ++at;
        } else if (source.substr(at, 2) == "//") {
            const auto end = source.find('\n', at);
            at = end == std::string_view::npos ? source.size() : end;
        } else if (source.substr(at, 2) == "/*") {
            const auto end = source.find("*/", at + 2);
            at = end == std::string_view::npos ? source.size() : end + 2;
        } else {
            break;
        }
    }
    return at;
}

// Reads the literal starting at the quote at `at`; returns its text and moves `at` past it.
std::string ReadLiteral(std::string_view source, std::size_t& at) {
    std::string text;
    ++at;
    while (at < source.size() && source[at] != '"') {
        char c = source[at++];
        if (c != '\\' || at >= source.size()) {
            text += c;
            continue;
        }
        c = source[at++];
        switch (c) {
        case 'n': text += '\n'; break;
        case 't': text += '\t'; break;
        case 'r': text += '\r'; break;
        case '0': case '1': case '2': case '3': case '4': case '5': case '6': case '7': {
            int value = c - '0';
            for (int i = 0; i < 2 && at < source.size() && source[at] >= '0' && source[at] <= '7'; ++i) value = value * 8 + (source[at++] - '0');
            text += static_cast<char>(value);
            break;
        }
        case 'x': {
            int value = 0;
            while (at < source.size() && HexValue(source[at]) >= 0) value = value * 16 + HexValue(source[at++]);
            text += static_cast<char>(value);
            break;
        }
        default: text += c; break;  // \" \\ \'
        }
    }
    ++at;  // the closing quote
    return text;
}

// Moves past a literal that isn't a text: "...", '...' or R"x(...)x".
std::size_t SkipLiteral(std::string_view source, std::size_t at) {
    if (source[at] == 'R' && at + 1 < source.size() && source[at + 1] == '"') {
        const auto open = source.find('(', at + 2);
        if (open == std::string_view::npos) return source.size();
        const std::string close = ")" + std::string(source.substr(at + 2, open - at - 2)) + "\"";
        const auto end = source.find(close, open);
        return end == std::string_view::npos ? source.size() : end + close.size();
    }
    const char quote = source[at++];
    while (at < source.size() && source[at] != quote) at += source[at] == '\\' ? 2 : 1;
    return at + 1;
}

// Has a printf directive (%s, %d, %.1f, %%), so translations must keep the same ones.
bool PrintfLike(std::string_view text) {
    for (std::size_t at = text.find('%'); at != std::string_view::npos; at = text.find('%', at + 1)) {
        auto next = at + 1;
        while (next < text.size() && std::string_view("-+ #0123456789.").find(text[next]) != std::string_view::npos) ++next;
        if (next < text.size() && std::string_view("sdiufFxXcgGeEp%").find(text[next]) != std::string_view::npos) return true;
    }
    return false;
}

std::string Escape(std::string_view text) {
    std::string out;
    for (const char c : text) {
        switch (c) {
        case '\\': out += "\\\\"; break;
        case '"': out += "\\\""; break;
        case '\n': out += "\\n"; break;
        case '\t': out += "\\t"; break;
        case '\r': out += "\\r"; break;
        default: out += c; break;
        }
    }
    return out;
}

// `keyword "text"`, split after each \n so long texts read well.
std::string PoString(std::string_view keyword, std::string_view text) {
    std::vector<std::string_view> lines;
    for (std::size_t at = 0; at < text.size();) {
        const auto end = text.find('\n', at);
        const auto next = end == std::string_view::npos ? text.size() : end + 1;
        lines.push_back(text.substr(at, next - at));
        at = next;
    }
    std::string out(keyword);
    if (lines.size() <= 1) return out + " \"" + Escape(text) + "\"\n";
    out += " \"\"\n";
    for (const auto line : lines) out += "\"" + Escape(line) + "\"\n";
    return out;
}

}

std::vector<std::string> FindTexts(std::string_view source) {
    std::vector<std::string> found;
    std::size_t at = 0;
    while (at < source.size()) {
        const char c = source[at];
        if (source.substr(at, 2) == "//" || source.substr(at, 2) == "/*") {
            at = SkipSpace(source, at);
            continue;
        }
        if (c == '"' || c == '\'' || (c == 'R' && at + 1 < source.size() && source[at + 1] == '"' && (at == 0 || !IdentifierChar(source[at - 1])))) {
            at = SkipLiteral(source, at);
            continue;
        }
        if (!IdentifierChar(c)) {
            ++at;
            continue;
        }
        const auto start = at;
        while (at < source.size() && IdentifierChar(source[at])) ++at;
        const auto word = source.substr(start, at - start);
        if (word != "T" && word != "N") continue;
        auto next = SkipSpace(source, at);
        if (next >= source.size() || source[next] != '(') continue;
        next = SkipSpace(source, next + 1);
        if (next >= source.size() || source[next] != '"') continue;
        std::string text;
        while (next < source.size() && source[next] == '"') {
            text += ReadLiteral(source, next);
            next = SkipSpace(source, next);
        }
        if (next < source.size() && source[next] == ')') found.push_back(std::move(text));
        at = next;
    }
    return found;
}

std::vector<Text> CollectTexts(const std::filesystem::path& root) {
    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(root / "src")) {
        const auto extension = entry.path().extension();
        if (entry.is_regular_file() && (extension == ".cpp" || extension == ".hpp")) files.push_back(entry.path());
    }
    std::sort(files.begin(), files.end());
    std::vector<Text> texts;
    std::map<std::string, std::size_t> index;
    for (const auto& file : files) {
        std::ifstream stream(file, std::ios::binary);
        const std::string source((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
        const auto name = file.lexically_relative(root).generic_string();
        for (auto english : FindTexts(source)) {
            // ImGui's "Label##id": T() translates the label alone, so that is the text.
            if (const auto id = english.find("##"); id != std::string::npos) english.resize(id);
            // Only texts with words: "%llu/%llu" or ")" read the same in every language.
            if (std::none_of(english.begin(), english.end(), [](char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); })) continue;
            auto [found, added] = index.emplace(english, texts.size());
            if (added) texts.push_back({english, {}});
            auto& where = texts[found->second].files;
            if (std::find(where.begin(), where.end(), name) == where.end()) where.push_back(name);
        }
    }
    return texts;
}

std::string MakePot(const std::vector<Text>& texts) {
    std::string out =
        "# The texts of AnyPS5 Launcher, in English. Made from the T(\"...\") and N(\"...\")\n"
        "# texts in src by the update-texts build target; don't edit it by hand.\n"
        "# Translations are lang/<code>.po files made from it.\n"
        "msgid \"\"\n"
        "msgstr \"\"\n"
        "\"Project-Id-Version: AnyPS5 Launcher\\n\"\n"
        "\"MIME-Version: 1.0\\n\"\n"
        "\"Content-Type: text/plain; charset=UTF-8\\n\"\n"
        "\"Content-Transfer-Encoding: 8bit\\n\"\n"
        "\n"
        "#. The name of this language, written in it: Ελληνικά, Español, Deutsch.\n"
        "msgctxt \"language name\"\n"
        "msgid \"English\"\n"
        "msgstr \"\"\n";
    for (const auto& text : texts) {
        out += "\n";
        // Notes for translators: what the placeholders and the ImGui ids are.
        if (text.english.find("{0}") != std::string::npos) out += "#. {0}, {1}... are filled in by the launcher; keep them, in any order.\n";
        if (PrintfLike(text.english)) out += "#, c-format\n";
        out += "#:";
        for (const auto& file : text.files) out += " " + file;
        out += "\n";
        out += PoString("msgid", text.english);
        out += "msgstr \"\"\n";
    }
    return out;
}

}
