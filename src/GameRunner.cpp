#include "GameRunner.h"

#include "Log.h"
#include "PlayDefaults.h"

#include <atomic>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <csignal>
#include <sys/wait.h>
#include <unistd.h>
#endif

// A program running in the background with its output (stdout and stderr together) read
// line by line on a separate thread. Programs it starts in turn (the compiler under make)
// are stopped along with it.
class ChildProcess {
public:
    ChildProcess() = default;
    ChildProcess(const ChildProcess&) = delete;
    ChildProcess& operator=(const ChildProcess&) = delete;
    ~ChildProcess() {
        kill();
        if (m_reader.joinable()) {
            m_reader.join();
        }
#ifdef _WIN32
        if (m_process != nullptr) CloseHandle(m_process);
        if (m_job != nullptr) CloseHandle(m_job);
#endif
    }

    // Runs `commandLine` (through the shell on POSIX) in `dir`. False, with `error`, if it
    // couldn't be started.
    bool start(const std::string& commandLine, const std::filesystem::path& dir, std::string& error);

    // Output lines received since the last call.
    std::vector<std::string> takeLines() {
        std::lock_guard lock(m_mutex);
        return std::exchange(m_lines, {});
    }

    // The exit code once the program has exited and all its output has been read.
    std::optional<int> exitCode();

    void kill();

private:
    // Splits the bytes read into lines (without \r\n).
    void received(const char* data, std::size_t size) {
        std::lock_guard lock(m_mutex);
        for (std::size_t i = 0; i < size; ++i) {
            if (data[i] == '\n') {
                m_lines.push_back(std::exchange(m_partial, {}));
            } else if (data[i] != '\r') {
                m_partial += data[i];
            }
        }
    }
    void finished() {
        std::lock_guard lock(m_mutex);
        if (!m_partial.empty()) {
            m_lines.push_back(std::exchange(m_partial, {}));
        }
        m_eof = true;
    }

    std::thread m_reader;
    std::mutex m_mutex;
    std::vector<std::string> m_lines;
    std::string m_partial;
    std::atomic<bool> m_eof{false};
    std::optional<int> m_exitCode;
#ifdef _WIN32
    HANDLE m_process = nullptr;
    HANDLE m_job = nullptr; // holds the program and everything it starts
#else
    pid_t m_pid = -1;
#endif
};

#ifdef _WIN32
namespace {
std::wstring widen(const std::string& text) {
    if (text.empty()) {
        return {};
    }
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring wide(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(), size);
    return wide;
}

std::string lastErrorText() {
    return "Windows error " + std::to_string(GetLastError());
}
} // namespace

bool ChildProcess::start(const std::string& commandLine, const std::filesystem::path& dir, std::string& error) {
    SECURITY_ATTRIBUTES inherit{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE readPipe = nullptr;
    HANDLE writePipe = nullptr;
    if (!CreatePipe(&readPipe, &writePipe, &inherit, 0)) {
        error = "can't create a pipe: " + lastErrorText();
        return false;
    }
    SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0); // only the child's end is inherited
    HANDLE input = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit, OPEN_EXISTING, 0, nullptr);

    // Closing the job (or TerminateJobObject) ends the program and everything it started.
    m_job = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    SetInformationJobObject(m_job, JobObjectExtendedLimitInformation, &limits, sizeof(limits));

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = input;
    startup.hStdOutput = writePipe;
    startup.hStdError = writePipe;
    PROCESS_INFORMATION info{};
    std::wstring command = widen(commandLine);
    const std::wstring directory = dir.wstring();
    // No console window; suspended until it is in the job, so nothing it starts escapes.
    const BOOL created = CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED,
                                        nullptr, directory.c_str(), &startup, &info);
    CloseHandle(writePipe); // the child holds its own copy; EOF once it (and its children) exit
    if (input != INVALID_HANDLE_VALUE) {
        CloseHandle(input);
    }
    if (!created) {
        error = "can't start: " + lastErrorText();
        CloseHandle(readPipe);
        return false;
    }
    AssignProcessToJobObject(m_job, info.hProcess);
    ResumeThread(info.hThread);
    CloseHandle(info.hThread);
    m_process = info.hProcess;

    m_reader = std::thread([this, readPipe] {
        char buffer[4096];
        DWORD read = 0;
        while (ReadFile(readPipe, buffer, sizeof(buffer), &read, nullptr) && read > 0) {
            received(buffer, read);
        }
        CloseHandle(readPipe);
        finished();
    });
    return true;
}

