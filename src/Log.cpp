#include "Log.h"

#include <chrono>
#include <ctime>
#include <utility>

namespace {
constexpr std::size_t kMaxEntries = 1000;

std::vector<LogEntry> g_entries;
std::size_t g_revision = 0;

std::string currentTime() {
    const std::time_t now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    char buffer[16];
    std::strftime(buffer, sizeof(buffer), "%H:%M:%S", &local);
    return buffer;
}

void add(LogLevel level, std::string text) {
    if (g_entries.size() >= kMaxEntries) {
        g_entries.erase(g_entries.begin(), g_entries.begin() + static_cast<std::ptrdiff_t>(kMaxEntries / 10));
    }
    g_entries.push_back({level, currentTime(), std::move(text)});
    ++g_revision;
}
} // namespace

void logInfo(std::string text) {
    add(LogLevel::Info, std::move(text));
}

void logWarning(std::string text) {
    add(LogLevel::Warning, std::move(text));
}

void logError(std::string text) {
    add(LogLevel::Error, std::move(text));
}

const std::vector<LogEntry>& logEntries() {
    return g_entries;
}

void clearLog() {
    g_entries.clear();
    ++g_revision;
}

std::size_t logRevision() {
    return g_revision;
}
