#include "Console.h"

// Kept apart from the rest of the editor: windows.h clashes with raylib's names.
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstdio>

namespace {
// True if `handle` already goes somewhere (a console, a file or a pipe).
bool connected(DWORD which) {
    const HANDLE handle = GetStdHandle(which);
    return handle != nullptr && handle != INVALID_HANDLE_VALUE && GetFileType(handle) != FILE_TYPE_UNKNOWN;
}
} // namespace

void attachParentConsole() {
    const bool out = connected(STD_OUTPUT_HANDLE);
    const bool err = connected(STD_ERROR_HANDLE);
    if ((out && err) || !AttachConsole(ATTACH_PARENT_PROCESS)) {
        return;
    }
    FILE* stream = nullptr;
    if (!out) {
        freopen_s(&stream, "CONOUT$", "w", stdout);
    }
    if (!err) {
        freopen_s(&stream, "CONOUT$", "w", stderr);
    }
}
#else
void attachParentConsole() {}
#endif
