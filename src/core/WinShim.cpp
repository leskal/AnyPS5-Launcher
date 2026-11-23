#ifdef _WIN32

#include "core/WinShim.hpp"

#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winioctl.h>
#include <shellapi.h>

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

unsigned long OpenFolder(const wchar_t* path) {
    const auto result = reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", path, nullptr, nullptr, SW_SHOWNORMAL));
    return result > 32 ? ERROR_SUCCESS : static_cast<unsigned long>(result);
}

}

#endif
