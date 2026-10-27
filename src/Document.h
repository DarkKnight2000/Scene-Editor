#pragma once

#include <filesystem>
#include <string>

#include "DetailsPanel.h"
#include "EntitiesPanel.h"
#include "History.h"
#include "Scene.h"

// One open scene (one tab): the scene itself plus everything that belongs to it —
// selection, undo history, file path, saved state and per-scene panel state.
struct Document {
    Scene scene;
    EntityId selected = scene.world();
    History history;

    EntitiesPanel entitiesPanel;
    DetailsPanel detailsPanel;

    std::filesystem::path path;  // empty until saved
    std::string savedSnapshot;   // serialized scene at the last save/load
    bool dirty = false;
    int untitledNumber = 1;      // for "Untitled", "Untitled 2", ...

    std::string displayName() const {
        if (!path.empty()) {
            return path.stem().string();
        }
        return untitledNumber <= 1 ? std::string("Untitled") : "Untitled " + std::to_string(untitledNumber);
    }
};
