#pragma once

#include <raylib.h>

#include <optional>
#include <string>
#include <vector>

#include "Components.h"

// Where an entity ends up after the origin chain and the World's transform.
struct Pose {
    Vector2 position;
    float rotation; // degrees
    Vector2 scale;
};

// One node of a scene: the World, the Canvas, a scene entity or a UI element. Components it
// doesn't have are empty.
//   World tree:  the World and scene entities, which always have a transform.
//   Canvas tree: the Canvas and UI elements, which always have a rectTransform.
struct Entity {
    EntityId id = kNoEntity;
    std::string name;
    // The entity it follows: in the World tree its origin, in the Canvas tree its parent
    // element. Always in its own tree; kNoEntity only for the World and the Canvas.
    EntityId parent = kNoEntity;

    std::optional<TransformComponent> transform;
    std::optional<SpriteComponent> sprite;
    std::optional<ColliderComponent> collider;
    std::optional<RigidbodyComponent> rigidbody;
    std::optional<PlatformerControllerComponent> platformer;

    std::optional<UIRectTransformComponent> rectTransform;
    std::optional<UIRectangleComponent> uiRectangle;
    std::optional<UILabelComponent> uiLabel;
};

// One entry of the entity tree, in display order.
struct HierarchyRow {
    EntityId entity;
    int depth; // 0 = a root (World or Canvas)
};

// A scene has two trees, each under a fixed root:
//   World    scene entities (Transform), placed in scene coordinates
//   Canvas   UI elements (UIRectTransform), laid out on the screen
// An entity's parent is always in its own tree.
//
// Entities are referred to by id. A pointer or reference from find() / get() is only good
// until the next create, destroy or reorder.
class Scene {
public:
    static constexpr EntityId kWorld = 1;
    static constexpr EntityId kCanvas = 2;

    // Creates the World and Canvas roots.
    Scene();

    // Creates an entity following the World, with a Transform. An empty name gets "Entity <n>".
    EntityId createEntity(std::string name = {});

    // Creates a UI element (with a UIRectTransform) under `parent`: the Canvas or another UI
    // element (anything else means the Canvas). An empty name gets "UI Element <n>".
    EntityId createUIElement(EntityId parent, std::string name = {});

    // Destroys an entity. Its followers switch to its parent and keep their placement.
    // The roots can't be destroyed. Returns false if nothing was destroyed.
    bool destroyEntity(EntityId entity);

    // nullptr if there's no such entity.
    Entity* find(EntityId entity);
    const Entity* find(EntityId entity) const;
    // The entity, which must exist.
    Entity& get(EntityId entity);
    const Entity& get(EntityId entity) const;
    bool contains(EntityId entity) const { return find(entity) != nullptr; }

    EntityId world() const { return kWorld; }
    bool isWorld(EntityId entity) const { return entity == kWorld; }
    EntityId canvas() const { return kCanvas; }
    bool isCanvas(EntityId entity) const { return entity == kCanvas; }
    bool isRoot(EntityId entity) const { return isWorld(entity) || isCanvas(entity); }
    // In the Canvas tree: the Canvas itself or a UI element.
    bool isUI(EntityId entity) const;

    // The entity's parent (see Entity::parent); kNoEntity for the roots and unknown ids.
    EntityId parentOf(EntityId entity) const;

    // Final placement: each transform in the origin chain applied on top of its parent's,
    // starting from the World (see TransformComponent::inheritRotation / inheritScale).
    // Non-uniform parent scale is applied per axis, so rotated children of a
    // non-uniformly scaled parent are approximated rather than skewed.
    // UI elements have no pose; they get the World's.
    Pose finalPose(EntityId entity) const;

    // A UI element's rectangle in canvas units, on a canvas of `canvasSize` (top-left at
    // (0, 0)). The Canvas itself gives the whole canvas.
    Rectangle uiRect(EntityId entity, Vector2 canvasSize) const;

    // Canvas size used to keep UI elements in place when they change parent: the design
    // resolution. Set by the editor from the project settings.
    void setUIReferenceSize(Vector2 size) { m_uiReferenceSize = size; }
    Vector2 uiReferenceSize() const { return m_uiReferenceSize; }

    // True if `origin` can be `entity`'s origin (parent): an existing entity in the same
    // tree that isn't `entity` or one of its followers. The roots never get one.
    bool canSetOrigin(EntityId entity, EntityId origin) const;

    // Changes `entity`'s origin while keeping its final position, rotation and scale (UI
    // elements: their rectangle at the reference size). Returns false if not allowed.
    bool setOrigin(EntityId entity, EntityId origin);

    // Sets a scene entity's local position so that its final position is `position`
    // (scene coordinates). Does nothing for the roots and UI elements.
    void placeEntity(EntityId entity, Vector2 position);

    // Moves `entity` under `newOrigin` (keeping its world position) and places it just
    // before sibling `before`, or last among the siblings when `before` is kNoEntity.
    // Its followers move along with it. Returns false if the move isn't allowed.
    bool moveEntity(EntityId entity, EntityId newOrigin, EntityId before);

    // Every entity, in sibling order: the World, the Canvas, then the rest.
    const std::vector<Entity>& entities() const { return m_entities; }

    // Position in entities(); the World is always 0 and the Canvas 1. -1 if not present.
    int orderIndex(EntityId entity) const;
    void setOrderIndex(EntityId entity, int index);

    // Low-level restore helpers for undo: create an entity with a given id (no components,
    // appended last), or destroy one without touching its followers. The caller restores
    // consistent state.
    Entity& createWithId(EntityId id);
    void destroyRaw(EntityId entity);

    // Both trees, the World's first: each entity is listed under its parent, siblings in
    // entities() order.
    std::vector<HierarchyRow> hierarchy() const;

private:
    std::vector<Entity> m_entities;
    Vector2 m_uiReferenceSize{1920.0f, 1080.0f};
    unsigned m_createdCount = 0;
    unsigned m_createdUICount = 0;
    EntityId m_nextId = kCanvas + 1;

    Entity& add(EntityId id, std::string name);
};
