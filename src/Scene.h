#pragma once

#include <entt/entt.hpp>
#include <raylib.h>

#include <string>
#include <unordered_map>
#include <vector>

#include "Components.h"

// Where an entity ends up after the origin chain and the World's transform.
struct Pose {
    Vector2 position;
    float rotation; // degrees
    Vector2 scale;
};

// One entry of the entity tree, in display order.
struct HierarchyRow {
    entt::entity entity;
    int depth; // 0 = a root (World or Canvas)
};

// A scene has two trees, each under a fixed root:
//   World    scene entities (Transform), placed in scene coordinates
//   Canvas   UI elements (UIRectTransform), laid out on the screen
// An entity's parent is always in its own tree.
class Scene {
public:
    // Creates the World and Canvas roots.
    Scene();

    // Creates an entity following the World, with Name and Transform components.
    // An empty name gets "Entity <n>".
    entt::entity createEntity(std::string name = {});

    // Creates a UI element (Name and UIRectTransform) under `parent`: the Canvas or another
    // UI element (anything else means the Canvas). An empty name gets "UI Element <n>".
    entt::entity createUIElement(entt::entity parent, std::string name = {});

    // Destroys an entity. Its followers switch to its parent and keep their placement.
    // The roots can't be destroyed. Returns false if nothing was destroyed.
    bool destroyEntity(entt::entity entity);

    entt::entity world() const { return m_world; }
    bool isWorld(entt::entity entity) const { return entity == m_world; }
    entt::entity canvas() const { return m_canvas; }
    bool isCanvas(entt::entity entity) const { return entity == m_canvas; }
    bool isRoot(entt::entity entity) const { return isWorld(entity) || isCanvas(entity); }
    // In the Canvas tree: the Canvas itself or a UI element.
    bool isUI(entt::entity entity) const;

    // The entity's parent: Transform's origin or UIRectTransform's parent; null for the roots.
    entt::entity parentOf(entt::entity entity) const;
    // Sets the parent without changing anything else (for loading and undo).
    void setParentRaw(entt::entity entity, entt::entity parent);

    entt::registry& registry() { return m_registry; }
    const entt::registry& registry() const { return m_registry; }

    // Final placement: each transform in the origin chain applied on top of its parent's,
    // starting from the World (see TransformComponent::inheritRotation / inheritScale).
    // Non-uniform parent scale is applied per axis, so rotated children of a
    // non-uniformly scaled parent are approximated rather than skewed.
    // UI elements have no pose; they get the World's.
    Pose finalPose(entt::entity entity) const;

    // A UI element's rectangle in canvas units, on a canvas of `canvasSize` (top-left at
    // (0, 0)). The Canvas itself gives the whole canvas.
    Rectangle uiRect(entt::entity entity, Vector2 canvasSize) const;

    // Canvas size used to keep UI elements in place when they change parent: the design
    // resolution. Set by the editor from the project settings.
    void setUIReferenceSize(Vector2 size) { m_uiReferenceSize = size; }
    Vector2 uiReferenceSize() const { return m_uiReferenceSize; }

    // True if `origin` can be `entity`'s origin (parent): an existing entity in the same
    // tree that isn't `entity` or one of its followers. The roots never get one.
    bool canSetOrigin(entt::entity entity, entt::entity origin) const;

    // Changes `entity`'s origin while keeping its final position, rotation and scale (UI
    // elements: their rectangle at the reference size). Returns false if not allowed.
    bool setOrigin(entt::entity entity, entt::entity origin);

    // Sets a scene entity's local position so that its final position is `position`
    // (scene coordinates). Does nothing for the roots and UI elements.
    void placeEntity(entt::entity entity, Vector2 position);

    // Moves `entity` under `newOrigin` (keeping its world position) and places it just
    // before sibling `before`, or last among the siblings when `before` is null.
    // Its followers move along with it. Returns false if the move isn't allowed.
    bool moveEntity(entt::entity entity, entt::entity newOrigin, entt::entity before);

    // Entities in sibling order (World first).
    const std::vector<entt::entity>& entities() const { return m_order; }

    // Stable ids, for references that must survive delete + restore (undo history).
    StableId idOf(entt::entity entity) const;
    entt::entity findById(StableId id) const; // entt::null if not present

    // Position in entities(); the World is always 0 and the Canvas 1.
    int orderIndex(entt::entity entity) const;
    void setOrderIndex(entt::entity entity, int index);

    // Low-level restore helpers for undo: create an entity with a given id (bare Name +
    // Transform following the World, appended last), or destroy one without touching
    // its followers. The caller restores consistent state.
    entt::entity createWithId(StableId id);
    void destroyRaw(entt::entity entity);

    // Both trees, the World's first: each entity is listed under its parent, siblings in
    // entities() order.
    std::vector<HierarchyRow> hierarchy() const;

private:
    entt::registry m_registry;
    std::vector<entt::entity> m_order;
    entt::entity m_world = entt::null;
    entt::entity m_canvas = entt::null;
    Vector2 m_uiReferenceSize{1920.0f, 1080.0f};
    unsigned m_createdCount = 0;
    unsigned m_createdUICount = 0;
    std::unordered_map<StableId, entt::entity> m_byId;
    StableId m_nextId = 1;

    void assignId(entt::entity entity, StableId id);
};
