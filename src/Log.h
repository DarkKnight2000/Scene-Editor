#pragma once

#include <cstddef>
#include <string>
#include <vector>

// Editor message log, shown in the Logs panel.

enum class LogLevel { Info, Warning, Error };

struct LogEntry {
    LogLevel level;
    std::string time; // HH:MM:SS
    std::string text;
};

void logInfo(std::string text);
void logWarning(std::string text);
void logError(std::string text);

const std::vector<LogEntry>& logEntries();
void clearLog();

// Increases whenever an entry is added; lets views notice new messages.
std::size_t logRevision();

// Also print every new entry, without its time: info to stdout, warnings and errors to
// stderr. For command-line use (e.g. --export).
void setLogToConsole(bool enabled);