std::optional<int> ChildProcess::exitCode() {
    if (!m_exitCode && m_process != nullptr && m_eof && WaitForSingleObject(m_process, 0) == WAIT_OBJECT_0) {
        DWORD code = 0;
        GetExitCodeProcess(m_process, &code);
        m_exitCode = static_cast<int>(code);
    }
    return m_exitCode;
}

void ChildProcess::kill() {
    if (m_job != nullptr) {
        TerminateJobObject(m_job, 1);
    }
}
#else
bool ChildProcess::start(const std::string& commandLine, const std::filesystem::path& dir, std::string& error) {
    int fds[2];
    if (pipe(fds) != 0) {
        error = "can't create a pipe";
        return false;
    }
    m_pid = fork();
    if (m_pid < 0) {
        error = "can't start a process";
        close(fds[0]);
        close(fds[1]);
        return false;
    }
    if (m_pid == 0) {
        setpgid(0, 0); // own process group, so kill() reaches everything it starts
        dup2(fds[1], STDOUT_FILENO);
        dup2(fds[1], STDERR_FILENO);
        close(fds[0]);
        close(fds[1]);
        if (chdir(dir.c_str()) != 0) {
            _exit(127);
        }
        execl("/bin/sh", "sh", "-c", commandLine.c_str(), static_cast<char*>(nullptr));
        _exit(127);
    }
    close(fds[1]);
    const int readFd = fds[0];
    m_reader = std::thread([this, readFd] {
        char buffer[4096];
        ssize_t count = 0;
        while ((count = read(readFd, buffer, sizeof(buffer))) > 0) {
            received(buffer, static_cast<std::size_t>(count));
        }
        close(readFd);
        finished();
    });
    return true;
}

std::optional<int> ChildProcess::exitCode() {
    int status = 0;
    if (!m_exitCode && m_pid > 0 && m_eof && waitpid(m_pid, &status, WNOHANG) == m_pid) {
        m_exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : 1;
    }
    return m_exitCode;
}

void ChildProcess::kill() {
    if (m_pid > 0 && !m_exitCode) {
        ::kill(-m_pid, SIGTERM);
    }
}
#endif

namespace {
// One command-line argument, quoted for the platform's command line.
std::string quote(const std::string& arg) {
#ifdef _WIN32
    std::string quoted = "\"";
    for (const char c : arg) {
        if (c == '"') {
            quoted += '\\';
        }
        quoted += c;
    }
    return quoted + "\"";
#else
    std::string quoted = "'";
    for (const char c : arg) {
        quoted += c == '\'' ? std::string("'\\''") : std::string(1, c);
    }
    return quoted + "'";
#endif
}

std::string orDefault(const std::string& value, const char* fallback) {
    return value.empty() ? std::string(fallback) : value;
}

// Makefile paths: forward slashes work for make and the compilers on every platform.
std::string makePath(std::string path) {
    for (char& c : path) {
        if (c == '\\') {
            c = '/';
        }
    }
    return path;
}

bool startsWith(const std::string& text, const char* prefix) {
    return text.rfind(prefix, 0) == 0;
}

bool containsWord(const std::string& text, const char* word) {
    return text.find(word) != std::string::npos;
}
} // namespace

