#include "core/Http.hpp"

#include <cstdio>
#include <cwchar>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <system_error>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winhttp.h>
#endif

#include "core/FileOps.hpp"
#include "core/Utf8.hpp"

namespace launcher {

namespace {

constexpr const char* UserAgent = "AnyPS5-Launcher";
constexpr std::size_t MaxText = 8u << 20;

bool IsGitHubApi(const std::string& url) {
    return url.rfind("https://api.github.com/", 0) == 0;
}

#ifdef _WIN32

std::wstring Wide(const std::string& text) {
    if (text.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), size);
    return result;
}

[[noreturn]] void Fail(const std::string& what) {
    throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), what);
}

struct Handle {
    HINTERNET value = nullptr;
    ~Handle() {
        if (value != nullptr) WinHttpCloseHandle(value);
    }
};

// Calls sink with each block of the body; returns the HTTP status.
template <typename Sink>
DWORD Send(const wchar_t* method, const std::string& url, const std::string& body, const std::string& contentType, JobControl* control, Sink sink) {
    const auto wideUrl = Wide(url);
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    wchar_t host[256]{};
    wchar_t path[4096]{};
    parts.lpszHostName = host;
    parts.dwHostNameLength = static_cast<DWORD>(std::size(host));
    parts.lpszUrlPath = path;
    parts.dwUrlPathLength = static_cast<DWORD>(std::size(path));
    wchar_t extra[4096]{};
    parts.lpszExtraInfo = extra;
    parts.dwExtraInfoLength = static_cast<DWORD>(std::size(extra));
    if (!WinHttpCrackUrl(wideUrl.c_str(), 0, 0, &parts)) Fail("bad URL " + url);
    if (parts.nScheme != INTERNET_SCHEME_HTTPS) throw std::runtime_error("only https downloads are allowed: " + url);

    Handle session{WinHttpOpen(Wide(UserAgent).c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0)};
    if (session.value == nullptr) {
        session.value = WinHttpOpen(Wide(UserAgent).c_str(), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (session.value == nullptr) Fail("cannot start WinHTTP");
    }
    // A server that stops answering must not hold the launcher (or its exit) for long.
    WinHttpSetTimeouts(session.value, 15000, 15000, 30000, 60000);
    Handle connection{WinHttpConnect(session.value, host, parts.nPort, 0)};
    if (connection.value == nullptr) Fail("cannot connect to " + url);
    const std::wstring target = std::wstring(path) + extra;
    Handle request{WinHttpOpenRequest(connection.value, method, target.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)};
    if (request.value == nullptr) Fail("cannot request " + url);
    std::wstring headers = IsGitHubApi(url) ? L"Accept: application/vnd.github+json\r\nX-GitHub-Api-Version: 2022-11-28\r\n" : L"";
    if (!contentType.empty()) headers += L"Content-Type: " + Wide(contentType) + L"\r\nAccept: application/json\r\n";
    void* data = body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(body.data());
    const auto bodySize = static_cast<DWORD>(body.size());
    if (!WinHttpSendRequest(request.value, headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(), headers.empty() ? 0 : static_cast<DWORD>(-1L),
                            data, bodySize, bodySize, 0)) {
        Fail("cannot reach " + url);
    }
    if (!WinHttpReceiveResponse(request.value, nullptr)) Fail("no answer from " + url);
    DWORD status = 0;
    DWORD size = sizeof(status);
    WinHttpQueryHeaders(request.value, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
    if (status < 200 || status >= 300) return status;
    if (control != nullptr) {
        wchar_t length[32]{};
        DWORD bytes = sizeof(length);
        if (WinHttpQueryHeaders(request.value, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX, length, &bytes, WINHTTP_NO_HEADER_INDEX)) {
            if (control->Total() == 0) control->SetTotal(std::wcstoull(length, nullptr, 10));
        }
    }
    std::vector<char> buffer(1u << 16);
    for (;;) {
        if (control != nullptr) ThrowIfCancelled(*control);
        DWORD read = 0;
        if (!WinHttpReadData(request.value, buffer.data(), static_cast<DWORD>(buffer.size()), &read)) Fail("download of " + url + " broke off");
        if (read == 0) break;
        sink(buffer.data(), static_cast<std::size_t>(read));
        if (control != nullptr) control->Advance(read);
    }
    return status;
}

template <typename Sink>
DWORD Get(const std::string& url, JobControl* control, Sink sink) {
    return Send(L"GET", url, {}, {}, control, sink);
}

#else

// Development hosts: curl does the transfer.
std::string Quote(const std::string& text) {
    std::string quoted = "'";
    for (const char c : text) quoted += c == '\'' ? std::string("'\\''") : std::string(1, c);
    return quoted + "'";
}

void Curl(const std::string& url, const std::filesystem::path& file, const std::string& body = {}, const std::string& contentType = {}) {
    if (url.rfind("https://", 0) != 0) throw std::runtime_error("only https downloads are allowed: " + url);
    std::string command = "curl -fsSL --proto =https -A " + Quote(UserAgent);
    if (IsGitHubApi(url)) command += " -H " + Quote("Accept: application/vnd.github+json");
    if (!contentType.empty()) command += " -H " + Quote("Content-Type: " + contentType) + " -H " + Quote("Accept: application/json") + " --data-binary " + Quote(body);
    command += " -o " + Quote(ToUtf8(file)) + " " + Quote(url);
    const int code = std::system(command.c_str());
    if (code != 0) throw std::runtime_error("download of " + url + " failed (curl exit status " + std::to_string(code) + ")");
}

#endif

#ifdef _WIN32
std::string StatusMessage(unsigned long status, const std::string& url) {
    std::string text = "HTTP " + std::to_string(status) + " for " + url;
    if (status == 404) text += " (not found)";
    if (status == 403 && IsGitHubApi(url)) text += " (GitHub's limit for anonymous requests may be used up; try again in an hour)";
    return text;
}
#endif

}

void HttpDownload(const std::string& url, const std::filesystem::path& file, JobControl& control) {
    auto partial = file;
    partial += ".partial";
    std::error_code error;
    std::filesystem::create_directories(file.parent_path(), error);
    std::filesystem::remove(partial, error);
    try {
#ifdef _WIN32
        std::ofstream stream(partial, std::ios::binary | std::ios::trunc);
        if (!stream) throw std::runtime_error("cannot write " + ToUtf8(partial));
        const auto status = Get(url, &control, [&](const char* data, std::size_t size) {
            stream.write(data, static_cast<std::streamsize>(size));
            if (!stream) throw std::runtime_error("cannot write " + ToUtf8(partial));
        });
        stream.close();
        if (status < 200 || status >= 300) throw std::runtime_error(StatusMessage(status, url));
#else
        ThrowIfCancelled(control);
        Curl(url, partial);
#endif
        std::filesystem::remove(file, error);
        RenameNew(partial, file);
    } catch (...) {
        std::filesystem::remove(partial, error);
        throw;
    }
}

namespace {

std::string FetchText(const std::string& url, const std::string& body, const std::string& contentType) {
#ifdef _WIN32
    std::string text;
    const auto status = Send(contentType.empty() ? L"GET" : L"POST", url, body, contentType, nullptr, [&](const char* data, std::size_t size) {
        if (text.size() + size > MaxText) throw std::runtime_error("answer from " + url + " is too large");
        text.append(data, size);
    });
    if (status < 200 || status >= 300) throw std::runtime_error(StatusMessage(status, url));
    return text;
#else
    const auto file = std::filesystem::temp_directory_path() / ("anyps5-launcher-" + std::to_string(std::hash<std::string>{}(url + body)) + ".txt");
    Curl(url, file, body, contentType);
    std::ifstream stream(file, std::ios::binary);
    std::string text{std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
    stream.close();
    std::error_code error;
    std::filesystem::remove(file, error);
    if (text.size() > MaxText) throw std::runtime_error("answer from " + url + " is too large");
    return text;
#endif
}

}

std::string HttpGetText(const std::string& url) {
    return FetchText(url, {}, {});
}

std::string HttpPostText(const std::string& url, const std::string& body, const std::string& contentType) {
    return FetchText(url, body, contentType.empty() ? std::string("application/octet-stream") : contentType);
}

}
