#include <raylib.h>
#include <raygui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include "Console.h"
#include "EditorConfig.h"
#include "EditorLayout.h"
#include "EditorVersion.h"
#include "Log.h"
#include "LogsPanel.h"
#include "Project.h"
#include "ProjectExport.h"
#include "ProjectLauncher.h"
#include "SceneView.h"
#include "Theme.h"
#include "Widgets.h"
#include "Workspace.h"

namespace {
constexpr float kToolbarHeight = 44.0f;
constexpr float kTabBarHeight = 36.0f;
constexpr float kStatusBarHeight = 28.0f;
constexpr int kLauncherWidth = 900;
constexpr int kLauncherHeight = 580;
constexpr int kEditorMinWidth = 1024; // below this the three panels get too cramped
constexpr int kEditorMinHeight = 600;

// Command line: at most one option.
struct Options {
    enum class Mode { Editor, Open, Export, Version, Help, Invalid } mode = Mode::Editor;
    std::filesystem::path project; // --open / --export
    std::string error;             // Invalid: what's wrong
};

constexpr const char* kUsage = "Usage: SceneEditor [option]\n"
                               "  (no option)          open the editor: the last project, or the project launcher\n"
                               "  --open <project>     open the editor on a project (its .sceneproj file or folder)\n"
                               "  --export <project>   export a project without opening the editor\n"
                               "  --version            print the version\n"
                               "  --help               print this help\n";

Options parseOptions(int argc, char** argv) {
    Options options;
    if (argc < 2) {
        return options;
    }
    const std::string_view option = argv[1];
    const bool takesProject = option == "--open" || option == "--export";
    if (option == "--version" || option == "--help") {
        options.mode = option == "--version" ? Options::Mode::Version : Options::Mode::Help;
    } else if (takesProject && argc >= 3) {
        options.mode = option == "--open" ? Options::Mode::Open : Options::Mode::Export;
        options.project = argv[2];
    } else if (takesProject) {
        options.mode = Options::Mode::Invalid;
        options.error = std::string(option) + " needs a project (its .sceneproj file or folder)";
        return options;
    } else {
        options.mode = Options::Mode::Invalid;
        options.error = "unknown option " + std::string(option);
        return options;
    }
    const int expected = takesProject ? 3 : 2;
    if (argc > expected) {
        options.mode = Options::Mode::Invalid;
        options.error = "unexpected " + std::string(argv[expected]) + " (only one option at a time)";
    }
    return options;
}

// --export: exports without a window, printing the log. Returns the exit code.
int exportFromCommandLine(const std::filesystem::path& path) {
    setLogToConsole(true);
    Project project;
    std::string error;
    if (!loadProject(path, project, error)) {
        logError("Can't open the project: " + error);
        return 1;
    }
    return exportProject(project) ? 0 : 1;
}

// Ctrl+= / Ctrl+- / Ctrl+0: UI scale up / down / back to 100%. Returns true if it changed.
bool handleUiScaleKeys(EditorConfig& config) {
    if (!IsKeyDown(KEY_LEFT_CONTROL) && !IsKeyDown(KEY_RIGHT_CONTROL)) {
        return false;
    }
    float scale = theme::uiScale();
    if (IsKeyPressed(KEY_EQUAL) || IsKeyPressed(KEY_KP_ADD)) {
        scale += 0.125f;
    } else if (IsKeyPressed(KEY_MINUS) || IsKeyPressed(KEY_KP_SUBTRACT)) {
        scale -= 0.125f;
    } else if (IsKeyPressed(KEY_ZERO) || IsKeyPressed(KEY_KP_0)) {
        scale = 1.0f;
    } else {
        return false;
    }
    theme::setUiScale(scale);
    config.uiScale = theme::uiScale();
    logInfo("UI scale " + std::to_string(static_cast<int>(std::lround(config.uiScale * 100.0f))) + "%");
    return true;
}

// Borderless fullscreen done by hand: an undecorated window covering the monitor.
// raylib's ToggleBorderlessWindowed() uses GLFW monitor fullscreen instead, which Windows
// keeps always-on-top (and raylib disables auto-minimize), so Alt+Tab can't cover it.
void toggleFullscreen(bool& fullscreen) {
    if (!fullscreen) {
        const int monitor = GetCurrentMonitor();
        const Vector2 origin = GetMonitorPosition(monitor);
        RestoreWindow(); // un-maximize first, or the new size is ignored
        SetWindowState(FLAG_WINDOW_UNDECORATED);
        SetWindowPosition(static_cast<int>(origin.x), static_cast<int>(origin.y));
        SetWindowSize(GetMonitorWidth(monitor), GetMonitorHeight(monitor));
    } else {
        ClearWindowState(FLAG_WINDOW_UNDECORATED);
        MaximizeWindow();
    }
    fullscreen = !fullscreen;
}

// Puts the window back where it was last time, keeping it on the current monitor.
void applyWindowConfig(const EditorConfig::Window& window) {
    if (window.maximized || window.width <= 0 || window.height <= 0) {
        MaximizeWindow();
        return;
    }
    const int monitor = GetCurrentMonitor();
    const Vector2 origin = GetMonitorPosition(monitor);
    const int monitorWidth = GetMonitorWidth(monitor);
    const int monitorHeight = GetMonitorHeight(monitor);
    const int width = std::min(window.width, monitorWidth);
    const int height = std::min(window.height, monitorHeight);
    // At least part of the title bar must stay reachable.
    const int x = std::clamp(window.x, static_cast<int>(origin.x), static_cast<int>(origin.x) + monitorWidth - 100);
    const int y = std::clamp(window.y, static_cast<int>(origin.y) + 30, static_cast<int>(origin.y) + monitorHeight - 100);
    SetWindowSize(width, height);
    SetWindowPosition(x, y);
}

// Records the window placement. While maximized or fullscreen the previous normal
// placement is kept, so un-maximizing next session goes back to it.
void captureWindowConfig(EditorConfig::Window& window, bool fullscreen) {
    window.fullscreen = fullscreen;
    window.maximized = fullscreen || IsWindowMaximized(); // leaving fullscreen maximizes
    if (!window.maximized) {
        const Vector2 position = GetWindowPosition();
        window.x = static_cast<int>(position.x);
        window.y = static_cast<int>(position.y);
        window.width = GetScreenWidth();
        window.height = GetScreenHeight();
    }
}
void saveConfig(const std::filesystem::path& file, const EditorConfig& config) {
    std::string error;
    if (!saveEditorConfig(file, config, error)) {
        logError("Couldn't save editor config: " + error);
    }
}

// The launcher is a small, fixed-size window centred on the monitor.
void enterLauncherWindow() {
    ClearWindowState(FLAG_WINDOW_UNDECORATED);
    RestoreWindow();
    SetWindowState(FLAG_WINDOW_RESIZABLE); // so the size can change; cleared again below
    const int launcherWidth = static_cast<int>(std::lround(kLauncherWidth * theme::uiScale()));
    const int launcherHeight = static_cast<int>(std::lround(kLauncherHeight * theme::uiScale()));
    SetWindowMinSize(launcherWidth, launcherHeight);
    SetWindowSize(launcherWidth, launcherHeight);
    const int monitor = GetCurrentMonitor();
    const Vector2 origin = GetMonitorPosition(monitor);
    SetWindowPosition(static_cast<int>(origin.x) + (GetMonitorWidth(monitor) - launcherWidth) / 2,
                      static_cast<int>(origin.y) + (GetMonitorHeight(monitor) - launcherHeight) / 2);
    ClearWindowState(FLAG_WINDOW_RESIZABLE);
    SetWindowTitle("Scene Editor");
}

void enterEditorWindow(const EditorConfig::Window& window, bool& fullscreen) {
    SetWindowState(FLAG_WINDOW_RESIZABLE);
    SetWindowMinSize(kEditorMinWidth, kEditorMinHeight);
    applyWindowConfig(window);
    if (window.fullscreen) {
        toggleFullscreen(fullscreen);
    }
}

// Shows the launcher until the user picks a project; nullopt if they close the window.
std::optional<Project> runLauncher(EditorConfig& config, const std::filesystem::path& configFile) {
    enterLauncherWindow();
    ProjectLauncher launcher(config);
    while (!WindowShouldClose()) {
        if (handleUiScaleKeys(config)) {
            enterLauncherWindow();
        }
        ui::beginFrame();
        BeginDrawing();
        ClearBackground(theme::kBackground);
        theme::beginFrame();
        const std::optional<Project> project = launcher.draw({0.0f, 0.0f, theme::screenWidth(), theme::screenHeight()});
        ui::drawPopups();
        ui::endFrame();
        theme::endFrame();
        EndDrawing();
        if (project) {
            saveConfig(configFile, config); // recent projects
            return project;
        }
    }
    saveConfig(configFile, config); // e.g. recent entries removed in the launcher
    return std::nullopt;
}

// Runs the editor on `project` until the user quits or closes the project.
Workspace::Exit runEditor(EditorConfig& config, const std::filesystem::path& configFile, const Project& project) {
    bool fullscreen = false;
    enterEditorWindow(config.window, fullscreen);

    EditorLayout layout;
    layout.setSizes(config.layout.entitiesWidth, config.layout.detailsWidth, config.layout.logsHeight);
    layout.setLogsOpen(config.layout.logsOpen);
    LogsPanel logsPanel;
    logsPanel.setProjectRoot(project.root());
    SceneView sceneView;
    Workspace workspace(project);
    workspace.setPlaySettings(config.play);
    const ProjectSession session = loadProjectSession(project);
    workspace.restoreScenes(session.openScenes, session.activeScene);

    // Remember right away, so the project reopens even if the editor doesn't exit cleanly.
    config.lastProject = project.file;
    config.addRecentProject(project.file);
    saveConfig(configFile, config);

    while (!workspace.exitConfirmed()) {
        // The close button / Alt+F4 only requests quitting: unsaved scenes are asked about first.
        if (WindowShouldClose()) {
            workspace.requestExit(Workspace::Exit::Quit);
        }
        if (IsKeyPressed(KEY_F11)) {
            toggleFullscreen(fullscreen);
        }
        if (!ui::isTextEditing()) {
            handleUiScaleKeys(config);
        }

        // Logical pixels (see Theme.h): toolbar, tabs, the panels with a small margin, status bar.
        const float width = theme::screenWidth();
        const float height = theme::screenHeight();
        constexpr float kMargin = 4.0f;
        const Rectangle toolbar{0.0f, 0.0f, width, kToolbarHeight};
        const Rectangle tabBar{0.0f, kToolbarHeight, width, kTabBarHeight};
        const float panelsTop = kToolbarHeight + kTabBarHeight;
        const Rectangle panels{kMargin, panelsTop, width - 2.0f * kMargin, height - panelsTop - kStatusBarHeight - kMargin};
        const Rectangle statusBar{0.0f, height - kStatusBarHeight, width, kStatusBarHeight};

        ui::beginFrame();
        workspace.beginFrame(); // tab changes queued last frame happen here
        workspace.handleShortcuts();
        Document& doc = workspace.active();
        const bool modal = workspace.isModalOpen();
        layout.update(panels, !modal);
        const DisplaySettings& display = workspace.project().display;
        doc.scene.setUIReferenceSize({static_cast<float>(display.width), static_cast<float>(display.height)});
        doc.history.beginFrame(doc.scene, doc.selected);
        sceneView.update(doc.scene, doc.selected, layout.sceneContent(), display, !modal);

        BeginDrawing();
        ClearBackground(theme::kBackground);
        theme::beginFrame();

        // A modal dialog blocks input to everything drawn before it.
        if (modal) {
            GuiLock();
        }

        workspace.drawToolbar(toolbar);
        workspace.drawTabs(tabBar);
        layout.draw();

        sceneView.draw(doc.scene, layout.sceneContent(), doc.selected, display);
        const Rectangle sceneHeader{layout.sceneBounds().x, layout.sceneBounds().y, layout.sceneBounds().width,
                                    EditorLayout::kHeaderHeight};
        sceneView.drawHeader(sceneHeader, layout.sceneContent(), display);

        // Panels with controls draw last so their popups and tooltips overlap neighbouring panels.
        doc.detailsPanel.draw(layout.detailsContent(), doc.scene, doc.history, doc.selected);
        doc.entitiesPanel.draw(layout.entitiesBounds(), layout.entitiesContent(), doc.scene, doc.history,
                               doc.selected);
        if (layout.logsOpen()) {
            logsPanel.draw(layout.logsBounds(), layout.logsContent());
        }
        bool logsOpen = layout.logsOpen();
        logsPanel.drawStatusBar(statusBar, logsOpen);
        layout.setLogsOpen(logsOpen);

        if (modal) {
            GuiUnlock();
        }
        workspace.drawDialogs();
        ui::drawPopups(); // after dialogs, so a dropdown opened in a dialog draws on top
        doc.history.endFrame(doc.scene);
        ui::endFrame();

        theme::endFrame();
        EndDrawing();
    }

    // Per-project: which scenes were open.
    ProjectSession closing;
    closing.openScenes = workspace.openScenePaths(closing.activeScene);
    std::string error;
    if (!saveProjectSession(project, closing, error)) {
        logError("Couldn't save project session: " + error);
    }

    // Editor-wide: window and panels, and whether to reopen this project next time.
    captureWindowConfig(config.window, fullscreen);
    config.layout = {layout.entitiesWidth(), layout.detailsWidth(), layout.logsHeight(), layout.logsOpen()};
    const Workspace::Exit exit = workspace.exitKind();
    config.lastProject = exit == Workspace::Exit::Quit ? project.file : std::filesystem::path{};
    saveConfig(configFile, config);
    return exit;
}
} // namespace

