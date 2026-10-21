#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

// How the Play button builds an exported project. Empty fields use the defaults this editor
// was built with (its own C compiler and raylib build; see PlayDefaults.h, made by meson).
struct PlaySettings {
    std::string make;          // GNU make program, e.g. mingw32-make
    std::string cc;            // C compiler
    std::string cflags;        // replaces the Makefile's CFLAGS
    std::string raylibInclude; // folder containing raylib.h
    std::string raylibLibs;    // raylib library to link (path without spaces)
};

// `settings` with every empty field replaced by its default and paths using forward
// slashes: what Play, and the exported project's VS Code files, build with.
PlaySettings resolvePlaySettings(const PlaySettings& settings);

// The make variables for a resolved PlaySettings: CC=..., CFLAGS=..., RAYLIB_INCLUDE=...,
// RAYLIB_LIBS=... (one argument each).
std::vector<std::string> playMakeVariables(const PlaySettings& resolved);

class ChildProcess;

// The Play button: builds an exported project with its Makefile ("make release") and runs
// the game, without blocking the editor. Build and game output go to the Logs panel.
class GameRunner {
public:
    enum class State { Idle, Building, Running };

    GameRunner();
    ~GameRunner(); // stops a build or game still running

    void setSettings(PlaySettings settings) { m_settings = std::move(settings); }
    const PlaySettings& settings() const { return m_settings; }

    // Runs "make release" in exportDir, then `executable` once the build succeeds. Stops a
    // game that is still running first (its executable is about to be replaced).
    void play(const std::filesystem::path& exportDir, const std::filesystem::path& executable);
    void stop();

    // Call once per frame: forwards output to the log, starts the game after a successful
    // build and notices when it exits.
    void update();

    State state() const { return m_state; }

private:
    void forwardOutput();
    void launchGame();

    PlaySettings m_settings;
    State m_state = State::Idle;
    std::filesystem::path m_dir;
    std::filesystem::path m_executable;
    std::unique_ptr<ChildProcess> m_process; // make while building, then the game
};
