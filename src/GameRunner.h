#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

// How the Play button builds an exported project. Empty fields use the defaults:
//   installed editor     gcc (Windows) or cc, the Makefile's CFLAGS, and raylib built from
//                        the raylib/src/ installed next to the editor (see AppPaths.h)
//   development build    the C compiler and raylib this editor was built with (PlayDefaults.h,
//                        made by meson)
struct PlaySettings {
    std::string make;          // GNU make program, e.g. mingw32-make
    std::string cc;            // C compiler
    std::string cflags;        // replaces the Makefile's CFLAGS
    std::string raylibSource;  // raylib's src/ folder, built along with the game
    std::string raylibInclude; // or a prebuilt raylib: the folder containing raylib.h
    std::string raylibLibs;    // and the library to link (path without spaces)
};

// `settings` with every empty field replaced by its default and paths using forward
// slashes: what Play, and the exported project's VS Code files, build with.
PlaySettings resolvePlaySettings(const PlaySettings& settings);

// The make variables for a resolved PlaySettings: CC=..., CFLAGS=..., and RAYLIB_SRC=... or
// RAYLIB_INCLUDE=... RAYLIB_LIBS=... (one argument each; empty settings are left out).
std::vector<std::string> playMakeVariables(const PlaySettings& resolved);

class ChildProcess;

// The Play button: builds an exported project with its Makefile ("make") and runs
// the game, without blocking the editor. Build and game output go to the Logs panel.
class GameRunner {
public:
    enum class State { Idle, Building, Running };

    GameRunner();
    ~GameRunner(); // stops a build or game still running

    void setSettings(PlaySettings settings) { m_settings = std::move(settings); }
    const PlaySettings& settings() const { return m_settings; }

    // Runs "make" in exportDir, then `executable` once the build succeeds. Stops a
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
