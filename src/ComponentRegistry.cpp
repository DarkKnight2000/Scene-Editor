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

// --- Per-component serialization. Transform's origin is saved by the scene serializer,
// since it refers to another entity. ---

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

json toJson(const BoxColliderComponent& c) {
    return {{"offset", toJson(c.offset)}, {"size", toJson(c.size)},     {"isTrigger", c.isTrigger},
            {"density", c.density},       {"friction", c.friction}, {"restitution", c.restitution}};
}

void fromJson(const json& j, BoxColliderComponent& c) {
    read(j, "offset", c.offset);
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

// The parent is saved by the scene serializer, like Transform's origin.
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
void originRow(Scene& scene, entt::entity e, ui::PropertyGrid& grid) {
    const entt::registry& reg = scene.registry();
    auto& t = scene.registry().get<TransformComponent>(e);

    std::vector<std::string> labels;
    std::vector<entt::entity> candidates;
    int current = 0;
    for (const entt::entity other : scene.entities()) {
        if (other == e || !scene.canSetOrigin(e, other)) {
            continue;
        }
        if (other == t.origin) {
            current = static_cast<int>(candidates.size());
        }
        labels.push_back(reg.get<NameComponent>(other).name);
        candidates.push_back(other);
    }

    if (ui::combo(grid.row("Origin"), &t.origin, labels, &current)) {
        scene.setOrigin(e, candidates[static_cast<std::size_t>(current)]);
    }
}

void drawTransform(Scene& scene, entt::entity e, ui::PropertyGrid& grid) {
    if (scene.isWorld(e)) {
        // The World's transform moves, rotates and scales the whole scene.
        auto& t = scene.registry().get<TransformComponent>(e);
        vec2Row(grid, "Position", t.position, 0.5f);
        ui::dragFloat(grid.row("Rotation"), &t.rotation, 0.5f, nullptr, "%.1f deg");
        vec2Row(grid, "Scale", t.scale, 0.01f);
        return;
    }
    originRow(scene, e, grid);

    auto& t = scene.registry().get<TransformComponent>(e);
    const bool following = !scene.isWorld(t.origin);
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
        const Pose pose = scene.finalPose(e);
        Rectangle x;
        Rectangle y;
        ui::splitHalves(grid.row("Final"), x, y);
        const Color dim = ui::styleColor(TEXT_COLOR_DISABLED);
        ui::label(x, TextFormat("X  %.2f", pose.position.x), dim, true);
        ui::label(y, TextFormat("Y  %.2f", pose.position.y), dim, true);
    }
}

void drawSprite(Scene& scene, entt::entity e, ui::PropertyGrid& grid) {
    auto& s = scene.registry().get<SpriteComponent>(e);
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

void drawBoxCollider(Scene& scene, entt::entity e, ui::PropertyGrid& grid) {
    auto& c = scene.registry().get<BoxColliderComponent>(e);
    vec2Row(grid, "Offset", c.offset, 0.5f);
    vec2Row(grid, "Size", c.size, 0.5f);
    ui::checkbox(grid.row("Is Trigger"), &c.isTrigger);
    // Physics material (Box2D): used by the exported game when physics is on.
    ui::dragFloat(grid.row("Density"), &c.density, 0.01f);
    ui::dragFloat(grid.row("Friction"), &c.friction, 0.01f);
    ui::dragFloat(grid.row("Restitution"), &c.restitution, 0.01f);
    c.density = std::max(c.density, 0.0f);
    c.friction = std::max(c.friction, 0.0f);
    c.restitution = std::clamp(c.restitution, 0.0f, 1.0f);
}

void drawRigidbody(Scene& scene, entt::entity e, ui::PropertyGrid& grid) {
    auto& r = scene.registry().get<RigidbodyComponent>(e);
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
    if (!scene.registry().all_of<BoxColliderComponent>(e)) {
        ui::label(grid.row(""), "Add a Box Collider to give it a shape.", ui::styleColor(TEXT_COLOR_DISABLED));
    }
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

void drawUIRectTransform(Scene& scene, entt::entity e, ui::PropertyGrid& grid) {
    auto& t = scene.registry().get<UIRectTransformComponent>(e);

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
    const Rectangle r = scene.uiRect(e, scene.uiReferenceSize());
    const Color dim = ui::styleColor(TEXT_COLOR_DISABLED);
    ui::label(grid.row("At design"), TextFormat("%.0f, %.0f   %.0f x %.0f", r.x, r.y, r.width, r.height), dim, true);
}

void drawUIRectangle(Scene& scene, entt::entity e, ui::PropertyGrid& grid) {
    colorRow(grid, "Color", scene.registry().get<UIRectangleComponent>(e).color);
}

void alignCombo(Rectangle r, TextAlign& align, const std::vector<std::string>& names) {
    int current = static_cast<int>(align);
    if (ui::combo(r, &align, names, &current)) {
        align = static_cast<TextAlign>(current);
    }
}

void drawUILabel(Scene& scene, entt::entity e, ui::PropertyGrid& grid) {
    auto& l = scene.registry().get<UILabelComponent>(e);
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

template <typename T>
constexpr ComponentType makeType(const char* name, const char* key, int icon, bool removable, ComponentScope scope,
                                 void (*drawFields)(Scene&, entt::entity, ui::PropertyGrid&)) {
    return ComponentType{
        name,
        key,
        icon,
        removable,
        scope,
        [](const entt::registry& reg, entt::entity e) { return reg.all_of<T>(e); },
        [](entt::registry& reg, entt::entity e) { reg.emplace_or_replace<T>(e); },
        [](entt::registry& reg, entt::entity e) { reg.remove<T>(e); },
        drawFields,
        [](const entt::registry& reg, entt::entity e, json& out) { out = toJson(reg.get<T>(e)); },
        [](entt::registry& reg, entt::entity e, const json& in) { fromJson(in, reg.get_or_emplace<T>(e)); },
    };
}

const std::array kTypes{
    makeType<TransformComponent>("Transform", "transform", ICON_CURSOR_MOVE, false, ComponentScope::World,
                                 drawTransform),
    makeType<SpriteComponent>("Sprite", "sprite", ICON_FILETYPE_IMAGE, true, ComponentScope::World, drawSprite),
    makeType<BoxColliderComponent>("Box Collider", "boxCollider", ICON_BOX_CORNERS_BIG, true, ComponentScope::World,
                                   drawBoxCollider),
    makeType<RigidbodyComponent>("Rigidbody", "rigidbody", ICON_PLAYER_JUMP, true, ComponentScope::World, drawRigidbody),
    makeType<UIRectTransformComponent>("UI Rect Transform", "uiRectTransform", ICON_BOX_CENTER, false,
                                       ComponentScope::UI, drawUIRectTransform),
    makeType<UIRectangleComponent>("UI Rectangle", "uiRectangle", ICON_WINDOW, true, ComponentScope::UI,
                                   drawUIRectangle),
    makeType<UILabelComponent>("UI Label", "uiLabel", ICON_TEXT_A, true, ComponentScope::UI, drawUILabel),
};

} // namespace

std::span<const ComponentType> componentTypes() {
    return kTypes;
}

int entityIcon(const Scene& scene, entt::entity entity) {
    if (scene.isWorld(entity)) {
        return ICON_HOUSE;
    }
    if (scene.isCanvas(entity)) {
        return ICON_MONITOR;
    }
    return scene.isUI(entity) ? ICON_BOX_DOTS_BIG : ICON_BOX;
}
