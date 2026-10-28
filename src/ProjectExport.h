#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "Project.h"
#include "Scene.h"
#include "SceneExporter.h"

// Scene files in a project's scenes folder.
inline constexpr const char* kSceneExtension = ".json";

// Where a project is exported to (Project Settings; by default <project>/export).
std::filesystem::path projectExportDir(const Project& project);

// Exports every saved scene of `project` into projectExportDir() (see SceneExporter.h),
// following its export settings and logging what happens. For each scene file,
// `openScene` may return the scene as the editor has it in memory, to export what the
// editor shows; otherwise (or without `openScene`) the file is read. Returns whether the
// export can be built: at least one scene, and every shared file written. `exported`, if
// given, receives what was exported of each scene. The game starts in `startScene` (a scene
// name; empty: the first scene).
bool exportProject(const Project& project,
                   const std::function<const Scene*(const std::filesystem::path& file)>& openScene = {},
                   std::vector<ExportedScene>* exported = nullptr, const std::string& startScene = {});
