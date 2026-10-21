#pragma once

#include <filesystem>
#include <string>
#include <vector>

// How the game maps its design resolution onto the player's screen. The scene origin is
// always at the screen centre.
//   Expand: the whole design area is visible; a wider or taller screen shows more of the
//           scene on that side (no bars).
//   Fit:    the whole design area is visible, letterboxed with bars on the extra sides.
//   Fill:   the screen is covered with no bars; the design area is cropped on one side.
enum class ScaleMode { Expand, Fit, Fill };

inline constexpr ScaleMode kScaleModes[] = {ScaleMode::Expand, ScaleMode::Fit, ScaleMode::Fill};
const char* scaleModeName(ScaleMode mode); // "Expand", "Fit", "Fill"

struct DisplaySettings {
    int width = 1280; // design resolution, in scene units
    int height = 720;
    ScaleMode scaleMode = ScaleMode::Expand;
};

// When Export writes a file you may want to edit by hand:
//   Always     on every export (your changes are replaced)
//   IfMissing  only when it doesn't exist yet, so your version is kept
//   Never      not at all (an existing one is left alone)
enum class ExportPolicy { Always, IfMissing, Never };
inline constexpr ExportPolicy kExportPolicies[] = {ExportPolicy::Always, ExportPolicy::IfMissing, ExportPolicy::Never};
const char* exportPolicyLabel(ExportPolicy policy); // "Always", "Only if missing", "Never"

struct ExportSettings {
    // Scale modes the exported runtime supports, indexed like kScaleModes. Only their code is
    // exported. The project's default scale mode is always included.
    bool scaleModes[3] = {true, true, true};
    ExportPolicy makefile = ExportPolicy::Always;
    ExportPolicy projectMain = ExportPolicy::Always;    // src/project_main.c
    ExportPolicy vscodeSettings = ExportPolicy::Always; // .vscode/c_cpp_properties.json
    ExportPolicy gitignore = ExportPolicy::Always;

    // Makes sure `defaultMode` is one of the exported modes.
    void includeMode(ScaleMode defaultMode);
    int modeCount() const;
};

// A project is a folder holding a project file and the project's scenes:
//
//   MyGame/
//     MyGame.sceneproj        name and settings (JSON)
//     scenes/                 scene files
//     .sceneeditor/session.json   per-user editor state (open tabs), not project data
//     .gitignore              written by createProject: ignores .sceneeditor/
struct Project {
    std::filesystem::path file; // the .sceneproj file
    std::string name;
    std::string scenesFolder = "scenes"; // relative to the project folder
    DisplaySettings display;
    // Folder holding easy_memory.h and its LICENSE, copied into every export. Relative paths
    // start at the project folder. Defaults to the editor's external/easy_memory; change it
    // in Project Settings if the editor's source tree moves.
    std::string easyMemoryDir = defaultEasyMemoryDir();
    ExportSettings exportSettings;

    std::filesystem::path root() const { return file.parent_path(); }
    std::filesystem::path scenesDir() const { return root() / scenesFolder; }
    std::filesystem::path easyMemoryPath() const;

    static std::string defaultEasyMemoryDir();
};

inline constexpr const char* kProjectExtension = ".sceneproj";

// `path` may be the .sceneproj file or the folder containing one.
bool loadProject(const std::filesystem::path& path, Project& project, std::string& error);

// Writes the project file (e.g. after changing settings).
bool saveProject(const Project& project, std::string& error);

// Creates <location>/<name>/<name>.sceneproj and its scenes folder. The project folder
// must not exist yet, or be empty.
bool createProject(const std::filesystem::path& location, const std::string& name, Project& project,
                   std::string& error);

// "" if `name` can be used as a project / folder name, otherwise why not.
std::string validateProjectName(const std::string& name);

// Which scenes were open (in tab order) and which tab was active. Paths are absolute in
// memory and stored relative to the project folder.
struct ProjectSession {
    std::vector<std::filesystem::path> openScenes;
    int activeScene = 0;
};

ProjectSession loadProjectSession(const Project& project);
bool saveProjectSession(const Project& project, const ProjectSession& session, std::string& error);
