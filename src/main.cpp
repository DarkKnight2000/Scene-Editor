#include <raylib.h>
#include <raygui.h>

#include <algorithm>
#include <filesystem>
#include <optional>
#include <string>

#include "EditorConfig.h"
#include "EditorLayout.h"
#include "Log.h"
#include "LogsPanel.h"
#include "Project.h"
#include "ProjectLauncher.h"
#include "SceneView.h"
#include "Widgets.h"
#include "Workspace.h"

namespace {
constexpr float kToolbarHeight = 36.0f;
constexpr float kTabBarHeight = 34.0f;
constexpr int kLauncherWidth = 900;
constexpr int kLauncherHeight = 580;
constexpr int kEditorMinWidth = 1024; // below this the three panels get too cramped
constexpr int kEditorMinHeight = 600;

void applyDarkStyle() {
    struct Entry {
        int property;
        unsigned int color;
    };
    constexpr Entry colors[] = {
        {BORDER_COLOR_NORMAL, 0x4a4a50ff},  {BASE_COLOR_NORMAL, 0x333338ff},
        {TEXT_COLOR_NORMAL, 0xd4d4d8ff},    {BORDER_COLOR_FOCUSED, 0x3d8fe0ff},
        {BASE_COLOR_FOCUSED, 0x3b3b42ff},   {TEXT_COLOR_FOCUSED, 0xffffffff},
        {BORDER_COLOR_PRESSED, 0x3d8fe0ff}, {BASE_COLOR_PRESSED, 0x2a5f99ff},
        {TEXT_COLOR_PRESSED, 0xffffffff},   {BORDER_COLOR_DISABLED, 0x3a3a3fff},
        {BASE_COLOR_DISABLED, 0x2a2a2eff},  {TEXT_COLOR_DISABLED, 0x6a6a70ff},
        {LINE_COLOR, 0x3e3e44ff},           {BACKGROUND_COLOR, 0x252529ff},
    };
    for (const Entry& e : colors) {
        GuiSetStyle(DEFAULT, e.property, static_cast<int>(e.color));
    }
    GuiSetStyle(DEFAULT, TEXT_SIZE, 20);
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
    SetWindowMinSize(kLauncherWidth, kLauncherHeight);
    SetWindowSize(kLauncherWidth, kLauncherHeight);
    const int monitor = GetCurrentMonitor();
    const Vector2 origin = GetMonitorPosition(monitor);
    SetWindowPosition(static_cast<int>(origin.x) + (GetMonitorWidth(monitor) - kLauncherWidth) / 2,
                      static_cast<int>(origin.y) + (GetMonitorHeight(monitor) - kLauncherHeight) / 2);
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
        ui::beginFrame();
        BeginDrawing();
        ClearBackground(Color{30, 30, 34, 255});
        const std::optional<Project> project =
            launcher.draw({0.0f, 0.0f, static_cast<float>(GetScreenWidth()), static_cast<float>(GetScreenHeight())});
        ui::endFrame();
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
    LogsPanel logsPanel;
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

        const float width = static_cast<float>(GetScreenWidth());
        const float height = static_cast<float>(GetScreenHeight());
        const Rectangle toolbar{0.0f, 0.0f, width, kToolbarHeight};
        const Rectangle tabBar{0.0f, kToolbarHeight, width, kTabBarHeight};
        const float panelsTop = kToolbarHeight + kTabBarHeight;
        const Rectangle panels{0.0f, panelsTop, width, height - panelsTop};

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
        ClearBackground(Color{30, 30, 34, 255});

        // A modal dialog blocks input to everything drawn before it.
        if (modal) {
            GuiLock();
        }

        workspace.drawToolbar(toolbar);
        workspace.drawTabs(tabBar);
        layout.draw();

        sceneView.draw(doc.scene, layout.sceneContent(), doc.selected, display);
        const Rectangle sceneHeader{layout.sceneBounds().x, layout.sceneBounds().y, layout.sceneBounds().width, 24.0f};
        sceneView.drawHeader(sceneHeader, layout.sceneContent(), display);

        // Panels with controls draw last so their popups and tooltips overlap neighbouring panels.
        doc.detailsPanel.draw(layout.detailsContent(), doc.scene, doc.history, doc.selected);
        doc.entitiesPanel.draw(layout.entitiesBounds(), layout.entitiesContent(), doc.scene, doc.history,
                               doc.selected);
        logsPanel.draw(layout.logsBounds(), layout.logsContent());

        if (modal) {
            GuiUnlock();
        }
        workspace.drawDialogs();
        ui::drawPopups(); // after dialogs, so a dropdown opened in a dialog draws on top
        doc.history.endFrame(doc.scene);
        ui::endFrame();

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
    config.layout = {layout.entitiesWidth(), layout.detailsWidth(), layout.logsHeight()};
    const Workspace::Exit exit = workspace.exitKind();
    config.lastProject = exit == Workspace::Exit::Quit ? project.file : std::filesystem::path{};
    saveConfig(configFile, config);
    return exit;
}
} // namespace

int main() {
    SetConfigFlags(FLAG_VSYNC_HINT | FLAG_MSAA_4X_HINT | FLAG_WINDOW_RESIZABLE);
    InitWindow(kLauncherWidth, kLauncherHeight, "Scene Editor");
    SetExitKey(KEY_NULL); // Esc shouldn't close the editor; use Alt+F4
    SetTargetFPS(60);
    applyDarkStyle();

    const std::filesystem::path configFile = configPath();
    EditorConfig config;
    std::string error;
    if (!loadEditorConfig(configFile, config, error)) {
        logWarning("Editor config ignored (" + error + "): " + configFile.string());
    }
    logInfo("Editor config: " + configFile.string());

    // Reopen the project that was open when the editor was last closed, if any.
    std::optional<Project> project;
    if (!config.lastProject.empty()) {
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

    CloseWindow();
    return 0;
}
