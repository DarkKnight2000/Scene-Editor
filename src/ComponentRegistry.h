#pragma once

#include <nlohmann/json_fwd.hpp>

#include <span>

#include "Components.h"

class Scene;
struct Entity;

namespace ui {
class PropertyGrid;
}

// Which tree a component belongs to: scene entities (World) or UI elements (Canvas).
enum class ComponentScope { World, UI };

// Runtime description of a component type, so editor code can list, add, remove
// and draw components without knowing their C++ types.
struct ComponentType {
    const char* name;
    const char* key; // stable identifier used in scene files
    int icon;       // raygui icon id
    bool removable; // core components (Transform, UIRectTransform) can't be removed or added
    ComponentScope scope;

    bool (*has)(const Entity&);
    void (*add)(Entity&);
    void (*remove)(Entity&);
    void (*drawFields)(Scene&, Entity&, ui::PropertyGrid&);

    // Scene file I/O. `load` adds the component if missing; absent fields keep their defaults.
    void (*save)(const Entity&, nlohmann::ordered_json&);
    void (*load)(Entity&, const nlohmann::ordered_json&);
};

// All component types shown in the Details panel, in display order.
std::span<const ComponentType> componentTypes();

// Icon for an entity in the Entities and Details panels: World, Canvas, UI element or entity.
int entityIcon(const Scene& scene, EntityId entity);