int main(int argc, char** argv) {
    const Options options = parseOptions(argc, argv);
    if (options.mode != Options::Mode::Editor) {
        attachParentConsole();
    }
    switch (options.mode) {
    case Options::Mode::Version: std::printf("Scene Editor %s\n", SCENE_EDITOR_VERSION); return 0;
    case Options::Mode::Help: std::printf("%s", kUsage); return 0;
    case Options::Mode::Invalid: std::fprintf(stderr, "SceneEditor: %s\n%s", options.error.c_str(), kUsage); return 2;
    case Options::Mode::Export: return exportFromCommandLine(options.project);
    case Options::Mode::Editor:
    case Options::Mode::Open: break;
    }

    SetConfigFlags(FLAG_VSYNC_HINT | FLAG_MSAA_4X_HINT | FLAG_WINDOW_RESIZABLE);
    InitWindow(kLauncherWidth, kLauncherHeight, "Scene Editor");
    SetExitKey(KEY_NULL); // Esc shouldn't close the editor; use Alt+F4
    SetTargetFPS(60);

    const std::filesystem::path configFile = configPath();
    EditorConfig config;
    std::string error;
    if (!loadEditorConfig(configFile, config, error)) {
        logWarning("Editor config ignored (" + error + "): " + configFile.string());
    }
    theme::load(config.uiScale);
    logInfo("Editor config: " + configFile.string());

    // --open: that project. Otherwise reopen the project that was open when the editor was
    // last closed, if any.
    std::optional<Project> project;
    if (options.mode == Options::Mode::Open) {
        Project opened;
        if (loadProject(options.project, opened, error)) {
            project = opened;
        } else {
            const std::string message = "Can't open the project: " + error;
            std::fprintf(stderr, "SceneEditor: %s\n", message.c_str());
            logError(message);
        }
    } else if (!config.lastProject.empty()) {
        Project last;
        if (loadProject(config.lastProject, last, error)) {
            project = last;
        } else {
            logWarning("Couldn't reopen the last project: " + error);
            config.lastProject.clear();
        }
    }

    // Launcher <-> editor until the user quits.
    while (true) {
        if (!project) {
            project = runLauncher(config, configFile);
            if (!project) {
                break; // launcher window closed
            }
        }
        if (runEditor(config, configFile, *project) == Workspace::Exit::Quit) {
            break;
        }
        project.reset(); // Close Project: back to the launcher
    }

    theme::unload();
    CloseWindow();
    return 0;
}
