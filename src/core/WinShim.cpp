#ifdef _WIN32

#include "core/WinShim.hpp"

#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winioctl.h>
#include <shellapi.h>
#include <tlhelp32.h>

namespace launcher::win {

namespace {

// The mount-point form of REPARSE_DATA_BUFFER, which user-mode headers don't declare.
struct MountPointBuffer {
    DWORD ReparseTag;
    WORD ReparseDataLength;
    WORD Reserved;
    WORD SubstituteNameOffset;
    WORD SubstituteNameLength;
    WORD PrintNameOffset;
    WORD PrintNameLength;
    WCHAR PathBuffer[1];
};

constexpr DWORD MountPointHeaderSize = 8;
constexpr DWORD MountPointFixedSize = 8;
constexpr DWORD BufferSize = 16 * 1024;
constexpr wchar_t NtPrefix[] = L"\\??\\";
constexpr DWORD NtPrefixLength = 4;

HANDLE OpenReparsePoint(const wchar_t* path, DWORD access) {
    return CreateFileW(path, access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                       FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
}

}

unsigned long CreateJunction(const wchar_t* link, const wchar_t* target) {
    wchar_t full[MAX_PATH * 4];
    const DWORD fullLength = GetFullPathNameW(target, sizeof(full) / sizeof(full[0]), full, nullptr);
    if (fullLength == 0) return GetLastError();
    if (fullLength >= sizeof(full) / sizeof(full[0])) return ERROR_FILENAME_EXCED_RANGE;
    const wchar_t* print = full;
    DWORD printLength = fullLength;
    if (printLength >= 4 && print[0] == L'\\' && print[1] == L'\\' && print[2] == L'?' && print[3] == L'\\') {
        print += 4;
        printLength -= 4;
    }
    if (printLength >= 2 && print[0] == L'\\' && print[1] == L'\\') return ERROR_NOT_SUPPORTED;

    const DWORD substituteBytes = (NtPrefixLength + printLength) * sizeof(WCHAR);
    const DWORD printBytes = printLength * sizeof(WCHAR);
    const DWORD pathBytes = substituteBytes + printBytes + 2 * static_cast<DWORD>(sizeof(WCHAR));
    if (MountPointHeaderSize + MountPointFixedSize + pathBytes > BufferSize) return ERROR_FILENAME_EXCED_RANGE;

    alignas(8) unsigned char storage[BufferSize] = {};
    auto* buffer = reinterpret_cast<MountPointBuffer*>(storage);
    buffer->ReparseTag = IO_REPARSE_TAG_MOUNT_POINT;
    buffer->ReparseDataLength = static_cast<WORD>(MountPointFixedSize + pathBytes);
    buffer->SubstituteNameOffset = 0;
    buffer->SubstituteNameLength = static_cast<WORD>(substituteBytes);
    buffer->PrintNameOffset = static_cast<WORD>(substituteBytes + sizeof(WCHAR));
    buffer->PrintNameLength = static_cast<WORD>(printBytes);
    WCHAR* out = buffer->PathBuffer;
    for (DWORD index = 0; index < NtPrefixLength; ++index) *out++ = NtPrefix[index];
    for (DWORD index = 0; index < printLength; ++index) *out++ = print[index];
    *out++ = L'\0';
    for (DWORD index = 0; index < printLength; ++index) *out++ = print[index];
    *out++ = L'\0';

    if (!CreateDirectoryW(link, nullptr)) return GetLastError();
    HANDLE handle = OpenReparsePoint(link, GENERIC_WRITE);
    if (handle == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        RemoveDirectoryW(link);
        return error;
    }
    DWORD returned = 0;
    const BOOL ok = DeviceIoControl(handle, FSCTL_SET_REPARSE_POINT, buffer, MountPointHeaderSize + buffer->ReparseDataLength, nullptr, 0, &returned, nullptr);
    const DWORD error = ok ? ERROR_SUCCESS : GetLastError();
    CloseHandle(handle);
    if (!ok) RemoveDirectoryW(link);
    return error;
}

unsigned long ReadJunction(const wchar_t* link, wchar_t* target, unsigned long capacity) {
    if (capacity == 0) return ERROR_INSUFFICIENT_BUFFER;
    target[0] = L'\0';
    const DWORD attributes = GetFileAttributesW(link);
    if (attributes == INVALID_FILE_ATTRIBUTES) return GetLastError();
    if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0) return ERROR_NOT_A_REPARSE_POINT;
    HANDLE handle = OpenReparsePoint(link, 0);
    if (handle == INVALID_HANDLE_VALUE) return GetLastError();
    alignas(8) unsigned char storage[BufferSize] = {};
    DWORD returned = 0;
    const BOOL ok = DeviceIoControl(handle, FSCTL_GET_REPARSE_POINT, nullptr, 0, storage, BufferSize, &returned, nullptr);
    const DWORD error = ok ? ERROR_SUCCESS : GetLastError();
    CloseHandle(handle);
    if (!ok) return error;
    const auto* buffer = reinterpret_cast<const MountPointBuffer*>(storage);
    if (buffer->ReparseTag != IO_REPARSE_TAG_MOUNT_POINT) return ERROR_NOT_A_REPARSE_POINT;
    const WCHAR* name = buffer->PathBuffer + buffer->SubstituteNameOffset / sizeof(WCHAR);
    DWORD length = buffer->SubstituteNameLength / sizeof(WCHAR);
    if (length >= NtPrefixLength && name[0] == L'\\' && name[1] == L'?' && name[2] == L'?' && name[3] == L'\\') {
        name += NtPrefixLength;
        length -= NtPrefixLength;
    }
    if (length + 1 > capacity) return ERROR_INSUFFICIENT_BUFFER;
    for (DWORD index = 0; index < length; ++index) target[index] = name[index];
    target[length] = L'\0';
    return ERROR_SUCCESS;
}

unsigned long RemoveJunction(const wchar_t* link) {
    const DWORD attributes = GetFileAttributesW(link);
    if (attributes == INVALID_FILE_ATTRIBUTES) return GetLastError();
    if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0) return ERROR_NOT_A_REPARSE_POINT;
    return RemoveDirectoryW(link) ? ERROR_SUCCESS : GetLastError();
}

