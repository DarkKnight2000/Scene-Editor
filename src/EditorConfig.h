#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "GameRunner.h"

// Editor state kept between sessions: window placement, panel sizes and projects.
// Stored as JSON in the user's config folder (see configPath()). Which scenes are open
// is per project (see ProjectSession).
struct EditorConfig {
    struct Window {
        bool maximized = true;
        bool fullscreen = false;
        // Normal (not maximized) placement; width 0 = never recorded.
        int x = 0;
        int y = 0;
        int width = 0;
        int height = 0;
    } window;

    struct Layout {
        float entitiesWidth = 300.0f;
        float detailsWidth = 340.0f;
        float logsHeight = 220.0f;
    } layout;

    // Project files, most recently opened first.
    std::vector<std::filesystem::path> recentProjects;
    // Project open when the editor was last closed; empty if it was closed back to the launcher.
    std::filesystem::path lastProject;
    // Folder last used for "New Project".
    std::filesystem::path newProjectLocation;

    // How the Play button builds the game ("play" in the file); empty = built-in defaults.
    PlaySettings play;

    // Moves `project` to the front of recentProjects (keeping at most kMaxRecent).
    void addRecentProject(const std::filesystem::path& project);
    static constexpr std::size_t kMaxRecent = 12;
};

// %APPDATA%/SceneEditor/editor.json on Windows, $XDG_CONFIG_HOME (or ~/.config)/SceneEditor/editor.json elsewhere.
// The SCENE_EDITOR_CONFIG environment variable overrides it.
std::filesystem::path configPath();

// Default parent folder for new projects: <Documents>/SceneEditor Projects.
std::filesystem::path defaultProjectsLocation();

// Missing or invalid fields keep their defaults. Returns false (with `error`) if the file
// exists but can't be read or parsed; a missing file is not an error.
bool loadEditorConfig(const std::filesystem::path& path, EditorConfig& config, std::string& error);
bool saveEditorConfig(const std::filesystem::path& path, const EditorConfig& config, std::string& error);
