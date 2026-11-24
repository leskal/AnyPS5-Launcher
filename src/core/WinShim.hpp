#pragma once

// Thin wrappers over the Win32 calls the launcher needs. This header and its
// source include no C++ standard library headers, so the source can be checked
// against the MinGW-w64 headers alone. Functions return 0 or a Win32 error code.

namespace launcher::win {

unsigned long CreateJunction(const wchar_t* link, const wchar_t* target);

// Writes the junction's target, an absolute path without the \??\ prefix.
// Returns ERROR_NOT_A_REPARSE_POINT (4390) when link is not a junction.
unsigned long ReadJunction(const wchar_t* link, wchar_t* target, unsigned long capacity);

// Removes the junction itself, never what it points to.
unsigned long RemoveJunction(const wchar_t* link);

void* CreateJob();
unsigned long AssignProcessIdToJob(void* job, unsigned long processId);
unsigned long JobActiveProcesses(void* job, unsigned long* count);
unsigned long TerminateJob(void* job, unsigned int exitCode);
void CloseJob(void* job);

// The largest visible top-level window of a process in the job, in screen pixels.
// Returns ERROR_NOT_FOUND (1168) when there is none.
unsigned long JobWindowRect(void* job, long* x, long* y, long* width, long* height);

// Asks Windows 11 to round the window's corners; does nothing on older Windows.
void RoundWindowCorners(void* window);

// Moves a file or folder to the Recycle Bin, without asking.
unsigned long RecycleItem(const wchar_t* path);

// Opens a folder in Explorer.
unsigned long OpenFolder(const wchar_t* path);

}
