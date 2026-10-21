#pragma once

#include <entt/entt.hpp>
#include <raylib.h>

#include <cstdint>
#include <string>

// Every editor entity has a Name and a Transform; the rest are optional.

// Identifies an entity across deletion and re-creation (entt handles get reused).
// Unique within a Scene; not saved to files.
using StableId = std::uint64_t;

struct IdComponent {
    StableId id;
};

// Marks the scene's root entity. Exactly one exists; it can't be deleted or moved.
struct WorldTag {};

struct NameComponent {
    std::string name;
};

struct TransformComponent {
    // Entity this transform follows (its parent): the World root unless set otherwise.
    // Only the World has no origin. position/rotation/scale are relative to the origin's
    // final placement, so the World's transform moves, rotates and zooms the whole scene.
    entt::entity origin = entt::null;
    Vector2 position{0.0f, 0.0f};
    float rotation = 0.0f; // degrees
    Vector2 scale{1.0f, 1.0f};

    // Turn off to follow the origin's position only, e.g. a health bar that shouldn't
    // spin or flip with its character. Applies to the whole parent chain, World included.
    bool inheritRotation = true;
    bool inheritScale = true;
};

// What a sprite draws, filling its size: a rectangle, a circle (an ellipse when the size
// isn't square) or a triangle pointing up (down when flipped by a negative y scale).
enum class SpriteShape { Rectangle, Circle, Triangle };

// A coloured shape for now; a texture reference comes later.
struct SpriteComponent {
    Vector2 size{64.0f, 64.0f};
    Color tint = WHITE;
    SpriteShape shape = SpriteShape::Rectangle;
};

// A box for collisions. With physics on (Project Settings), it is the entity's Box2D shape:
// the shape of its Rigidbody, or a static body of its own when the entity has no Rigidbody.
// A trigger is a Box2D sensor: it reports overlaps but doesn't collide.
struct BoxColliderComponent {
    Vector2 offset{0.0f, 0.0f};
    Vector2 size{64.0f, 64.0f};
    bool isTrigger = false;
    float density = 1.0f;     // kg per square metre
    float friction = 0.6f;
    float restitution = 0.0f; // bounciness, 0..1
};

// Box2D body type: Static never moves, Kinematic moves by its velocity only, Dynamic is
// moved by gravity, forces and collisions.
enum class BodyType { Static, Kinematic, Dynamic };

// Makes the entity a Box2D body (Project Settings > Physics). Its Box Collider, if any, is
// the body's shape. The exported game moves dynamic bodies and writes their placement back
// into the entity's transform.
struct RigidbodyComponent {
    BodyType type = BodyType::Dynamic;
    float gravityScale = 1.0f;
    float linearDamping = 0.0f;
    float angularDamping = 0.0f;
    bool fixedRotation = false; // never rotates (e.g. a platformer character)
    bool isBullet = false;      // continuous collision for fast movers
};

// --- UI ---------------------------------------------------------------------------------
// UI elements live under the Canvas root instead of the World. They're laid out on the
// screen rather than placed in the scene, so the World's transform (camera, screen shake)
// never moves them. They have a UIRectTransform instead of a Transform.

// Marks the Canvas: the root of the UI tree. Exactly one exists; it can't be deleted or moved.
// Its rectangle is the screen (with Fit scaling: the design area inside the black bars),
// measured in canvas units: the design resolution's units, the longer side extended to the
// screen's shape.
struct CanvasTag {};

// A UI element's rectangle, relative to its parent's rectangle (the Canvas's or another UI
// element's). All points use y down and (0, 0) at the top-left.
//   anchorMin / anchorMax: points on the parent rectangle, as fractions (0..1). Equal on an
//     axis: the element is pinned to that point (a corner, edge or the centre). Apart: the
//     element stretches with the parent along that axis.
//   pivot: the point of the element (as fractions of its own size) that is placed.
//   position: where the pivot goes, in canvas units, from the anchor point (the anchors'
//     reference point at the pivot's fraction).
//   size: the element's size in canvas units; on a stretched axis it is added to the
//     distance between the anchors instead (0 = exactly fill it, -20 = 10 in on each side).
struct UIRectTransformComponent {
    entt::entity parent = entt::null; // the Canvas or another UI element
    Vector2 anchorMin{0.5f, 0.5f};
    Vector2 anchorMax{0.5f, 0.5f};
    Vector2 pivot{0.5f, 0.5f};
    Vector2 position{0.0f, 0.0f};
    Vector2 size{200.0f, 100.0f};
};

// A filled rectangle covering the element's UIRectTransform.
struct UIRectangleComponent {
    Color color{40, 44, 52, 220};
};

// Text alignment inside a rectangle, per axis: Start = left / top, End = right / bottom.
enum class TextAlign { Start, Center, End };

// Text drawn inside the element's rectangle. Sizes are in canvas units, so text scales with
// the rest of the UI (the exported game multiplies them by the same UI scale).
struct UILabelComponent {
    std::string text = "Label";
    float fontSize = 32.0f;
    float spacing = 2.0f; // between characters
    Color color = WHITE;
    TextAlign alignH = TextAlign::Center;
    TextAlign alignV = TextAlign::Center;
};
