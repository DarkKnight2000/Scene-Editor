#pragma once

#include <filesystem>

// The installer puts what exports and Play need next to the executable:
//
//   SceneEditor.exe
//   box2d/        Box2D, copied into exports with physics
//   raylib/src/   raylib's sources, built by Play with the game's compiler
//
// A development build has none of these and uses the paths it was built with instead.

// `relative` (e.g. "box2d") inside the executable's folder if it exists there, otherwise empty.
std::filesystem::path installedPath(const std::filesystem::path& relative);

// `path` without spaces where the system has such a name for it (Windows short names), for
// make, which splits paths at spaces. Unchanged otherwise.
std::filesystem::path pathWithoutSpaces(const std::filesystem::path& path);
