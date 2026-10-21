#pragma once

#include <raylib.h>

#include <vector>

#include "ComponentRegistry.h"
#include "Scene.h"

class History;

// Shows the selected entity's name and components, with editable fields,
// an "Add Component" menu, per-component remove buttons and a delete-entity button.
class DetailsPanel {
public:
    // Clears `selected` if the user deletes the entity.
    void draw(Rectangle content, Scene& scene, History& history, entt::entity& selected);

private:
    // Returns the index of a component type to add, or -1.
    // Only components of `scope` (the entity's tree) are offered.
    int drawAddMenu(Rectangle anchor, Rectangle content, const entt::registry& registry, entt::entity entity,
                    ComponentScope scope);

    Vector2 m_scroll{};
    float m_contentHeight = 0.0f;
    std::vector<bool> m_collapsed; // per component type, shared by all entities

    bool m_addMenuOpen = false;
    Rectangle m_addMenuRect{}; // last frame's menu rect, so it can block clicks underneath
    entt::entity m_lastEntity = entt::null;
};
