#include "AppPaths.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

#include <system_error>
#include <vector>

namespace {
std::filesystem::path executableDir() {
#ifdef _WIN32
    std::vector<wchar_t> buffer(MAX_PATH);
    for (;;) {
        const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0) {
            return {};
        }
        if (length < buffer.size()) {
            return std::filesystem::path(std::wstring(buffer.data(), length)).parent_path();
        }
        buffer.resize(buffer.size() * 2); // truncated: try a bigger buffer
    }
#else
    std::error_code ec;
    return std::filesystem::read_symlink("/proc/self/exe", ec).parent_path();
#endif
}
} // namespace

std::filesystem::path installedPath(const std::filesystem::path& relative) {
    static const std::filesystem::path dir = executableDir();
    if (dir.empty()) {
        return {};
    }
    const std::filesystem::path path = dir / relative;
    std::error_code ec;
    return std::filesystem::exists(path, ec) ? path : std::filesystem::path{};
}

std::filesystem::path pathWithoutSpaces(const std::filesystem::path& path) {
#ifdef _WIN32
    const std::wstring text = path.wstring();
    if (text.find(L' ') == std::wstring::npos) {
        return path;
    }
    const DWORD length = GetShortPathNameW(text.c_str(), nullptr, 0);
    if (length == 0) {
        return path;
    }
    std::vector<wchar_t> buffer(length);
    if (GetShortPathNameW(text.c_str(), buffer.data(), length) == 0) {
        return path;
    }
    return std::filesystem::path(buffer.data());
#else
    return path;
#endif
}
