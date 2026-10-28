#include "ComponentRegistry.h"

#include "Components.h"
#include "Scene.h"
#include "Widgets.h"

#include <nlohmann/json.hpp>
#include <raygui.h>

#include <algorithm>
#include <array>
#include <string>
#include <vector>

namespace {

using json = nlohmann::ordered_json; // keeps keys in the order they're written

// --- JSON helpers: missing or mistyped fields leave the value unchanged. ---

json toJson(Vector2 v) {
    return json::array({v.x, v.y});
}

json toJson(Color c) {
    return json::array({c.r, c.g, c.b, c.a});
}

void read(const json& j, const char* key, float& out) {
    if (const auto it = j.find(key); it != j.end() && it->is_number()) {
        out = it->get<float>();
    }
}

void read(const json& j, const char* key, bool& out) {
    if (const auto it = j.find(key); it != j.end() && it->is_boolean()) {
        out = it->get<bool>();
    }
}

void read(const json& j, const char* key, Vector2& out) {
    if (const auto it = j.find(key); it != j.end() && it->is_array() && it->size() == 2 && (*it)[0].is_number() &&
                                     (*it)[1].is_number()) {
        out = {(*it)[0].get<float>(), (*it)[1].get<float>()};
    }
}

void read(const json& j, const char* key, std::string& out) {
    if (const auto it = j.find(key); it != j.end() && it->is_string()) {
        out = it->get<std::string>();
    }
}

constexpr const char* kAlignNames[] = {"start", "center", "end"};

void read(const json& j, const char* key, TextAlign& out) {
    std::string name;
    read(j, key, name);
    for (int i = 0; i < 3; ++i) {
        if (name == kAlignNames[i]) {
            out = static_cast<TextAlign>(i);
        }
    }
}

void read(const json& j, const char* key, Color& out) {
    const auto it = j.find(key);
    if (it == j.end() || !it->is_array() || it->size() != 4) {
        return;
    }
    unsigned char channels[4];
    for (std::size_t i = 0; i < 4; ++i) {
        if (!(*it)[i].is_number_integer()) {
            return;
        }
        channels[i] = static_cast<unsigned char>(std::clamp((*it)[i].get<int>(), 0, 255));
    }
    out = {channels[0], channels[1], channels[2], channels[3]};
}

// --- Per-component serialization. The parent is saved by the scene serializer, since it
// refers to another entity. ---

json toJson(const TransformComponent& t) {
    return {{"position", toJson(t.position)},
            {"rotation", t.rotation},
            {"scale", toJson(t.scale)},
            {"inheritRotation", t.inheritRotation},
            {"inheritScale", t.inheritScale}};
}

void fromJson(const json& j, TransformComponent& t) {
    read(j, "position", t.position);
    read(j, "rotation", t.rotation);
    read(j, "scale", t.scale);
    read(j, "inheritRotation", t.inheritRotation);
    read(j, "inheritScale", t.inheritScale);
}

constexpr const char* kShapeNames[] = {"rectangle", "circle", "triangle"};

json toJson(const SpriteComponent& s) {
    return {{"size", toJson(s.size)},
            {"tint", toJson(s.tint)},
            {"shape", kShapeNames[static_cast<int>(s.shape)]},
            {"export", s.exported}};
}

void fromJson(const json& j, SpriteComponent& s) {
    read(j, "size", s.size);
    read(j, "tint", s.tint);
    std::string shape;
    read(j, "shape", shape);
    for (int i = 0; i < 3; ++i) {
        if (shape == kShapeNames[i]) {
            s.shape = static_cast<SpriteShape>(i);
        }
    }
    read(j, "export", s.exported); // missing in older files: exported
}

constexpr const char* kColliderShapeNames[] = {"box", "circle", "triangle"};

json toJson(const ColliderComponent& c) {
    return {{"shape", kColliderShapeNames[static_cast<int>(c.shape)]},
            {"offset", toJson(c.offset)},
            {"size", toJson(c.size)},
            {"radius", c.radius},
            {"isTrigger", c.isTrigger},
            {"density", c.density},
            {"friction", c.friction},
            {"restitution", c.restitution}};
}

void fromJson(const json& j, ColliderComponent& c) {
    std::string shape;
    read(j, "shape", shape); // missing in older files (box colliders): a box
    for (int i = 0; i < 3; ++i) {
        if (shape == kColliderShapeNames[i]) {
            c.shape = static_cast<ColliderShape>(i);
        }
    }
    read(j, "offset", c.offset);
    read(j, "radius", c.radius);
    read(j, "size", c.size);
    read(j, "isTrigger", c.isTrigger);
    read(j, "density", c.density);
    read(j, "friction", c.friction);
    read(j, "restitution", c.restitution);
}

constexpr const char* kBodyTypeNames[] = {"static", "kinematic", "dynamic"};

json toJson(const RigidbodyComponent& r) {
    return {{"type", kBodyTypeNames[static_cast<int>(r.type)]},
            {"gravityScale", r.gravityScale},
            {"linearDamping", r.linearDamping},
            {"angularDamping", r.angularDamping},
            {"fixedRotation", r.fixedRotation},
            {"isBullet", r.isBullet}};
}

void fromJson(const json& j, RigidbodyComponent& r) {
    std::string type;
    read(j, "type", type);
    for (int i = 0; i < 3; ++i) {
        if (type == kBodyTypeNames[i]) {
            r.type = static_cast<BodyType>(i);
        }
    }
    read(j, "gravityScale", r.gravityScale);
    read(j, "linearDamping", r.linearDamping);
    read(j, "angularDamping", r.angularDamping);
    read(j, "fixedRotation", r.fixedRotation);
    read(j, "isBullet", r.isBullet);
}

json toJson(const PlatformerControllerComponent& p) {
    return {{"moveSpeed", p.moveSpeed},   {"acceleration", p.acceleration}, {"airControl", p.airControl},
            {"jumpHeight", p.jumpHeight}, {"coyoteTime", p.coyoteTime},     {"jumpBuffer", p.jumpBuffer},
            {"jumpCut", p.jumpCut},       {"maxFallSpeed", p.maxFallSpeed}};
}

void fromJson(const json& j, PlatformerControllerComponent& p) {
    read(j, "moveSpeed", p.moveSpeed);
    read(j, "acceleration", p.acceleration);
    read(j, "airControl", p.airControl);
    read(j, "jumpHeight", p.jumpHeight);
    read(j, "coyoteTime", p.coyoteTime);
    read(j, "jumpBuffer", p.jumpBuffer);
    read(j, "jumpCut", p.jumpCut);
    read(j, "maxFallSpeed", p.maxFallSpeed);
}

json toJson(const UIRectTransformComponent& t) {
    return {{"anchorMin", toJson(t.anchorMin)},
            {"anchorMax", toJson(t.anchorMax)},
            {"pivot", toJson(t.pivot)},
            {"position", toJson(t.position)},
            {"size", toJson(t.size)}};
}

void fromJson(const json& j, UIRectTransformComponent& t) {
    read(j, "anchorMin", t.anchorMin);
    read(j, "anchorMax", t.anchorMax);
    read(j, "pivot", t.pivot);
    read(j, "position", t.position);
    read(j, "size", t.size);
}

json toJson(const UILabelComponent& l) {
    return {{"text", l.text},
            {"fontSize", l.fontSize},
            {"spacing", l.spacing},
            {"color", toJson(l.color)},
            {"alignH", kAlignNames[static_cast<int>(l.alignH)]},
            {"alignV", kAlignNames[static_cast<int>(l.alignV)]}};
}

void fromJson(const json& j, UILabelComponent& l) {
    read(j, "text", l.text);
    read(j, "fontSize", l.fontSize);
    read(j, "spacing", l.spacing);
    read(j, "color", l.color);
    read(j, "alignH", l.alignH);
    read(j, "alignV", l.alignV);
}

constexpr const char* kDirectionNames[] = {"horizontal", "vertical"};

json toJson(const UILinearLayoutComponent& l) {
    return {{"direction", kDirectionNames[static_cast<int>(l.direction)]},
            {"spacing", l.spacing},
            {"padding", l.padding},
            {"align", kAlignNames[static_cast<int>(l.align)]}};
}

void fromJson(const json& j, UILinearLayoutComponent& l) {
    std::string direction;
    read(j, "direction", direction);
    l.direction = direction == kDirectionNames[1] ? LayoutDirection::Vertical : LayoutDirection::Horizontal;
    read(j, "spacing", l.spacing);
    read(j, "padding", l.padding);
    read(j, "align", l.align);
}

json toJson(const UIRectangleComponent& r) {
    return {{"color", toJson(r.color)}};
}

void fromJson(const json& j, UIRectangleComponent& r) {
    read(j, "color", r.color);
}

// --- Details panel fields ---

void vec2Row(ui::PropertyGrid& grid, const char* label, Vector2& v, float speed) {
    Rectangle x;
    Rectangle y;
    ui::splitHalves(grid.row(label), x, y);
    ui::dragFloat(x, &v.x, speed, "X");
    ui::dragFloat(y, &v.y, speed, "Y");
}

void colorRow(ui::PropertyGrid& grid, const char* label, Color& color) {
    const Rectangle r = grid.row(label);
    grid.growLastRow(ui::colorField(r, &color) - r.height);
}

// Dropdown of entities `e` may follow: the World plus every entity that isn't `e` or one of its followers.
void originRow(Scene& scene, Entity& e, ui::PropertyGrid& grid) {
    std::vector<std::string> labels;
    std::vector<EntityId> candidates;
    int current = 0;
    for (const Entity& other : scene.entities()) {
        if (other.id == e.id || !scene.canSetOrigin(e.id, other.id)) {
            continue;
        }
        if (other.id == e.parent) {
            current = static_cast<int>(candidates.size());
        }
        labels.push_back(other.name);
        candidates.push_back(other.id);
    }

    if (ui::combo(grid.row("Origin"), &e.parent, labels, &current)) {
        scene.setOrigin(e.id, candidates[static_cast<std::size_t>(current)]);
    }
}

void drawTransform(Scene& scene, Entity& e, ui::PropertyGrid& grid) {
    TransformComponent& t = *e.transform;
    if (scene.isWorld(e.id)) {
        // The World's transform moves, rotates and scales the whole scene.
        vec2Row(grid, "Position", t.position, 0.5f);
        ui::dragFloat(grid.row("Rotation"), &t.rotation, 0.5f, nullptr, "%.1f deg");
        vec2Row(grid, "Scale", t.scale, 0.01f);
        return;
    }
    originRow(scene, e, grid);

    const bool following = !scene.isWorld(e.parent);
    vec2Row(grid, following ? "Offset" : "Position", t.position, 0.5f);
    ui::dragFloat(grid.row("Rotation"), &t.rotation, 0.5f, nullptr, "%.1f deg");
    vec2Row(grid, "Scale", t.scale, 0.01f);

    // Inherit: two toggles, on while the parent's rotation / scale is followed.
    Rectangle rotationToggle;
    Rectangle scaleToggle;
    ui::splitHalves(grid.row("Inherit"), rotationToggle, scaleToggle);
    ui::toggle(rotationToggle, "Rotation", &t.inheritRotation);
    ui::toggle(scaleToggle, "Scale", &t.inheritScale);

    if (following) {
        // Where the entity ends up after its parents (and the World) are applied.
        const Pose pose = scene.finalPose(e.id);
        Rectangle x;
        Rectangle y;
        ui::splitHalves(grid.row("Final"), x, y);
        const Color dim = ui::styleColor(TEXT_COLOR_DISABLED);
        ui::label(x, TextFormat("X  %.2f", pose.position.x), dim, true);
        ui::label(y, TextFormat("Y  %.2f", pose.position.y), dim, true);
    }
}

void drawSprite(Scene&, Entity& e, ui::PropertyGrid& grid) {
    SpriteComponent& s = *e.sprite;
    int shape = static_cast<int>(s.shape);
    if (ui::combo(grid.row("Shape"), &s.shape, {"Rectangle", "Circle", "Triangle"}, &shape)) {
        s.shape = static_cast<SpriteShape>(std::clamp(shape, 0, 2));
    }
    vec2Row(grid, "Size", s.size, 0.5f);
    colorRow(grid, "Tint", s.tint);
    const Rectangle exportRow = grid.row("Export");
    ui::checkbox({exportRow.x, exportRow.y + (exportRow.height - 20.0f) / 2.0f, 20.0f, 20.0f}, &s.exported);
    ui::label({exportRow.x + 28.0f, exportRow.y, exportRow.width - 28.0f, exportRow.height},
              s.exported ? "to the C code" : "designer only", ui::styleColor(TEXT_COLOR_DISABLED));
}

void drawCollider(Scene&, Entity& e, ui::PropertyGrid& grid) {
    ColliderComponent& c = *e.collider;
    int shape = static_cast<int>(c.shape);
    if (ui::combo(grid.row("Shape"), &c.shape, {"Box", "Circle", "Triangle"}, &shape)) {
        c.shape = static_cast<ColliderShape>(std::clamp(shape, 0, 2));
    }
    vec2Row(grid, "Offset", c.offset, 0.5f);
    if (c.shape == ColliderShape::Circle) {
        ui::dragFloat(grid.row("Radius"), &c.radius, 0.5f);
        c.radius = std::max(c.radius, 0.0f);
    } else {
        vec2Row(grid, "Size", c.size, 0.5f);
    }
    ui::checkbox(grid.row("Is Trigger"), &c.isTrigger);
    // Physics material (Box2D): used by the exported game when physics is on.
    ui::dragFloat(grid.row("Density"), &c.density, 0.01f);
    ui::dragFloat(grid.row("Friction"), &c.friction, 0.01f);
    ui::dragFloat(grid.row("Restitution"), &c.restitution, 0.01f);
    c.density = std::max(c.density, 0.0f);
    c.friction = std::max(c.friction, 0.0f);
    c.restitution = std::clamp(c.restitution, 0.0f, 1.0f);
}

void drawRigidbody(Scene&, Entity& e, ui::PropertyGrid& grid) {
    RigidbodyComponent& r = *e.rigidbody;
    int type = static_cast<int>(r.type);
    if (ui::combo(grid.row("Body Type"), &r.type, {"Static", "Kinematic", "Dynamic"}, &type)) {
        r.type = static_cast<BodyType>(std::clamp(type, 0, 2));
    }
    ui::dragFloat(grid.row("Gravity"), &r.gravityScale, 0.01f, nullptr, "%.2f x");
    // Damping: linear (L) and angular (A).
    Rectangle linear;
    Rectangle angular;
    ui::splitHalves(grid.row("Damping"), linear, angular);
    ui::dragFloat(linear, &r.linearDamping, 0.01f, "L");
    ui::dragFloat(angular, &r.angularDamping, 0.01f, "A");
    r.linearDamping = std::max(r.linearDamping, 0.0f);
    r.angularDamping = std::max(r.angularDamping, 0.0f);
    ui::checkbox(grid.row("Fixed Rot."), &r.fixedRotation);
    ui::checkbox(grid.row("Bullet"), &r.isBullet);
    if (!e.collider) {
        ui::label(grid.row(""), "Add a Collider to give it a shape.", ui::styleColor(TEXT_COLOR_DISABLED));
    }
}

void drawPlatformer(Scene&, Entity& e, ui::PropertyGrid& grid) {
    PlatformerControllerComponent& p = *e.platformer;
    ui::dragFloat(grid.row("Move Speed"), &p.moveSpeed, 1.0f, nullptr, "%.0f /s");
    ui::dragFloat(grid.row("Acceleration"), &p.acceleration, 10.0f, nullptr, "%.0f /s^2");
    ui::dragFloat(grid.row("Air Control"), &p.airControl, 0.01f);
    ui::dragFloat(grid.row("Jump Height"), &p.jumpHeight, 1.0f, nullptr, "%.0f");
    ui::dragFloat(grid.row("Coyote Time"), &p.coyoteTime, 0.005f, nullptr, "%.3f s");
    ui::dragFloat(grid.row("Jump Buffer"), &p.jumpBuffer, 0.005f, nullptr, "%.3f s");
    ui::dragFloat(grid.row("Jump Cut"), &p.jumpCut, 0.01f);
    ui::dragFloat(grid.row("Max Fall"), &p.maxFallSpeed, 1.0f, nullptr, "%.0f /s");
    p.moveSpeed = std::max(p.moveSpeed, 0.0f);
    p.acceleration = std::max(p.acceleration, 0.0f);
    p.airControl = std::clamp(p.airControl, 0.0f, 1.0f);
    p.jumpHeight = std::max(p.jumpHeight, 0.0f);
    p.coyoteTime = std::clamp(p.coyoteTime, 0.0f, 1.0f);
    p.jumpBuffer = std::clamp(p.jumpBuffer, 0.0f, 1.0f);
    p.jumpCut = std::clamp(p.jumpCut, 0.0f, 1.0f);
    p.maxFallSpeed = std::max(p.maxFallSpeed, 0.0f);

    // What it needs to move.
    if (!e.rigidbody || e.rigidbody->type != BodyType::Dynamic) {
        ui::label(grid.row(""), "Needs a dynamic Rigidbody.", ui::styleColor(TEXT_COLOR_DISABLED));
    } else if (!e.rigidbody->fixedRotation) {
        ui::label(grid.row(""), "Tip: turn on Fixed Rot. in the Rigidbody.", ui::styleColor(TEXT_COLOR_DISABLED));
    }
    if (!e.collider) {
        ui::label(grid.row(""), "Needs a Collider to stand on things.", ui::styleColor(TEXT_COLOR_DISABLED));
    }
    ui::label(grid.row(""), "Keys: A / D or arrows, Space / W / Up", ui::styleColor(TEXT_COLOR_DISABLED));
}

// Anchor presets: where the element is pinned (or stretched) on its parent. Choosing one
// sets the anchors and the pivot, and puts the element flush against that point or edge.
struct AnchorPreset {
    const char* label;
    Vector2 anchorMin;
    Vector2 anchorMax;
};
constexpr AnchorPreset kAnchorPresets[] = {
    {"Top Left", {0.0f, 0.0f}, {0.0f, 0.0f}},
    {"Top", {0.5f, 0.0f}, {0.5f, 0.0f}},
    {"Top Right", {1.0f, 0.0f}, {1.0f, 0.0f}},
    {"Left", {0.0f, 0.5f}, {0.0f, 0.5f}},
    {"Center", {0.5f, 0.5f}, {0.5f, 0.5f}},
    {"Right", {1.0f, 0.5f}, {1.0f, 0.5f}},
    {"Bottom Left", {0.0f, 1.0f}, {0.0f, 1.0f}},
    {"Bottom", {0.5f, 1.0f}, {0.5f, 1.0f}},
    {"Bottom Right", {1.0f, 1.0f}, {1.0f, 1.0f}},
    {"Stretch Top", {0.0f, 0.0f}, {1.0f, 0.0f}},
    {"Stretch Middle", {0.0f, 0.5f}, {1.0f, 0.5f}},
    {"Stretch Bottom", {0.0f, 1.0f}, {1.0f, 1.0f}},
    {"Stretch Left", {0.0f, 0.0f}, {0.0f, 1.0f}},
    {"Stretch Center", {0.5f, 0.0f}, {0.5f, 1.0f}},
    {"Stretch Right", {1.0f, 0.0f}, {1.0f, 1.0f}},
    {"Stretch All", {0.0f, 0.0f}, {1.0f, 1.0f}},
};
constexpr int kCustomPreset = static_cast<int>(std::size(kAnchorPresets));

bool sameVector(Vector2 a, Vector2 b) {
    return a.x == b.x && a.y == b.y;
}

void applyAnchorPreset(UIRectTransformComponent& t, const AnchorPreset& preset) {
    constexpr float kDefaultSize[2] = {200.0f, 100.0f};
    const bool wasStretched[2] = {t.anchorMin.x != t.anchorMax.x, t.anchorMin.y != t.anchorMax.y};
    t.anchorMin = preset.anchorMin;
    t.anchorMax = preset.anchorMax;
    const bool stretched[2] = {t.anchorMin.x != t.anchorMax.x, t.anchorMin.y != t.anchorMax.y};
    float* size[2] = {&t.size.x, &t.size.y};
    float* pivot[2] = {&t.pivot.x, &t.pivot.y};
    const float anchor[2] = {t.anchorMin.x, t.anchorMin.y};
    for (int axis = 0; axis < 2; ++axis) {
        if (stretched[axis]) {
            *size[axis] = 0.0f; // fill the parent along this axis
            *pivot[axis] = 0.5f;
        } else {
            if (wasStretched[axis]) {
                *size[axis] = kDefaultSize[axis]; // a stretch amount is no use as a fixed size
            }
            *pivot[axis] = anchor[axis]; // flush against the anchored edge (or centred)
        }
    }
    t.position = {0.0f, 0.0f};
}

void drawUIRectTransform(Scene& scene, Entity& e, ui::PropertyGrid& grid) {
    UIRectTransformComponent& t = *e.rectTransform;

    std::vector<std::string> labels;
    int current = kCustomPreset;
    for (int i = 0; i < kCustomPreset; ++i) {
        labels.push_back(kAnchorPresets[i].label);
        if (sameVector(t.anchorMin, kAnchorPresets[i].anchorMin) && sameVector(t.anchorMax, kAnchorPresets[i].anchorMax)) {
            current = i;
        }
    }
    labels.push_back("Custom");
    if (ui::combo(grid.row("Anchor"), &t.anchorMin, labels, &current) && current < kCustomPreset) {
        applyAnchorPreset(t, kAnchorPresets[current]);
    }

    vec2Row(grid, "Anchor Min", t.anchorMin, 0.01f);
    vec2Row(grid, "Anchor Max", t.anchorMax, 0.01f);
    vec2Row(grid, "Pivot", t.pivot, 0.01f);
    vec2Row(grid, "Offset", t.position, 0.5f);
    vec2Row(grid, "Size", t.size, 0.5f);

    // The resulting rectangle on a canvas of the design resolution.
    const Rectangle r = scene.uiRect(e.id, scene.uiReferenceSize());
    const Color dim = ui::styleColor(TEXT_COLOR_DISABLED);
    if (const Entity* parent = scene.find(e.parent); parent != nullptr && parent->uiLinearLayout) {
        ui::label(grid.row(""),
                  parent->uiLinearLayout->direction == LayoutDirection::Horizontal ? "Offset X: set by the parent's layout"
                                                                                  : "Offset Y: set by the parent's layout",
                  dim);
    }
    ui::label(grid.row("At design"), TextFormat("%.0f, %.0f   %.0f x %.0f", r.x, r.y, r.width, r.height), dim, true);
}

void drawUIRectangle(Scene&, Entity& e, ui::PropertyGrid& grid) {
    colorRow(grid, "Color", e.uiRectangle->color);
}

void alignCombo(Rectangle r, TextAlign& align, const std::vector<std::string>& names) {
    int current = static_cast<int>(align);
    if (ui::combo(r, &align, names, &current)) {
        align = static_cast<TextAlign>(current);
    }
}

void drawUILabel(Scene&, Entity& e, ui::PropertyGrid& grid) {
    UILabelComponent& l = *e.uiLabel;
    ui::textField(grid.row("Text"), l.text);
    ui::dragFloat(grid.row("Font Size"), &l.fontSize, 0.25f);
    l.fontSize = std::max(l.fontSize, 1.0f);
    ui::dragFloat(grid.row("Spacing"), &l.spacing, 0.05f);
    colorRow(grid, "Color", l.color);
    Rectangle h;
    Rectangle v;
    ui::splitHalves(grid.row("Align"), h, v);
    alignCombo(h, l.alignH, {"Left", "Center", "Right"});
    alignCombo(v, l.alignV, {"Top", "Middle", "Bottom"});
}

void drawUILinearLayout(Scene&, Entity& e, ui::PropertyGrid& grid) {
    UILinearLayoutComponent& l = *e.uiLinearLayout;
    int direction = static_cast<int>(l.direction);
    if (ui::combo(grid.row("Direction"), &l.direction, {"Horizontal", "Vertical"}, &direction)) {
        l.direction = static_cast<LayoutDirection>(std::clamp(direction, 0, 1));
    }
    ui::dragFloat(grid.row("Spacing"), &l.spacing, 0.5f);
    ui::dragFloat(grid.row("Padding"), &l.padding, 0.5f);
    if (l.direction == LayoutDirection::Horizontal) {
        alignCombo(grid.row("Align"), l.align, {"Left", "Center", "Right"});
    } else {
        alignCombo(grid.row("Align"), l.align, {"Top", "Middle", "Bottom"});
    }
    ui::label(grid.row(""), "Lines up the children in order.", ui::styleColor(TEXT_COLOR_DISABLED));
}

// A component type for the Entity field `Field` (a std::optional of the component).
template <auto Field>
constexpr ComponentType makeType(const char* name, const char* key, int icon, bool removable, ComponentScope scope,
                                 void (*drawFields)(Scene&, Entity&, ui::PropertyGrid&)) {
    return ComponentType{
        name,
        key,
        icon,
        removable,
        scope,
        [](const Entity& e) { return (e.*Field).has_value(); },
        [](Entity& e) { (e.*Field).emplace(); },
        [](Entity& e) { (e.*Field).reset(); },
        drawFields,
        [](const Entity& e, json& out) { out = toJson(*(e.*Field)); },
        [](Entity& e, const json& in) {
            if (!(e.*Field)) {
                (e.*Field).emplace();
            }
            fromJson(in, *(e.*Field));
        },
    };
}

const std::array kTypes{
    makeType<&Entity::transform>("Transform", "transform", ICON_CURSOR_MOVE, false, ComponentScope::World, drawTransform),
    makeType<&Entity::sprite>("Sprite", "sprite", ICON_FILETYPE_IMAGE, true, ComponentScope::World, drawSprite),
    makeType<&Entity::collider>("Collider", "collider", ICON_BOX_CORNERS_BIG, true, ComponentScope::World,
                                drawCollider),
    makeType<&Entity::rigidbody>("Rigidbody", "rigidbody", ICON_PLAYER_JUMP, true, ComponentScope::World, drawRigidbody),
    makeType<&Entity::platformer>("Platformer Controller", "platformer", ICON_PLAYER, true, ComponentScope::World,
                                  drawPlatformer),
    makeType<&Entity::rectTransform>("UI Rect Transform", "uiRectTransform", ICON_BOX_CENTER, false, ComponentScope::UI,
                                     drawUIRectTransform),
    makeType<&Entity::uiRectangle>("UI Rectangle", "uiRectangle", ICON_WINDOW, true, ComponentScope::UI, drawUIRectangle),
    makeType<&Entity::uiLabel>("UI Label", "uiLabel", ICON_TEXT_A, true, ComponentScope::UI, drawUILabel),
    makeType<&Entity::uiLinearLayout>("UI Linear Layout", "uiLinearLayout", ICON_VERTICAL_BARS, true, ComponentScope::UI,
                                      drawUILinearLayout),
};

} // namespace

std::span<const ComponentType> componentTypes() {
    return kTypes;
}

int entityIcon(const Scene& scene, EntityId entity) {
    if (scene.isWorld(entity)) {
        return ICON_HOUSE;
    }
    if (scene.isCanvas(entity)) {
        return ICON_MONITOR;
    }
    return scene.isUI(entity) ? ICON_BOX_DOTS_BIG : ICON_BOX;
}