void* CreateJob() {
    return CreateJobObjectW(nullptr, nullptr);
}

unsigned long AssignProcessIdToJob(void* job, unsigned long processId) {
    HANDLE process = OpenProcess(PROCESS_SET_QUOTA | PROCESS_TERMINATE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
    if (process == nullptr) return GetLastError();
    const DWORD error = AssignProcessToJobObject(static_cast<HANDLE>(job), process) ? ERROR_SUCCESS : GetLastError();
    CloseHandle(process);
    return error;
}

unsigned long JobActiveProcesses(void* job, unsigned long* count) {
    JOBOBJECT_BASIC_ACCOUNTING_INFORMATION information{};
    if (!QueryInformationJobObject(static_cast<HANDLE>(job), JobObjectBasicAccountingInformation, &information, sizeof(information), nullptr)) {
        return GetLastError();
    }
    *count = information.ActiveProcesses;
    return ERROR_SUCCESS;
}

unsigned long TerminateJob(void* job, unsigned int exitCode) {
    return TerminateJobObject(static_cast<HANDLE>(job), exitCode) ? ERROR_SUCCESS : GetLastError();
}

void CloseJob(void* job) {
    if (job != nullptr) CloseHandle(static_cast<HANDLE>(job));
}

namespace {

struct WindowSearch {
    HANDLE job;
    RECT best;
    LONGLONG area;
};

BOOL CALLBACK VisitWindow(HWND window, LPARAM parameter) {
    auto* search = reinterpret_cast<WindowSearch*>(parameter);
    if (!IsWindowVisible(window) || IsIconic(window) || GetWindow(window, GW_OWNER) != nullptr) return TRUE;
    DWORD processId = 0;
    GetWindowThreadProcessId(window, &processId);
    if (processId == 0) return TRUE;
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
    if (process == nullptr) return TRUE;
    BOOL inJob = FALSE;
    const BOOL checked = IsProcessInJob(process, search->job, &inJob);
    CloseHandle(process);
    if (!checked || !inJob) return TRUE;
    RECT rect{};
    if (!GetWindowRect(window, &rect)) return TRUE;
    const LONGLONG area = static_cast<LONGLONG>(rect.right - rect.left) * static_cast<LONGLONG>(rect.bottom - rect.top);
    if (area > search->area) {
        search->area = area;
        search->best = rect;
    }
    return TRUE;
}

}

bool ProcessRunning(const wchar_t* exeName) {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    bool found = false;
    for (BOOL more = Process32FirstW(snapshot, &entry); more && !found; more = Process32NextW(snapshot, &entry)) {
        found = lstrcmpiW(entry.szExeFile, exeName) == 0;
    }
    CloseHandle(snapshot);
    return found;
}

void WindowsVersion(wchar_t* text, unsigned long capacity) {
    if (capacity == 0) return;
    text[0] = 0;
    const wchar_t* key = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion";
    wchar_t product[128] = L"Windows";
    wchar_t display[64] = L"";
    wchar_t build[32] = L"";
    DWORD size = sizeof(product);
    RegGetValueW(HKEY_LOCAL_MACHINE, key, L"ProductName", RRF_RT_REG_SZ, nullptr, product, &size);
    size = sizeof(display);
    RegGetValueW(HKEY_LOCAL_MACHINE, key, L"DisplayVersion", RRF_RT_REG_SZ, nullptr, display, &size);
    size = sizeof(build);
    RegGetValueW(HKEY_LOCAL_MACHINE, key, L"CurrentBuild", RRF_RT_REG_SZ, nullptr, build, &size);
    // Windows 11 still says "Windows 10" in ProductName; the build tells them apart.
    long number = 0;
    for (const wchar_t* c = build; *c >= L'0' && *c <= L'9'; ++c) number = number * 10 + (*c - L'0');
    if (number >= 22000 && product[8] == L'1' && product[9] == L'0') product[9] = L'1';
    unsigned long used = 0;
    const auto append = [&](const wchar_t* part) {
        for (const wchar_t* c = part; *c != 0 && used + 1 < capacity; ++c) text[used++] = *c;
        text[used] = 0;
    };
    append(product);
    if (display[0] != 0) {
        append(L" ");
        append(display);
    }
    append(L" (build ");
    append(build);
    append(L")");
}

unsigned long RecycleItem(const wchar_t* path) {
    // SHFileOperation wants a list ending in two zeros.
    int length = 0;
    while (path[length] != 0) ++length;
    if (length == 0 || length > 32000) return ERROR_INVALID_PARAMETER;
    HANDLE heap = GetProcessHeap();
    auto* list = static_cast<wchar_t*>(HeapAlloc(heap, HEAP_ZERO_MEMORY, static_cast<SIZE_T>(length + 2) * sizeof(wchar_t)));
    if (list == nullptr) return ERROR_NOT_ENOUGH_MEMORY;
    for (int i = 0; i < length; ++i) list[i] = path[i];
    SHFILEOPSTRUCTW operation{};
    operation.wFunc = FO_DELETE;
    operation.pFrom = list;
    operation.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI;
    const int result = SHFileOperationW(&operation);
    HeapFree(heap, 0, list);
    if (result != 0) return static_cast<unsigned long>(result);
    return operation.fAnyOperationsAborted ? ERROR_CANCELLED : ERROR_SUCCESS;
}

unsigned long JobWindowRect(void* job, long* x, long* y, long* width, long* height) {
    if (job == nullptr) return ERROR_INVALID_HANDLE;
    WindowSearch search{static_cast<HANDLE>(job), RECT{}, 0};
    EnumWindows(VisitWindow, reinterpret_cast<LPARAM>(&search));
    if (search.area <= 0) return ERROR_NOT_FOUND;
    *x = search.best.left;
    *y = search.best.top;
    *width = search.best.right - search.best.left;
    *height = search.best.bottom - search.best.top;
    return ERROR_SUCCESS;
}

void RoundWindowCorners(void* window) {
    using SetAttribute = HRESULT(WINAPI*)(HWND, DWORD, LPCVOID, DWORD);
    static HMODULE dwm = LoadLibraryW(L"dwmapi.dll");
    if (dwm == nullptr || window == nullptr) return;
    const auto set = reinterpret_cast<SetAttribute>(reinterpret_cast<void*>(GetProcAddress(dwm, "DwmSetWindowAttribute")));
    if (set == nullptr) return;
    const DWORD preference = 2;  // DWMWCP_ROUND
    set(static_cast<HWND>(window), 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &preference, sizeof(preference));
}

unsigned long OpenFolder(const wchar_t* path) {
    const auto result = reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", path, nullptr, nullptr, SW_SHOWNORMAL));
    return result > 32 ? ERROR_SUCCESS : static_cast<unsigned long>(result);
}

}

#endif
