#pragma once

#include <filesystem>
#include <string>

#include "Scene.h"

// Scene file format (JSON):
//   {
//     "format": "SceneEditor.scene", "version": 1,
//     "entities": [
//       { "id": 0, "name": "World", "world": true },
//       { "id": 1, "name": "Canvas", "canvas": true },
//       { "id": 2, "name": "Player", "origin": 0,
//         "components": { "transform": {...}, "sprite": {...} } },
//       { "id": 3, "name": "Health", "origin": 1,
//         "components": { "uiRectTransform": {...}, "uiRectangle": {...} } }
//     ]
//   }
// Entities are stored in sibling order; ids are only meaningful within one file. "origin" is
// the parent in either tree. Files from before the Canvas existed load with an empty one.

std::string serializeScene(const Scene& scene);

// Replaces `out` only on success; otherwise `error` says why.
// Recoverable problems (e.g. a bad origin) are reported through the log.
bool deserializeScene(const std::string& text, Scene& out, std::string& error);

bool saveSceneFile(const Scene& scene, const std::filesystem::path& path, std::string& error);
bool loadSceneFile(const std::filesystem::path& path, Scene& out, std::string& error);