PlaySettings resolvePlaySettings(const PlaySettings& settings) {
    PlaySettings r;
    r.make = makePath(orDefault(settings.make, SCENE_EDITOR_PLAY_MAKE));
    r.cc = orDefault(settings.cc, SCENE_EDITOR_PLAY_CC);
    r.cflags = orDefault(settings.cflags, SCENE_EDITOR_PLAY_CFLAGS);
    r.raylibInclude = makePath(orDefault(settings.raylibInclude, SCENE_EDITOR_PLAY_RAYLIB_INCLUDE));
    r.raylibLibs = makePath(orDefault(settings.raylibLibs, SCENE_EDITOR_PLAY_RAYLIB_LIBS));
    return r;
}

std::vector<std::string> playMakeVariables(const PlaySettings& resolved) {
    return {"CC=" + resolved.cc, "CFLAGS=" + resolved.cflags, "RAYLIB_INCLUDE=" + resolved.raylibInclude,
            "RAYLIB_LIBS=" + resolved.raylibLibs};
}

GameRunner::GameRunner() = default;
GameRunner::~GameRunner() = default;

void GameRunner::play(const std::filesystem::path& exportDir, const std::filesystem::path& executable) {
    if (m_process) {
        m_process->kill();
        m_process.reset(); // waits for its output to close, so the executable is free again
    }
    m_dir = exportDir;
    m_executable = executable;

    const PlaySettings resolved = resolvePlaySettings(m_settings);
    std::string command = quote(resolved.make);
    for (const std::string& variable : playMakeVariables(resolved)) {
        command += " " + quote(variable);
    }

    auto process = std::make_unique<ChildProcess>();
    std::string error;
    if (!process->start(command, m_dir, error)) {
        logError("Play: can't run make (" + resolved.make + "): " + error);
        m_state = State::Idle;
        return;
    }
    logInfo("Play: building " + m_executable.stem().string() + " (make)...");
    m_process = std::move(process);
    m_state = State::Building;
}

void GameRunner::stop() {
    if (!m_process) {
        return;
    }
    m_process->kill();
    m_process.reset();
    logInfo(m_state == State::Building ? "Play: build cancelled" : "Play: game stopped");
    m_state = State::Idle;
}

void GameRunner::forwardOutput() {
    for (const std::string& line : m_process->takeLines()) {
        if (line.empty()) {
            continue;
        }
        // raylib's startup chatter (INFO: ...) would bury everything else.
        if (m_state == State::Running && startsWith(line, "INFO: ")) {
            continue;
        }
        if (containsWord(line, "error") || containsWord(line, "ERROR") || containsWord(line, "Error")) {
            logError("  " + line);
        } else if (containsWord(line, "warning") || containsWord(line, "WARNING")) {
            logWarning("  " + line);
        } else {
            logInfo("  " + line);
        }
    }
}

void GameRunner::update() {
    if (!m_process) {
        return;
    }
    forwardOutput();
    const std::optional<int> code = m_process->exitCode();
    if (!code) {
        return;
    }
    m_process.reset();
    if (m_state == State::Building) {
        if (*code != 0) {
            logError("Play: build failed (make exited with " + std::to_string(*code) + ")");
            m_state = State::Idle;
            return;
        }
        launchGame();
    } else {
        logInfo("Play: game exited (code " + std::to_string(*code) + ")");
        m_state = State::Idle;
    }
}

void GameRunner::launchGame() {
    const std::filesystem::path& exe = m_executable;
    auto process = std::make_unique<ChildProcess>();
    std::string error;
    const std::u8string exeText = exe.u8string();
    if (!process->start(quote(std::string(exeText.begin(), exeText.end())), m_dir, error)) {
        logError("Play: can't start " + exe.string() + ": " + error);
        m_state = State::Idle;
        return;
    }
    logInfo("Play: running " + exe.filename().string() + " (Esc quits it; Stop or Shift+F5 here)");
    m_process = std::move(process);
    m_state = State::Running;
}
