#include "SceneExporter.h"

#include "Components.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace {

// C float literal that round-trips: "50.0f", "-0.5f", "1e+06f".
std::string floatLiteral(float value) {
    if (!std::isfinite(value)) {
        value = 0.0f;
    }
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%.9g", static_cast<double>(value));
    std::string text = buffer;
    if (text.find_first_of(".e") == std::string::npos) {
        text += ".0";
    }
    return text + "f";
}

// Names go into /* */ comments: keep them on one line and unable to end the comment.
std::string commentText(const std::string& text) {
    std::string result;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (c == '\n' || c == '\r') {
            result += ' ';
        } else if (c == '*' && i + 1 < text.size() && text[i + 1] == '/') {
            result += "* ";
        } else {
            result += c;
        }
    }
    return result;
}

// Text inside a C string literal.
std::string cString(const std::string& text) {
    std::string result;
    for (const char c : text) {
        if (c == '"' || c == '\\') {
            result += '\\';
            result += c;
        } else if (c == '\n' || c == '\r') {
            result += ' ';
        } else {
            result += c;
        }
    }
    return result;
}

std::string upper(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return text;
}

// Replaces every "{{key}}" in `text`.
std::string fill(std::string text, const std::vector<std::pair<std::string, std::string>>& values) {
    for (const auto& [key, value] : values) {
        const std::string token = "{{" + key + "}}";
        for (std::size_t at = text.find(token); at != std::string::npos; at = text.find(token, at + value.size())) {
            text.replace(at, token.size(), value);
        }
    }
    return text;
}

const char* scaleModeConstant(ScaleMode mode) {
    switch (mode) {
    case ScaleMode::Fit: return "SCENE_SCALE_FIT";
    case ScaleMode::Fill: return "SCENE_SCALE_FILL";
    case ScaleMode::Expand: break;
    }
    return "SCENE_SCALE_EXPAND";
}

std::string vector2(Vector2 v) {
    return "(Vector2){" + floatLiteral(v.x) + ", " + floatLiteral(v.y) + "}";
}

std::string color(Color c) {
    return "(Color){" + std::to_string(c.r) + ", " + std::to_string(c.g) + ", " + std::to_string(c.b) + ", " +
           std::to_string(c.a) + "}";
}

const char* alignConstant(TextAlign align) {
    switch (align) {
    case TextAlign::Start: return "SCENE_ALIGN_START";
    case TextAlign::End: return "SCENE_ALIGN_END";
    case TextAlign::Center: break;
    }
    return "SCENE_ALIGN_CENTER";
}

std::string transformCall(const TransformComponent& t) {
    return "scene_transform(" + floatLiteral(t.position.x) + ", " + floatLiteral(t.position.y) + ", " +
           floatLiteral(t.rotation) + ", " + floatLiteral(t.scale.x) + ", " + floatLiteral(t.scale.y) + ")";
}

// Writes `text` to `path`, creating its folder if needed.
bool writeFile(const std::filesystem::path& path, const std::string& text, std::string& error) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) {
        error = "can't create " + path.parent_path().string() + ": " + ec.message();
        return false;
    }
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
        error = "can't write " + path.string();
        return false;
    }
    file << text;
    if (!file) {
        error = "write failed for " + path.string();
        return false;
    }
    return true;
}

} // namespace

std::filesystem::path exportedExecutable(const std::filesystem::path& outDir, const std::string& projectName) {
#ifdef _WIN32
    return outDir / kBinDir / (sceneIdentifier(projectName) + ".exe");
#else
    return outDir / kBinDir / sceneIdentifier(projectName);
#endif
}

std::string sceneIdentifier(const std::string& sceneName) {
    std::string id;
    for (const char c : sceneName) {
        const auto u = static_cast<unsigned char>(c);
        id += (u < 128 && (std::isalnum(u) || c == '_')) ? c : '_';
    }
    return id.empty() ? std::string("_") : id;
}

namespace {

// Lower-case C identifier for a field named after `name` in the editor ("Health Bar" ->
// health_bar). Not a keyword, and not starting with a digit.
std::string fieldIdentifier(const std::string& name) {
    static const std::unordered_set<std::string> keywords{
        "auto",     "break",   "case",     "char",     "const",    "continue",      "default",     "do",
        "double",   "else",    "enum",     "extern",   "float",    "for",           "goto",        "if",
        "inline",   "int",     "long",     "register", "restrict", "return",        "short",       "signed",
        "sizeof",   "static",  "struct",   "switch",   "typedef",  "union",         "unsigned",    "void",
        "volatile", "while",   "bool",     "true",     "false",    "asm",           "alignas",     "alignof",
        "noreturn", "static_assert", "thread_local"};
    std::string id = sceneIdentifier(name);
    std::transform(id.begin(), id.end(), id.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (std::isdigit(static_cast<unsigned char>(id[0]))) {
        id = "e_" + id;
    }
    if (keywords.count(id) > 0) {
        id += "_";
    }
    return id;
}

// `base`, or base_2, base_3... if taken; the result is marked as taken.
std::string unique(const std::string& base, std::unordered_set<std::string>& used) {
    std::string name = base;
    for (int n = 2; !used.insert(name).second; ++n) {
        name = base + "_" + std::to_string(n);
    }
    return name;
}

// The struct type of a scene: "hud" -> HudScene.
std::string sceneType(const std::string& id) {
    if (std::isdigit(static_cast<unsigned char>(id[0]))) {
        return "Scene_" + id;
    }
    std::string type = id;
    type[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(type[0])));
    return type + "Scene";
}

// The SceneManager member holding a scene ("current" is the manager's own).
std::string managerField(const std::string& id) {
    const std::string field = fieldIdentifier(id);
    return field == "current" ? "current_scene" : field;
}

const char* shapeName(SpriteShape shape) {
    switch (shape) {
    case SpriteShape::Circle: return "circle";
    case SpriteShape::Triangle: return "triangle";
    case SpriteShape::Rectangle: break;
    }
    return "rectangle";
}

const char* bodyTypeConstant(BodyType type) {
    switch (type) {
    case BodyType::Kinematic: return "b2_kinematicBody";
    case BodyType::Dynamic: return "b2_dynamicBody";
    case BodyType::Static: break;
    }
    return "b2_staticBody";
}

std::string uiRectTransformCall(const UIRectTransformComponent& u) {
    return "scene_ui_rect_transform(" + vector2(u.anchorMin) + ", " + vector2(u.anchorMax) + ", " + vector2(u.pivot) +
           ", " + vector2(u.position) + ", " + vector2(u.size) + ")";
}

} // namespace

bool exportScene(const Scene& scene, const std::string& sceneName, const std::filesystem::path& outDir, bool physics,
                 ExportedScene& info, std::string& error) {
    const std::string id = sceneIdentifier(sceneName);
    const std::string prefix = "scene_" + id;
    const std::string type = sceneType(id);
    const std::string guard = "SCENE_" + upper(id) + "_H";
    const entt::registry& registry = scene.registry();
    const auto nameOf = [&](entt::entity e) -> const std::string& { return registry.get<NameComponent>(e).name; };

    // Scene entities in drawing order; scene entities and UI elements in tree order (parents
    // first: poses and rectangles are computed in that order).
    std::vector<entt::entity> drawn;
    for (const entt::entity e : scene.entities()) {
        if (!scene.isRoot(e) && !scene.isUI(e)) {
            drawn.push_back(e);
        }
    }
    std::vector<entt::entity> tree;
    std::vector<entt::entity> ui;
    for (const HierarchyRow& row : scene.hierarchy()) {
        if (!scene.isRoot(row.entity)) {
            (scene.isUI(row.entity) ? ui : tree).push_back(row.entity);
        }
    }

    // A field per entity and UI element, named after it.
    std::unordered_set<std::string> used{"world", "ui_font", "ui_canvas", "ui_scale", "physics_world", "physics_time"};
    std::unordered_map<entt::entity, std::string> field;
    for (const entt::entity e : drawn) {
        field[e] = unique(fieldIdentifier(nameOf(e)), used);
    }
    for (const entt::entity e : ui) {
        field[e] = unique(fieldIdentifier(nameOf(e)), used);
    }
    const auto ref = [&](entt::entity e) { return "s->" + field.at(e); };
    // The parent entity or element; entt::null for the World / the Canvas.
    const auto parentOf = [&](entt::entity e) {
        const entt::entity p = scene.parentOf(e);
        return field.count(p) > 0 ? p : entt::entity{entt::null};
    };
    std::unordered_set<entt::entity> hasChildren;
    for (const entt::entity e : tree) {
        if (const entt::entity p = parentOf(e); p != entt::null) {
            hasChildren.insert(p);
        }
    }

    // Physics bodies: entities with a rigidbody or a collider (a collider alone: static), made
    // in drawing order.
    std::vector<entt::entity> bodies;
    if (physics) {
        for (const entt::entity e : drawn) {
            if (registry.all_of<RigidbodyComponent>(e) || registry.all_of<BoxColliderComponent>(e)) {
                bodies.push_back(e);
            }
        }
    }
    const bool hasBodies = !bodies.empty();
    bool anyCollider = false;
    for (const entt::entity e : drawn) {
        anyCollider = anyCollider || registry.all_of<BoxColliderComponent>(e);
    }

    info = ExportedScene{};
    info.name = sceneName;
    info.id = id;
    info.type = type;
    info.hasBodies = hasBodies;
    info.entityCount = static_cast<int>(drawn.size());
    info.uiCount = static_cast<int>(ui.size());
    for (const entt::entity e : ui) {
        if (info.scoreLabel.empty() && nameOf(e) == "Score" && registry.all_of<UILabelComponent>(e)) {
            info.scoreLabel = field.at(e);
        }
        if (info.minimapElement.empty() && nameOf(e) == "Minimap") {
            info.minimapElement = field.at(e);
        }
    }
    for (const entt::entity e : bodies) {
        const auto* rb = registry.try_get<RigidbodyComponent>(e);
        if (info.playerBody.empty() && nameOf(e) == "Player" && rb != nullptr && rb->type == BodyType::Dynamic) {
            info.playerBody = field.at(e);
        }
    }

    // --- Header: the scene struct ---
    std::ostringstream fields;
    if (!drawn.empty()) {
        fields << "\n    /* Entities, in drawing order */\n";
    }
    for (const entt::entity e : drawn) {
        const auto* sprite = registry.try_get<SpriteComponent>(e);
        std::string what = sprite != nullptr && sprite->exported ? shapeName(sprite->shape) : "no sprite";
        if (physics && registry.all_of<RigidbodyComponent>(e)) {
            what += ", rigidbody";
        }
        if (registry.all_of<BoxColliderComponent>(e)) {
            what += ", collider";
        }
        fields << "    SceneEntity " << field.at(e) << "; /* \"" << commentText(nameOf(e)) << "\": " << what << " */\n";
    }
    if (!ui.empty()) {
        fields << "\n    /* UI elements, parents before children (drawn in this order) */\n";
    }
    for (const entt::entity e : ui) {
        std::string what;
        if (registry.all_of<UIRectangleComponent>(e)) {
            what = "rectangle";
        }
        if (registry.all_of<UILabelComponent>(e)) {
            what += what.empty() ? "label" : ", label";
        }
        fields << "    SceneUIElement " << field.at(e) << "; /* \"" << commentText(nameOf(e)) << "\""
               << (what.empty() ? std::string() : ": " + what) << " */\n";
    }
    if (hasBodies) {
        fields << "\n    /* Physics: the scene's Box2D world. Bodies and shapes carry their SceneEntity * as\n"
                  "       user data (e.g. for b2World_GetContactEvents after a step). */\n"
                  "    b2WorldId physics_world;\n"
                  "    float physics_time; /* time not stepped yet */\n";
    }

    const std::string h = fill(R"GEN({{marker}} from scene "{{name}}". Do not edit:
// re-export from the editor instead.
#ifndef {{guard}}
#define {{guard}}

#include "scene_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Scene "{{name}}": every entity and UI element is a field, named after it in the editor.
   Read their placements to draw your own things (entity.pose, element.rect), or change
   their fields (transform, sprite, visible, layout, label...). The hierarchy is fixed. */
typedef struct {{type}} {
    SceneTransform world; /* the World root: moves, rotates and zooms the whole scene */
    const Font *ui_font;  /* labels' font (a label's own font wins); NULL: raylib's default */
    Rectangle ui_canvas;  /* from the last {{prefix}}_update_ui: the Canvas in window pixels */
    float ui_scale;       /* ...and window pixels per canvas unit */
{{fields}}} {{type}};

/* Fills in the designed values{{init_physics}}, then the poses. Everything in `s` is
   overwritten. */
void {{prefix}}_init({{type}} *s);

/* Frees what init created{{deinit_physics}}. */
void {{prefix}}_deinit({{type}} *s);

/* Recomputes every entity's pose (parents before children), e.g. after moving entities.
   {{prefix}}_draw calls it. */
void {{prefix}}_update_poses({{type}} *s);

/* Updates the poses and draws the sprites in drawing order. Call between scene_begin(mode)
   and scene_end(). */
void {{prefix}}_draw({{type}} *s);

/* Debug view: outlines of the box colliders (between scene_begin and scene_end). */
void {{prefix}}_draw_colliders({{type}} *s, Color color);

/* Lays out the UI for the window and `mode` (the mode given to scene_begin): every
   element's rect, in window pixels, and ui_canvas / ui_scale. {{prefix}}_draw_ui calls it;
   call it yourself to read rects before drawing the UI. */
void {{prefix}}_update_ui({{type}} *s, SceneScaleMode mode);

/* Updates the UI layout and draws the UI rectangles and labels on top of everything. Call
   after scene_end(), then draw your own widgets into the elements' rects. */
void {{prefix}}_draw_ui({{type}} *s, SceneScaleMode mode);
{{step_decl}}
#ifdef __cplusplus
}
#endif

#endif /* {{guard}} */
)GEN",
                               {{"marker", kGeneratedMarker},
                                {"name", commentText(sceneName)},
                                {"guard", guard},
                                {"type", type},
                                {"prefix", prefix},
                                {"fields", fields.str()},
                                {"init_physics", hasBodies ? " and creates the physics world and bodies" : ""},
                                {"deinit_physics", hasBodies ? " (the physics world)" : " (nothing: the scene has no physics)"},
                                {"step_decl", hasBodies ? "\n/* Advances the physics by `dt` seconds (e.g. GetFrameTime()) in fixed steps of\n"
                                                          "   SCENE_PHYSICS_TIME_STEP, then moves the entities of moving bodies. Move bodies\n"
                                                          "   with Box2D (entity.body); changing a transform doesn't move its body. */\n"
                                                          "void " + prefix + "_step(" + type + " *s, float dt);\n"
                                                        : ""}});

    // --- Source ---
    std::ostringstream init;
    const auto& world = registry.get<TransformComponent>(scene.world());
    init << "    memset(s, 0, sizeof(*s));\n"
         << "    s->world = " << transformCall(world) << ";\n";
    const auto inheritLines = [&](const std::string& target, const TransformComponent& t) {
        if (!t.inheritRotation) {
            init << "    " << target << ".inherit_rotation = false;\n";
        }
        if (!t.inheritScale) {
            init << "    " << target << ".inherit_scale = false;\n";
        }
    };
    inheritLines("s->world", world);
    for (const entt::entity e : drawn) {
        const auto& t = registry.get<TransformComponent>(e);
        init << "\n    " << ref(e) << ".transform = " << transformCall(t) << ";\n";
        inheritLines(ref(e) + ".transform", t);
        init << "    " << ref(e) << ".visible = true;\n";
        if (const auto* sprite = registry.try_get<SpriteComponent>(e); sprite != nullptr && sprite->exported) {
            init << "    " << ref(e) << ".sprite.size = " << vector2(sprite->size) << ";\n"
                 << "    " << ref(e) << ".sprite.tint = " << color(sprite->tint) << ";\n";
        }
        if (const auto* c = registry.try_get<BoxColliderComponent>(e)) {
            init << "    " << ref(e) << ".collider = scene_box_collider(" << vector2(c->offset) << ", " << vector2(c->size)
                 << ", " << (c->isTrigger ? "true" : "false") << ", " << floatLiteral(c->density) << ", "
                 << floatLiteral(c->friction) << ", " << floatLiteral(c->restitution) << ");\n";
        }
    }
    for (const entt::entity e : ui) {
        init << "\n    " << ref(e) << ".layout = " << uiRectTransformCall(registry.get<UIRectTransformComponent>(e)) << ";\n"
             << "    " << ref(e) << ".visible = true;\n";
        if (const auto* rect = registry.try_get<UIRectangleComponent>(e)) {
            init << "    " << ref(e) << ".color = " << color(rect->color) << ";\n";
        }
        if (const auto* label = registry.try_get<UILabelComponent>(e)) {
            init << "    scene_set_ui_label(&" << ref(e) << ".label, \"" << cString(label->text) << "\", "
                 << floatLiteral(label->fontSize) << ", " << floatLiteral(label->spacing) << ", "
                 << alignConstant(label->alignH) << ", " << alignConstant(label->alignV) << ", " << color(label->color)
                 << ");\n";
        }
    }

    if (hasBodies) {
        // Bodies are placed in the World's own space: poses from the parent chain, without the
        // World transform. Only bodies and their ancestors need one.
        std::unordered_set<entt::entity> needed;
        for (const entt::entity e : bodies) {
            for (entt::entity a = e; a != entt::null; a = parentOf(a)) {
                needed.insert(a);
            }
        }
        init << "\n    /* Physics: the world, and the bodies at their placement in the World's own space. */\n"
             << "    s->physics_world = scene_physics_create_world();\n"
             << "    {\n"
             << "        b2BodyDef def;\n";
        for (const entt::entity e : tree) {
            if (needed.count(e) == 0) {
                continue;
            }
            const entt::entity p = parentOf(e);
            init << "        const ScenePose pp_" << field.at(e) << " = scene_child_pose("
                 << (p == entt::null ? std::string("scene_pose_identity()") : "pp_" + field.at(p)) << ", " << ref(e)
                 << ".transform);\n";
        }
        for (const entt::entity e : bodies) {
            const auto* rb = registry.try_get<RigidbodyComponent>(e);
            const std::string pp = "pp_" + field.at(e);
            init << "        def = scene_body_def(" << pp << ", "
                 << bodyTypeConstant(rb != nullptr ? rb->type : BodyType::Static) << ", &" << ref(e) << ");\n";
            if (rb != nullptr) {
                const RigidbodyComponent defaults;
                if (rb->gravityScale != defaults.gravityScale) {
                    init << "        def.gravityScale = " << floatLiteral(rb->gravityScale) << ";\n";
                }
                if (rb->linearDamping != defaults.linearDamping) {
                    init << "        def.linearDamping = " << floatLiteral(rb->linearDamping) << ";\n";
                }
                if (rb->angularDamping != defaults.angularDamping) {
                    init << "        def.angularDamping = " << floatLiteral(rb->angularDamping) << ";\n";
                }
                if (rb->fixedRotation) {
                    init << "        def.fixedRotation = true;\n";
                }
                if (rb->isBullet) {
                    init << "        def.isBullet = true;\n";
                }
            }
            init << "        " << ref(e) << ".body = b2CreateBody(s->physics_world, &def);\n";
            if (registry.all_of<BoxColliderComponent>(e)) {
                init << "        scene_add_box_collider(" << ref(e) << ".body, " << pp << ".scale, &" << ref(e)
                     << ".collider, &" << ref(e) << ");\n";
            }
        }
        init << "    }\n";
    }
    init << "\n    " << prefix << "_update_poses(s);\n";

    // Poses: straight-line, parents first. A parent's rotation is turned into a cosine and
    // sine once, for all its children.
    std::ostringstream poses;
    if (tree.empty()) {
        poses << "    (void)s;\n";
    } else {
        poses << "    const ScenePose world = scene_pose_of(s->world);\n"
              << "    const SceneRot world_rot = scene_rot(world.rotation);\n";
    }
    for (const entt::entity e : tree) {
        const entt::entity p = parentOf(e);
        poses << "    " << ref(e) << ".pose = scene_compose("
              << (p == entt::null ? std::string("world, world_rot") : ref(p) + ".pose, rot_" + field.at(p)) << ", "
              << ref(e) << ".transform);\n";
        if (hasChildren.count(e) > 0) {
            poses << "    const SceneRot rot_" << field.at(e) << " = scene_rot(" << ref(e) << ".pose.rotation);\n";
        }
    }

    // Drawing: one call per exported sprite, its shape fixed at export.
    std::ostringstream draw;
    draw << "    " << prefix << "_update_poses(s);\n";
    for (const entt::entity e : drawn) {
        const auto* sprite = registry.try_get<SpriteComponent>(e);
        if (sprite == nullptr || !sprite->exported) {
            continue;
        }
        draw << "    if (" << ref(e) << ".visible) {\n"
             << "        scene_draw_" << shapeName(sprite->shape) << "(&" << ref(e) << ".sprite, " << ref(e) << ".pose);\n"
             << "    }\n";
    }

    std::ostringstream colliders;
    if (!anyCollider) {
        colliders << "    (void)s;\n"
                  << "    (void)color;\n";
    }
    for (const entt::entity e : drawn) {
        if (registry.all_of<BoxColliderComponent>(e)) {
            colliders << "    scene_draw_box_lines(scene_box(" << ref(e) << ".pose, " << ref(e) << ".collider.offset, "
                      << ref(e) << ".collider.size), 2.0f, color);\n";
        }
    }

    // UI layout, parents first; then drawing in the same order.
    std::ostringstream updateUI;
    updateUI << "    s->ui_canvas = scene_ui_canvas(mode);\n"
             << "    s->ui_scale = scene_ui_scale(s->ui_canvas);\n";
    for (const entt::entity e : ui) {
        const entt::entity p = parentOf(e);
        updateUI << "    " << ref(e) << ".rect = scene_ui_layout("
                 << (p == entt::null ? std::string("s->ui_canvas") : ref(p) + ".rect") << ", &" << ref(e)
                 << ".layout, s->ui_scale);\n";
    }
    std::ostringstream drawUI;
    bool anyLabel = false;
    for (const entt::entity e : ui) {
        anyLabel = anyLabel || registry.all_of<UILabelComponent>(e);
    }
    if (anyLabel) {
        drawUI << "    const Font font = s->ui_font != NULL ? *s->ui_font : GetFontDefault();\n";
    }
    drawUI << "    " << prefix << "_update_ui(s, mode);\n";
    for (const entt::entity e : ui) {
        const bool rect = registry.all_of<UIRectangleComponent>(e);
        const bool label = registry.all_of<UILabelComponent>(e);
        if (!rect && !label) {
            continue;
        }
        drawUI << "    if (" << ref(e) << ".visible) {\n";
        if (rect) {
            drawUI << "        DrawRectangleRec(" << ref(e) << ".rect, " << ref(e) << ".color);\n";
        }
        if (label) {
            drawUI << "        scene_draw_ui_label(&" << ref(e) << ".label, " << ref(e) << ".rect, s->ui_scale, font);\n";
        }
        drawUI << "    }\n";
    }

    std::ostringstream step;
    if (hasBodies) {
        step << "\nvoid " << prefix << "_step(" << type << " *s, float dt) {\n"
             << "    scene_physics_advance(s->physics_world, &s->physics_time, dt);\n";
        // Moving bodies back into their entities, parents first: a child's transform is
        // relative to its parent's new placement.
        bool header = false;
        for (const entt::entity e : tree) {
            const auto* rb = registry.try_get<RigidbodyComponent>(e);
            if (rb == nullptr || rb->type == BodyType::Static) {
                continue;
            }
            if (!header) {
                step << "    /* Moving bodies back into their entities, parents first. */\n";
                header = true;
            }
            std::string base = "scene_pose_identity()";
            std::vector<entt::entity> chain;
            for (entt::entity a = parentOf(e); a != entt::null; a = parentOf(a)) {
                chain.push_back(a);
            }
            for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
                base = "scene_child_pose(" + base + ", " + ref(*it) + ".transform)";
            }
            step << "    scene_body_to_entity(&" << ref(e) << ", " << base << ");\n";
        }
        step << "}\n";
    }

    const std::string c = fill(R"GEN({{marker}} from scene "{{name}}". Do not edit:
// re-export from the editor instead.
#include "{{id}}.h"

#include <string.h>

void {{prefix}}_init({{type}} *s) {
{{init}}}

void {{prefix}}_deinit({{type}} *s) {
{{deinit}}}

void {{prefix}}_update_poses({{type}} *s) {
{{poses}}}

void {{prefix}}_draw({{type}} *s) {
{{draw}}}

void {{prefix}}_draw_colliders({{type}} *s, Color color) {
{{colliders}}}

void {{prefix}}_update_ui({{type}} *s, SceneScaleMode mode) {
{{update_ui}}}

void {{prefix}}_draw_ui({{type}} *s, SceneScaleMode mode) {
{{draw_ui}}}
{{step}})GEN",
                               {{"marker", kGeneratedMarker},
                                {"name", commentText(sceneName)},
                                {"id", id},
                                {"type", type},
                                {"prefix", prefix},
                                {"init", init.str()},
                                {"deinit", hasBodies ? "    if (B2_IS_NON_NULL(s->physics_world)) {\n"
                                                       "        b2DestroyWorld(s->physics_world); /* and every body in it */\n"
                                                       "        s->physics_world = b2_nullWorldId;\n"
                                                       "    }\n"
                                                     : "    (void)s;\n"},
                                {"poses", poses.str()},
                                {"draw", draw.str()},
                                {"colliders", colliders.str()},
                                {"update_ui", updateUI.str()},
                                {"draw_ui", drawUI.str()},
                                {"step", step.str()}});

    const std::filesystem::path dir = outDir / kGeneratedDir;
    return writeFile(dir / (id + ".h"), h, error) && writeFile(dir / (id + ".c"), c, error);
}

bool writeScenesHeader(const std::vector<std::string>& sceneNames, const std::filesystem::path& outDir,
                       std::string& error) {
    std::ostringstream h;
    h << kGeneratedMarker << ": every exported scene. Do not edit.\n"
      << "#ifndef SCENES_H\n"
      << "#define SCENES_H\n\n"
      << "#include \"scene_common.h\"\n"
      << "#include \"scene_manager.h\"\n\n";
    for (const std::string& name : sceneNames) {
        h << "#include \"" << sceneIdentifier(name) << ".h\" /* scene_" << sceneIdentifier(name) << "_* */\n";
    }
    h << "\n#endif /* SCENES_H */\n";
    return writeFile(outDir / kGeneratedDir / "scenes.h", h.str(), error);
}

namespace {
// SceneId enum constant of a scene: "my level" -> SCENE_ID_MY_LEVEL.
std::string sceneIdConstant(const std::string& sceneName) {
    return "SCENE_ID_" + upper(sceneIdentifier(sceneName));
}
} // namespace

bool writeSceneManager(const std::vector<ExportedScene>& scenes, const std::filesystem::path& outDir, std::string& error) {
    std::ostringstream includes;
    std::ostringstream ids;
    std::ostringstream members;
    std::ostringstream names;
    std::ostringstream init;
    std::ostringstream update;
    std::ostringstream draw;
    std::ostringstream colliders;
    std::ostringstream drawUI;
    std::ostringstream reload;
    std::ostringstream deinit;
    for (const ExportedScene& s : scenes) {
        const std::string prefix = "scene_" + s.id;
        const std::string member = managerField(s.id);
        const std::string constant = sceneIdConstant(s.name);
        includes << "#include \"" << s.id << ".h\"\n";
        ids << "    " << constant << ", /* \"" << commentText(s.name) << "\" */\n";
        members << "    " << s.type << " " << member << ";\n";
        names << "    \"" << cString(s.name) << "\",\n";
        init << "    " << prefix << "_init(&manager->" << member << ");\n";
        if (s.hasBodies) {
            update << "    case " << constant << ": " << prefix << "_step(&manager->" << member << ", dt); break;\n";
        }
        draw << "    case " << constant << ": " << prefix << "_draw(&manager->" << member << "); break;\n";
        colliders << "    case " << constant << ": " << prefix << "_draw_colliders(&manager->" << member
                  << ", color); break;\n";
        drawUI << "    case " << constant << ": " << prefix << "_draw_ui(&manager->" << member << ", mode); break;\n";
        reload << "    case " << constant << ": {\n"
               << "        const Font *font = manager->" << member << ".ui_font;\n"
               << "        " << prefix << "_deinit(&manager->" << member << ");\n"
               << "        " << prefix << "_init(&manager->" << member << ");\n"
               << "        manager->" << member << ".ui_font = font;\n"
               << "        break;\n"
               << "    }\n";
        deinit << "    " << prefix << "_deinit(&manager->" << member << ");\n";
    }
    const std::vector<std::pair<std::string, std::string>> values{
        {"marker", kGeneratedMarker}, {"includes", includes.str()},   {"ids", ids.str()},       {"members", members.str()},
        {"names", names.str()},       {"init", init.str()},           {"update", update.str()}, {"draw", draw.str()},
        {"colliders", colliders.str()}, {"draw_ui", drawUI.str()},    {"reload", reload.str()}, {"deinit", deinit.str()},
    };

    const std::string h = fill(R"GEN({{marker}}: every scene of the project in one place. Do not edit.
#ifndef SCENE_MANAGER_H
#define SCENE_MANAGER_H

#include "scene_common.h"
{{includes}}
#ifdef __cplusplus
extern "C" {
#endif

/* Every exported scene, in export order. */
typedef enum SceneId {
{{ids}}    SCENE_ID_COUNT
} SceneId;

/* Scene names, by SceneId. */
extern const char *const scene_names[SCENE_ID_COUNT];

/* Every scene's struct, and which one is shown. Set `current` to a SceneId to switch
   scenes; reach a scene's entities through its member, e.g. manager.hud.score.rect. */
typedef struct SceneManager {
{{members}}    int current;
} SceneManager;

/* Initialises every scene, and shows `first`. */
void scene_manager_init(SceneManager *manager, int first);
/* Advances the current scene's physics (scenes with bodies). */
void scene_manager_update(SceneManager *manager, float dt);
/* Draws the current scene (between scene_begin and scene_end). */
void scene_manager_draw(SceneManager *manager);
/* Debug view: the current scene's box colliders (between scene_begin and scene_end). */
void scene_manager_draw_colliders(SceneManager *manager, Color color);
/* Lays out and draws the current scene's UI (after scene_end). */
void scene_manager_draw_ui(SceneManager *manager, SceneScaleMode mode);
/* Rebuilds a scene from its exported data (its ui_font is kept). */
void scene_manager_reload(SceneManager *manager, int id);
/* Frees what every scene's init created. */
void scene_manager_deinit(SceneManager *manager);

#ifdef __cplusplus
}
#endif

#endif /* SCENE_MANAGER_H */
)GEN",
                               values);

    const std::string c = fill(R"GEN({{marker}}: every scene of the project in one place. Do not edit.
#include "scene_manager.h"

const char *const scene_names[SCENE_ID_COUNT] = {
{{names}}};

void scene_manager_init(SceneManager *manager, int first) {
{{init}}    manager->current = first >= 0 && first < SCENE_ID_COUNT ? first : 0;
}

void scene_manager_update(SceneManager *manager, float dt) {
    switch (manager->current) {
{{update}}    default: break;
    }
    (void)dt;
}

void scene_manager_draw(SceneManager *manager) {
    switch (manager->current) {
{{draw}}    default: break;
    }
}

void scene_manager_draw_colliders(SceneManager *manager, Color color) {
    switch (manager->current) {
{{colliders}}    default: break;
    }
}

void scene_manager_draw_ui(SceneManager *manager, SceneScaleMode mode) {
    switch (manager->current) {
{{draw_ui}}    default: break;
    }
}

void scene_manager_reload(SceneManager *manager, int id) {
    switch (id) {
{{reload}}    default: break;
    }
}

void scene_manager_deinit(SceneManager *manager) {
{{deinit}}}
)GEN",
                               values);

    const std::filesystem::path dir = outDir / kGeneratedDir;
    return writeFile(dir / "scene_manager.h", h, error) && writeFile(dir / "scene_manager.c", c, error);
}

namespace {
// The screen-scaling parts of scene_common for the exported scale modes only.
struct ScalingCode {
    std::string modeEnum;   // enum entries
    std::string modeList;   // initialisers of scene_scale_modes[]
    std::string source;     // scene_camera, scene_begin, scene_end (and Fit's helpers)
    std::string uiCanvas;   // the start of scene_update_ui: where the canvas is
};

ScalingCode scalingCode(const ExportSettings& options) {
    const bool expand = options.scaleModes[0];
    const bool fit = options.scaleModes[1];
    const bool fill = options.scaleModes[2];
    ScalingCode code;
    const char* const notes[] = {
        "whole design area visible; extra screen space shows more scene",
        "whole design area visible; extra space becomes black bars",
        "window fully covered; the design area is cropped to fit",
    };
    const char* const names[] = {"SCENE_SCALE_EXPAND", "SCENE_SCALE_FIT", "SCENE_SCALE_FILL"};
    for (int i = 0; i < 3; ++i) {
        if (options.scaleModes[i]) {
            code.modeEnum += std::string("    ") + names[i] + " = " + std::to_string(i) + ", /* " + notes[i] + " */\n";
            code.modeList += std::string(code.modeList.empty() ? "" : ", ") + names[i];
        }
    }

    std::string zoom;
    if (fill && (expand || fit)) {
        zoom = "    /* Expand and Fit fit the whole design area (smaller scale); Fill covers the window (larger scale). */\n"
               "    if (mode == SCENE_SCALE_FILL) {\n"
               "        camera.zoom = scale_x > scale_y ? scale_x : scale_y;\n"
               "    } else {\n"
               "        camera.zoom = scale_x < scale_y ? scale_x : scale_y;\n"
               "    }\n";
    } else if (fill) {
        zoom = "    /* Fill covers the window (the larger scale). */\n"
               "    camera.zoom = scale_x > scale_y ? scale_x : scale_y;\n"
               "    (void)mode;\n";
    } else {
        zoom = std::string("    /* ") + (expand && fit ? "Expand and Fit fit" : expand ? "Expand fits" : "Fit fits") +
               " the whole design area (the smaller scale). */\n"
               "    camera.zoom = scale_x < scale_y ? scale_x : scale_y;\n"
               "    (void)mode;\n";
    }
    code.source = "Camera2D scene_camera(SceneScaleMode mode) {\n"
                  "    const float screen_w = (float)GetScreenWidth();\n"
                  "    const float screen_h = (float)GetScreenHeight();\n"
                  "    const float scale_x = screen_w / SCENE_DESIGN_WIDTH;\n"
                  "    const float scale_y = screen_h / SCENE_DESIGN_HEIGHT;\n"
                  "    Camera2D camera = {0};\n"
                  "    camera.offset.x = screen_w * 0.5f;\n"
                  "    camera.offset.y = screen_h * 0.5f;\n" +
                  zoom +
                  "    return camera;\n"
                  "}\n\n";
    if (fit) {
        code.source = "/* Mode of the current scene_begin(), so scene_end() knows whether to draw Fit's bars. */\n"
                      "static SceneScaleMode active_mode = SCENE_DEFAULT_SCALE_MODE;\n\n" +
                      code.source +
                      "/* The design area in window pixels, for Fit's clipping and bars. */\n"
                      "static Rectangle design_rect_on_screen(Camera2D camera) {\n"
                      "    const float w = SCENE_DESIGN_WIDTH * camera.zoom;\n"
                      "    const float h = SCENE_DESIGN_HEIGHT * camera.zoom;\n"
                      "    Rectangle r;\n"
                      "    r.x = camera.offset.x - w * 0.5f;\n"
                      "    r.y = camera.offset.y - h * 0.5f;\n"
                      "    r.width = w;\n"
                      "    r.height = h;\n"
                      "    return r;\n"
                      "}\n\n"
                      "void scene_begin(SceneScaleMode mode) {\n"
                      "    const Camera2D camera = scene_camera(mode);\n"
                      "    active_mode = mode;\n"
                      "    if (mode == SCENE_SCALE_FIT) {\n"
                      "        const Rectangle r = design_rect_on_screen(camera);\n"
                      "        BeginScissorMode((int)r.x, (int)r.y, (int)r.width, (int)r.height);\n"
                      "    }\n"
                      "    BeginMode2D(camera);\n"
                      "}\n\n"
                      "void scene_end(void) {\n"
                      "    EndMode2D();\n"
                      "    if (active_mode == SCENE_SCALE_FIT) {\n"
                      "        const Rectangle r = design_rect_on_screen(scene_camera(SCENE_SCALE_FIT));\n"
                      "        const int screen_w = GetScreenWidth();\n"
                      "        const int screen_h = GetScreenHeight();\n"
                      "        EndScissorMode();\n"
                      "        /* Black bars outside the design area. */\n"
                      "        DrawRectangle(0, 0, screen_w, (int)r.y, BLACK);\n"
                      "        DrawRectangle(0, (int)(r.y + r.height), screen_w, screen_h, BLACK);\n"
                      "        DrawRectangle(0, 0, (int)r.x, screen_h, BLACK);\n"
                      "        DrawRectangle((int)(r.x + r.width), 0, screen_w, screen_h, BLACK);\n"
                      "    }\n"
                      "}\n";
    } else {
        code.source += "void scene_begin(SceneScaleMode mode) {\n"
                       "    BeginMode2D(scene_camera(mode));\n"
                       "}\n\n"
                       "void scene_end(void) {\n"
                       "    EndMode2D();\n"
                       "}\n";
    }

    const std::string window = "        scene->ui_canvas.x = 0.0f;\n"
                               "        scene->ui_canvas.y = 0.0f;\n"
                               "        scene->ui_canvas.width = (float)GetScreenWidth();\n"
                               "        scene->ui_canvas.height = (float)GetScreenHeight();\n";
    if (fit && (expand || fill)) {
        code.uiCanvas = "    /* The Canvas: the window, or with Fit the design area inside the bars. One canvas unit\n"
                        "       is ui_scale pixels: the design area just fits inside the canvas. */\n"
                        "    if (mode == SCENE_SCALE_FIT) {\n"
                        "        scene->ui_canvas = design_rect_on_screen(scene_camera(SCENE_SCALE_FIT));\n"
                        "    } else {\n" +
                        window + "    }\n";
    } else if (fit) {
        code.uiCanvas = "    /* The Canvas: the design area inside Fit's bars. One canvas unit is ui_scale pixels. */\n"
                        "    scene->ui_canvas = design_rect_on_screen(scene_camera(SCENE_SCALE_FIT));\n"
                        "    (void)mode;\n";
    } else {
        code.uiCanvas = "    /* The Canvas: the window. One canvas unit is ui_scale pixels: the design area just\n"
                        "       fits inside the canvas. */\n"
                        "    {\n" +
                        window + "    }\n"
                                 "    (void)mode;\n";
    }
    return code;
}
} // namespace

bool writeSceneCommon(const std::filesystem::path& outDir, const DisplaySettings& display,
                      const ExportSettings& options, const PhysicsSettings& physics, std::string& error) {
    const ScalingCode scaling = scalingCode(options);
    const std::vector<std::pair<std::string, std::string>> values{
        {"marker", kGeneratedMarker},
        {"width", floatLiteral(static_cast<float>(display.width))},
        {"height", floatLiteral(static_cast<float>(display.height))},
        {"mode", scaleModeConstant(display.scaleMode)},
        {"mode_enum", scaling.modeEnum},
        {"mode_count", std::to_string(options.modeCount())},
        {"mode_list", scaling.modeList},
        {"scaling_source", scaling.source},
        {"ui_canvas", scaling.uiCanvas},
        {"physics", physics.enabled ? "1" : "0"},
        {"pixels_per_meter", floatLiteral(physics.pixelsPerMeter)},
        {"gravity_x", floatLiteral(physics.gravityX)},
        {"gravity_y", floatLiteral(physics.gravityY)},
    };

    const std::string h = fill(R"GEN({{marker}}: code shared by every exported scene. Do not edit.
#ifndef SCENE_COMMON_H
#define SCENE_COMMON_H

#include <math.h>
#include <stdbool.h>

#include <raylib.h>

/* Box2D physics (Project Settings > Physics). Box2D's headers need C11. */
#define SCENE_PHYSICS {{physics}}
#if SCENE_PHYSICS
#include <box2d/box2d.h>
#define SCENE_PIXELS_PER_METER {{pixels_per_meter}} /* scene units in one metre (Box2D works in metres) */
#define SCENE_GRAVITY_X {{gravity_x}}        /* metres per second squared, y down */
#define SCENE_GRAVITY_Y {{gravity_y}}
#define SCENE_PHYSICS_TIME_STEP (1.0f / 60.0f) /* fixed step of the scenes' _step functions */
#define SCENE_PHYSICS_SUB_STEPS 4
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------------------
   Screen scaling: the scene is designed for one resolution and fitted to any window
   --------------------------------------------------------------------------------------- */

/* Design resolution from the project settings: the scene area the game is laid out for,
   centred on the scene origin. */
#define SCENE_DESIGN_WIDTH {{width}}
#define SCENE_DESIGN_HEIGHT {{height}}

/* How the design area is fitted to a window of another size or shape. Only the modes
   chosen in Project Settings > Export are exported (and only their code). */
typedef enum SceneScaleMode {
{{mode_enum}}} SceneScaleMode;

/* The exported scale modes, e.g. for an options menu. */
#define SCENE_SCALE_MODE_COUNT {{mode_count}}
extern const SceneScaleMode scene_scale_modes[SCENE_SCALE_MODE_COUNT];

/* The scale mode chosen in Project Settings, as a suggested default. */
#define SCENE_DEFAULT_SCALE_MODE {{mode}}

/* Camera showing the scene on the current window: scene origin at the window centre,
   zoomed so the design area fits according to `mode`. Also converts between window
   pixels and scene coordinates, e.g. GetScreenToWorld2D(GetMousePosition(), camera). */
Camera2D scene_camera(SceneScaleMode mode);

/* Wrap scene drawing in these (between BeginDrawing and EndDrawing). Everything drawn in
   between, the scene's sprites and your own drawing, is in scene coordinates:
       scene_begin(mode);  scene_level_draw(&level);  DrawCircleV(level.coin.pose.position, ...);  scene_end(); */
void scene_begin(SceneScaleMode mode);
void scene_end(void);

/* The Canvas the UI is laid out on, in window pixels: the window, or with SCENE_SCALE_FIT
   the design area inside the bars. */
Rectangle scene_ui_canvas(SceneScaleMode mode);

/* Window pixels per canvas unit on `canvas`: the design area just fits inside it. */
float scene_ui_scale(Rectangle canvas);

/* ---------------------------------------------------------------------------------------
   Entities. Each exported scene is a struct (see its header) with a SceneEntity field per
   entity and a SceneUIElement field per UI element, named after them in the editor.
   --------------------------------------------------------------------------------------- */

/* Position, rotation (degrees) and scale relative to the entity's parent. Positions are
   in scene coordinates: (0, 0) is the scene origin, y points down. */
typedef struct SceneTransform {
    Vector2 position;
    float rotation;
    Vector2 scale;
    bool inherit_rotation; /* false: ignore the parents' rotation (e.g. a health bar) */
    bool inherit_scale;    /* false: ignore the parents' scale */
} SceneTransform;

/* Final placement in the scene, after the parents and the World transform. */
typedef struct ScenePose {
    Vector2 position;
    float rotation;
    Vector2 scale;
} ScenePose;

/* What a sprite draws, filling its size, in the shape chosen in the editor. */
typedef struct SceneSprite {
    Vector2 size; /* before scaling */
    Color tint;
} SceneSprite;

/* A box collider: the box at `offset` from the entity's position, before scaling. With
   physics it's the shape of the entity's Box2D body; either way scene_box() places it in
   the scene, e.g. for level geometry: scene_box(e.pose, e.collider.offset, e.collider.size). */
typedef struct SceneBoxCollider {
    Vector2 offset;
    Vector2 size;
    bool is_trigger;   /* with physics: a sensor that reports overlaps but doesn't collide */
    float density;     /* physics material: kg per square metre */
    float friction;
    float restitution; /* bounciness 0..1 */
} SceneBoxCollider;

typedef struct SceneEntity {
    SceneTransform transform;  /* relative to its parent */
    ScenePose pose;            /* in the scene: from the last scene's _update_poses / _draw */
    SceneSprite sprite;        /* unused by entities without a sprite */
    SceneBoxCollider collider; /* unused by entities without a box collider */
    bool visible;              /* false: its sprite isn't drawn (its children's still are) */
#if SCENE_PHYSICS
    b2BodyId body;             /* its Box2D body (rigidbody or collider), else b2_nullBodyId */
#endif
} SceneEntity;

/* A rotated box in the scene: centre, size (scaled) and rotation in degrees. */
typedef struct SceneBox {
    Vector2 center;
    Vector2 size;
    float rotation;
} SceneBox;

/* UI elements are laid out on the Canvas (the window) instead of placed in the scene, so the
   camera and the World transform never move them. Measured in canvas units: the design
   resolution's units, with the window's extra length on one axis as extra canvas.

   A UI element's rectangle, relative to its parent's (the Canvas's or another element's):
     anchor_min / anchor_max  points on the parent rectangle, as fractions (0..1). Equal on
                              an axis: pinned to that point. Apart: stretched with the parent.
     pivot                    the point of the element (fractions of its size) that is placed
     position                 where the pivot goes, in canvas units, from the anchor point
     size                     size in canvas units; on a stretched axis it is added to the
                              distance between the anchors (0 = fill, -20 = 10 in per side) */
typedef struct SceneUIRectTransform {
    Vector2 anchor_min;
    Vector2 anchor_max;
    Vector2 pivot;
    Vector2 position;
    Vector2 size;
} SceneUIRectTransform;

/* Text alignment inside a rectangle, per axis: START = left / top, END = right / bottom. */
typedef enum SceneTextAlign {
    SCENE_ALIGN_START,
    SCENE_ALIGN_CENTER,
    SCENE_ALIGN_END
} SceneTextAlign;

#define SCENE_TEXT_MAX 128 /* label text is truncated to SCENE_TEXT_MAX - 1 bytes */

/* Text drawn inside a UI element's rectangle. font_size and spacing are in canvas units, so
   text scales with the rest of the UI. */
typedef struct SceneUILabel {
    char text[SCENE_TEXT_MAX];
    float font_size;
    float spacing; /* between characters */
    SceneTextAlign align_h;
    SceneTextAlign align_v;
    Color color;
    const Font *font; /* NULL: the scene's ui_font, or raylib's default font */
} SceneUILabel;

/* A UI element. Draw your own widgets into `rect` after the scene's _draw_ui (or
   _update_ui); multiply sizes in canvas units by the scene's ui_scale. */
typedef struct SceneUIElement {
    SceneUIRectTransform layout; /* relative to its parent element or the Canvas */
    Rectangle rect;              /* window pixels, from the last scene's _update_ui / _draw_ui */
    bool visible;                /* false: neither its rectangle nor its label is drawn */
    Color color;                 /* fill of its UI rectangle, if it has one */
    SceneUILabel label;          /* its text, if it has a label */
} SceneUIElement;

/* ---------------------------------------------------------------------------------------
   Building blocks of the generated scenes, usable in your own code too
   --------------------------------------------------------------------------------------- */

/* A transform with the given values that inherits rotation and scale. */
static inline SceneTransform scene_transform(float x, float y, float rotation, float scale_x, float scale_y) {
    SceneTransform t;
    t.position.x = x;
    t.position.y = y;
    t.rotation = rotation;
    t.scale.x = scale_x;
    t.scale.y = scale_y;
    t.inherit_rotation = true;
    t.inherit_scale = true;
    return t;
}

static inline SceneBoxCollider scene_box_collider(Vector2 offset, Vector2 size, bool is_trigger, float density,
                                                  float friction, float restitution) {
    SceneBoxCollider c;
    c.offset = offset;
    c.size = size;
    c.is_trigger = is_trigger;
    c.density = density;
    c.friction = friction;
    c.restitution = restitution;
    return c;
}

static inline SceneUIRectTransform scene_ui_rect_transform(Vector2 anchor_min, Vector2 anchor_max, Vector2 pivot,
                                                           Vector2 position, Vector2 size) {
    SceneUIRectTransform t;
    t.anchor_min = anchor_min;
    t.anchor_max = anchor_max;
    t.pivot = pivot;
    t.position = position;
    t.size = size;
    return t;
}

/* Cosine and sine of a rotation in degrees: worked out once for a parent, used by all of its
   children. */
typedef struct SceneRot {
    float c;
    float s;
} SceneRot;

static inline SceneRot scene_rot(float degrees) {
    SceneRot r;
    r.c = cosf(degrees * (PI / 180.0f));
    r.s = sinf(degrees * (PI / 180.0f));
    return r;
}

static inline ScenePose scene_pose_of(SceneTransform t) {
    ScenePose p;
    p.position = t.position;
    p.rotation = t.rotation;
    p.scale = t.scale;
    return p;
}

static inline ScenePose scene_pose_identity(void) {
    return scene_pose_of(scene_transform(0.0f, 0.0f, 0.0f, 1.0f, 1.0f));
}

/* Child pose = parent pose * local transform (translate, rotate, scale), honouring the
   inherit flags. Same rule as the editor. `rot` is scene_rot(parent.rotation). */
static inline ScenePose scene_compose(ScenePose parent, SceneRot rot, SceneTransform t) {
    const float rotation = t.inherit_rotation ? parent.rotation : 0.0f;
    const float c = t.inherit_rotation ? rot.c : 1.0f;
    const float s = t.inherit_rotation ? rot.s : 0.0f;
    const float scale_x = t.inherit_scale ? parent.scale.x : 1.0f;
    const float scale_y = t.inherit_scale ? parent.scale.y : 1.0f;
    const float ox = t.position.x * scale_x;
    const float oy = t.position.y * scale_y;
    ScenePose p;
    p.position.x = parent.position.x + ox * c - oy * s;
    p.position.y = parent.position.y + ox * s + oy * c;
    p.rotation = rotation + t.rotation;
    p.scale.x = scale_x * t.scale.x;
    p.scale.y = scale_y * t.scale.y;
    return p;
}

/* scene_compose, working out the parent's rotation itself. */
static inline ScenePose scene_child_pose(ScenePose parent, SceneTransform t) {
    return scene_compose(parent, scene_rot(parent.rotation), t);
}

/* Child rectangle = the anchors on the parent, plus size, with the pivot at position.
   `scale` converts canvas units to the parent's units (window pixels: the UI scale). */
static inline Rectangle scene_ui_layout(Rectangle parent, const SceneUIRectTransform *t, float scale) {
    const float min_x = parent.x + t->anchor_min.x * parent.width;
    const float min_y = parent.y + t->anchor_min.y * parent.height;
    const float span_x = (t->anchor_max.x - t->anchor_min.x) * parent.width;
    const float span_y = (t->anchor_max.y - t->anchor_min.y) * parent.height;
    Rectangle r;
    r.width = span_x + t->size.x * scale;
    r.height = span_y + t->size.y * scale;
    r.x = min_x + span_x * t->pivot.x + t->position.x * scale - t->pivot.x * r.width;
    r.y = min_y + span_y * t->pivot.y + t->position.y * scale - t->pivot.y * r.height;
    return r;
}

/* A box of `size` at `offset` from an entity (both before scaling), placed in the scene at
   `pose`: scene_box(e.pose, e.collider.offset, e.collider.size) is its collider,
   scene_box(e.pose, (Vector2){0, 0}, e.sprite.size) its sprite's area. */
SceneBox scene_box(ScenePose pose, Vector2 offset, Vector2 size);

/* The box's corners in the scene (top-left, top-right, bottom-right, bottom-left before
   rotating). */
void scene_box_corners(SceneBox box, Vector2 corners[4]);

/* Outline of a box (between scene_begin and scene_end), `thick` in scene units. */
void scene_draw_box_lines(SceneBox box, float thick, Color color);

/* Sprites at a pose (between scene_begin and scene_end): a rectangle, a circle (an ellipse
   when the size isn't square) or a triangle pointing up (down when flipped). */
void scene_draw_rectangle(const SceneSprite *sprite, ScenePose pose);
void scene_draw_circle(const SceneSprite *sprite, ScenePose pose);
void scene_draw_triangle(const SceneSprite *sprite, ScenePose pose);

/* Sets every field of a label (the text is copied, truncated to SCENE_TEXT_MAX - 1 bytes). */
void scene_set_ui_label(SceneUILabel *label, const char *text, float font_size, float spacing, SceneTextAlign align_h,
                        SceneTextAlign align_v, Color color);
/* Replaces a label's text, e.g. scene_set_ui_label_text(&hud.score.label, TextFormat("%d", score)). */
void scene_set_ui_label_text(SceneUILabel *label, const char *text);

/* Draws a label in `rect` (window pixels), its sizes times `ui_scale`, with its own font or
   else `font`. */
void scene_draw_ui_label(const SceneUILabel *label, Rectangle rect, float ui_scale, Font font);

/* Draws `text` (lines split at '\n') inside `rect` with any font, aligned per axis. Sizes are
   in pixels, as DrawTextEx takes them: for text that scales with the UI, multiply sizes in
   canvas units by the scene's ui_scale. Text isn't clipped or wrapped. */
void scene_draw_ui_text(Font font, const char *text, Rectangle rect, float font_size, float spacing,
                        SceneTextAlign align_h, SceneTextAlign align_v, Color color);

#if SCENE_PHYSICS
/* ----- Physics (Box2D) -------------------------------------------------------------------
   Box2D works in metres: SCENE_PIXELS_PER_METER scene units are one metre. Physics happens
   in the World's own space, so moving the World transform doesn't move bodies. For anything
   else, use the Box2D API on entity.body and the scene's physics_world. */

b2Vec2 scene_to_meters(Vector2 v);
Vector2 scene_to_scene_units(b2Vec2 m);

/* A Box2D world with the project's gravity. */
b2WorldId scene_physics_create_world(void);

/* A body definition at `pose` (in the World's own space), with `entity` as user data. */
b2BodyDef scene_body_def(ScenePose pose, b2BodyType type, SceneEntity *entity);

/* Adds `collider` to `body` as a box shape at the entity's `scale`, with `entity` as user
   data. A trigger is a sensor. */
void scene_add_box_collider(b2BodyId body, Vector2 scale, const SceneBoxCollider *collider, SceneEntity *entity);

/* Steps `world` in fixed steps for `dt` more seconds (at most a quarter second's worth);
   `time` keeps what's left over. */
void scene_physics_advance(b2WorldId world, float *time, float dt);

/* Moves the entity's transform to where its body is; `base` is its parent's placement in the
   World's own space. Its scale is kept. */
void scene_body_to_entity(SceneEntity *entity, ScenePose base);

/* Velocity in scene units per second; impulses in kg * scene units per second; forces in
   kg * scene units per second squared. They do nothing for entities without a body. */
Vector2 scene_get_velocity(const SceneEntity *entity);
void scene_set_velocity(SceneEntity *entity, Vector2 velocity);
void scene_apply_impulse(SceneEntity *entity, Vector2 impulse);
void scene_apply_force(SceneEntity *entity, Vector2 force);
#endif /* SCENE_PHYSICS */

#ifdef __cplusplus
}
#endif

#endif /* SCENE_COMMON_H */
)GEN",
                               values);

    const std::string c = fill(R"GEN({{marker}}: code shared by every exported scene. Do not edit.
#include "scene_common.h"

#include <string.h>

/* ----- Screen scaling ----- */

const SceneScaleMode scene_scale_modes[SCENE_SCALE_MODE_COUNT] = {{{mode_list}}};

{{scaling_source}}
Rectangle scene_ui_canvas(SceneScaleMode mode) {
    struct {
        Rectangle ui_canvas;
    } canvas, *scene = &canvas;
{{ui_canvas}}    return scene->ui_canvas;
}

float scene_ui_scale(Rectangle canvas) {
    const float scale_x = canvas.width / SCENE_DESIGN_WIDTH;
    const float scale_y = canvas.height / SCENE_DESIGN_HEIGHT;
    return scale_x < scale_y ? scale_x : scale_y;
}

/* ----- Boxes and sprites ----- */

/* A point in entity units (before scaling, relative to its position) placed in the scene. */
static Vector2 to_scene(ScenePose pose, SceneRot rot, float x, float y) {
    const float px = x * pose.scale.x;
    const float py = y * pose.scale.y;
    Vector2 p;
    p.x = pose.position.x + px * rot.c - py * rot.s;
    p.y = pose.position.y + px * rot.s + py * rot.c;
    return p;
}

SceneBox scene_box(ScenePose pose, Vector2 offset, Vector2 size) {
    SceneBox box;
    box.center = to_scene(pose, scene_rot(pose.rotation), offset.x, offset.y);
    box.size.x = size.x * fabsf(pose.scale.x);
    box.size.y = size.y * fabsf(pose.scale.y);
    box.rotation = pose.rotation;
    return box;
}

void scene_box_corners(SceneBox box, Vector2 corners[4]) {
    const SceneRot rot = scene_rot(box.rotation);
    const float hw = box.size.x / 2.0f;
    const float hh = box.size.y / 2.0f;
    const float xs[4] = {-hw, hw, hw, -hw};
    const float ys[4] = {-hh, -hh, hh, hh};
    int i;
    for (i = 0; i < 4; ++i) {
        corners[i].x = box.center.x + xs[i] * rot.c - ys[i] * rot.s;
        corners[i].y = box.center.y + xs[i] * rot.s + ys[i] * rot.c;
    }
}

void scene_draw_box_lines(SceneBox box, float thick, Color color) {
    Vector2 corners[4];
    int i;
    scene_box_corners(box, corners);
    for (i = 0; i < 4; ++i) {
        DrawLineEx(corners[i], corners[(i + 1) % 4], thick, color);
    }
}

#define SCENE_CIRCLE_SEGMENTS 36

/* raylib only draws triangles whose corners run counter-clockwise on screen. */
static void draw_triangle_any_order(Vector2 a, Vector2 b, Vector2 c, Color color) {
    if ((b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x) > 0.0f) {
        const Vector2 t = b;
        b = c;
        c = t;
    }
    DrawTriangle(a, b, c, color);
}

void scene_draw_rectangle(const SceneSprite *sprite, ScenePose pose) {
    const float w = sprite->size.x * fabsf(pose.scale.x);
    const float h = sprite->size.y * fabsf(pose.scale.y);
    DrawRectanglePro((Rectangle){pose.position.x, pose.position.y, w, h}, (Vector2){w / 2.0f, h / 2.0f}, pose.rotation,
                     sprite->tint);
}

void scene_draw_circle(const SceneSprite *sprite, ScenePose pose) {
    const float hw = sprite->size.x / 2.0f;
    const float hh = sprite->size.y / 2.0f;
    const SceneRot rot = scene_rot(pose.rotation);
    Vector2 previous = to_scene(pose, rot, hw, 0.0f);
    int i;
    for (i = 1; i <= SCENE_CIRCLE_SEGMENTS; ++i) {
        const float angle = 2.0f * PI * (float)i / SCENE_CIRCLE_SEGMENTS;
        const Vector2 next = to_scene(pose, rot, hw * cosf(angle), hh * sinf(angle));
        draw_triangle_any_order(pose.position, previous, next, sprite->tint);
        previous = next;
    }
}

void scene_draw_triangle(const SceneSprite *sprite, ScenePose pose) {
    const float hw = sprite->size.x / 2.0f;
    const float hh = sprite->size.y / 2.0f;
    const SceneRot rot = scene_rot(pose.rotation);
    draw_triangle_any_order(to_scene(pose, rot, 0.0f, -hh), to_scene(pose, rot, -hw, hh), to_scene(pose, rot, hw, hh),
                            sprite->tint);
}

/* ----- UI ----- */

void scene_set_ui_label_text(SceneUILabel *label, const char *text) {
    /* Copy and truncate by hand (strncpy is flagged as deprecated by some C runtimes). */
    size_t n = 0;
    while (text != NULL && n < SCENE_TEXT_MAX - 1 && text[n] != '\0') {
        label->text[n] = text[n];
        ++n;
    }
    label->text[n] = '\0';
}

void scene_set_ui_label(SceneUILabel *label, const char *text, float font_size, float spacing, SceneTextAlign align_h,
                        SceneTextAlign align_v, Color color) {
    scene_set_ui_label_text(label, text);
    label->font_size = font_size;
    label->spacing = spacing;
    label->align_h = align_h;
    label->align_v = align_v;
    label->color = color;
    label->font = NULL;
}

static float align_fraction(SceneTextAlign align) {
    return align == SCENE_ALIGN_START ? 0.0f : align == SCENE_ALIGN_END ? 1.0f : 0.5f;
}

void scene_draw_ui_text(Font font, const char *text, Rectangle rect, float font_size, float spacing,
                        SceneTextAlign align_h, SceneTextAlign align_v, Color color) {
    char line[1024]; /* longer lines are cut */
    const float line_height = font_size * 1.2f;
    int lines = 1;
    const char *c;
    float y;
    if (text == NULL) {
        return;
    }
    for (c = text; *c != '\0'; ++c) {
        lines += *c == '\n';
    }
    y = rect.y + (rect.height - (line_height * (float)(lines - 1) + font_size)) * align_fraction(align_v);
    for (;;) {
        size_t n = 0;
        size_t kept;
        Vector2 position;
        while (text[n] != '\0' && text[n] != '\n') {
            ++n;
        }
        kept = n < sizeof(line) - 1 ? n : sizeof(line) - 1;
        memcpy(line, text, kept);
        line[kept] = '\0';
        position.x = rect.x + (rect.width - MeasureTextEx(font, line, font_size, spacing).x) * align_fraction(align_h);
        position.y = y;
        DrawTextEx(font, line, position, font_size, spacing, color);
        if (text[n] == '\0') {
            break;
        }
        text += n + 1;
        y += line_height;
    }
}

void scene_draw_ui_label(const SceneUILabel *label, Rectangle rect, float ui_scale, Font font) {
    scene_draw_ui_text(label->font != NULL ? *label->font : font, label->text, rect, label->font_size * ui_scale,
                       label->spacing * ui_scale, label->align_h, label->align_v, label->color);
}

#if SCENE_PHYSICS
/* ----- Physics (Box2D) ----- */

b2Vec2 scene_to_meters(Vector2 v) {
    b2Vec2 m;
    m.x = v.x / SCENE_PIXELS_PER_METER;
    m.y = v.y / SCENE_PIXELS_PER_METER;
    return m;
}

Vector2 scene_to_scene_units(b2Vec2 m) {
    Vector2 v;
    v.x = m.x * SCENE_PIXELS_PER_METER;
    v.y = m.y * SCENE_PIXELS_PER_METER;
    return v;
}

b2WorldId scene_physics_create_world(void) {
    b2WorldDef def = b2DefaultWorldDef();
    def.gravity.x = SCENE_GRAVITY_X;
    def.gravity.y = SCENE_GRAVITY_Y;
    return b2CreateWorld(&def);
}

b2BodyDef scene_body_def(ScenePose pose, b2BodyType type, SceneEntity *entity) {
    b2BodyDef def = b2DefaultBodyDef();
    def.type = type;
    def.position = scene_to_meters(pose.position);
    def.rotation = b2MakeRot(pose.rotation * DEG2RAD);
    def.userData = entity;
    return def;
}

void scene_add_box_collider(b2BodyId body, Vector2 scale, const SceneBoxCollider *collider, SceneEntity *entity) {
    b2ShapeDef shape = b2DefaultShapeDef();
    Vector2 centre;
    b2Polygon box;
    centre.x = collider->offset.x * scale.x;
    centre.y = collider->offset.y * scale.y;
    box = b2MakeOffsetBox(collider->size.x * fabsf(scale.x) * 0.5f / SCENE_PIXELS_PER_METER,
                          collider->size.y * fabsf(scale.y) * 0.5f / SCENE_PIXELS_PER_METER, scene_to_meters(centre),
                          b2Rot_identity);
    shape.density = collider->density;
    shape.material.friction = collider->friction;
    shape.material.restitution = collider->restitution;
    shape.isSensor = collider->is_trigger;
    shape.enableSensorEvents = true;
    shape.enableContactEvents = true;
    shape.userData = entity;
    b2CreatePolygonShape(body, &shape, &box);
}

void scene_physics_advance(b2WorldId world, float *time, float dt) {
    *time += dt < 0.25f ? dt : 0.25f;
    while (*time >= SCENE_PHYSICS_TIME_STEP) {
        b2World_Step(world, SCENE_PHYSICS_TIME_STEP, SCENE_PHYSICS_SUB_STEPS);
        *time -= SCENE_PHYSICS_TIME_STEP;
    }
}

static float divide_or(float value, float divisor, float fallback) {
    return divisor != 0.0f ? value / divisor : fallback;
}

void scene_body_to_entity(SceneEntity *entity, ScenePose base) {
    SceneTransform *t = &entity->transform;
    const Vector2 position = scene_to_scene_units(b2Body_GetPosition(entity->body));
    const float rotation = b2Rot_GetAngle(b2Body_GetRotation(entity->body)) * RAD2DEG;
    const float parent_rotation = t->inherit_rotation ? base.rotation : 0.0f;
    const SceneRot back = scene_rot(-parent_rotation);
    const float dx = position.x - base.position.x;
    const float dy = position.y - base.position.y;
    t->position.x = divide_or(dx * back.c - dy * back.s, t->inherit_scale ? base.scale.x : 1.0f, t->position.x);
    t->position.y = divide_or(dx * back.s + dy * back.c, t->inherit_scale ? base.scale.y : 1.0f, t->position.y);
    t->rotation = rotation - parent_rotation;
}

Vector2 scene_get_velocity(const SceneEntity *entity) {
    const Vector2 none = {0.0f, 0.0f};
    return b2Body_IsValid(entity->body) ? scene_to_scene_units(b2Body_GetLinearVelocity(entity->body)) : none;
}

void scene_set_velocity(SceneEntity *entity, Vector2 velocity) {
    if (b2Body_IsValid(entity->body)) {
        b2Body_SetLinearVelocity(entity->body, scene_to_meters(velocity));
    }
}

void scene_apply_impulse(SceneEntity *entity, Vector2 impulse) {
    if (b2Body_IsValid(entity->body)) {
        b2Body_ApplyLinearImpulseToCenter(entity->body, scene_to_meters(impulse), true);
    }
}

void scene_apply_force(SceneEntity *entity, Vector2 force) {
    if (b2Body_IsValid(entity->body)) {
        b2Body_ApplyForceToCenter(entity->body, scene_to_meters(force), true);
    }
}
#endif /* SCENE_PHYSICS */
)GEN",
                               values);

    const std::filesystem::path dir = outDir / kUtilsDir;
    return writeFile(dir / "scene_common.h", h, error) && writeFile(dir / "scene_common.c", c, error);
}

bool writeProjectMain(const std::string& projectName, const std::vector<ExportedScene>& scenes,
                      const std::filesystem::path& outDir, std::string& error) {
    // Examples tied to the designed scenes, when the project has them: a "Score" label showing
    // the time, text of your own in a "Minimap" element, and a dynamic "Player" body that jumps.
    std::string score;
    std::string widget;
    std::string jump;
    for (const ExportedScene& s : scenes) {
        const std::string member = "manager." + managerField(s.id);
        const std::string constant = sceneIdConstant(s.name);
        if (score.empty() && !s.scoreLabel.empty()) {
            score = "        /* A label's text can change: the \"Score\" of scene \"" + commentText(s.name) +
                    "\" shows the time. */\n"
                    "        scene_set_ui_label_text(&" + member + "." + s.scoreLabel +
                    ".label, TextFormat(\"Time %.1f\", GetTime()));\n";
        }
        if (widget.empty() && !s.minimapElement.empty()) {
            widget = "        /* Your own UI in a UI element's rect: text at the bottom of scene \"" + commentText(s.name) +
                     "\"'s \"Minimap\",\n"
                     "           sized in canvas units times ui_scale, so it scales with the UI. */\n"
                     "        if (manager.current == " + constant + ") {\n"
                     "            const float scale = " + member + ".ui_scale;\n"
                     "            scene_draw_ui_text(GetFontDefault(), TextFormat(\"%d FPS\", GetFPS()), " + member + "." +
                     s.minimapElement + ".rect,\n"
                     "                               24.0f * scale, 2.0f * scale, SCENE_ALIGN_CENTER, SCENE_ALIGN_END, RAYWHITE);\n"
                     "        }\n";
        }
        if (jump.empty() && !s.playerBody.empty()) {
            jump = "            /* Physics: Space makes the \"Player\" of scene \"" + commentText(s.name) + "\" jump. */\n"
                   "            if (IsKeyPressed(KEY_SPACE) && manager.current == " + constant + ") {\n"
                   "                SceneEntity *player = &" + member + "." + s.playerBody + ";\n"
                   "                const Vector2 velocity = {scene_get_velocity(player).x, -6.0f * SCENE_PIXELS_PER_METER};\n"
                   "                scene_set_velocity(player, velocity);\n"
                   "            }\n";
        }
    }

    const std::string c = fill(R"CODE({{marker}}: example main() for project "{{project}}". Do not edit:
// re-export from the editor instead (copy it to your own file to change it).
//
// Shows how the exported scenes fit together in a raylib program: a SceneManager holding
// every scene's struct, switching scenes, and using the placements the editor worked out.
// Build it with every exported .c file and raylib (see the Makefile). Put your own code
// next to it in src/. If your game has its own main(), leave this file out of the build
// (make EXCLUDE=src/project_main.c).
//
// Starts in borderless fullscreen; F11 or Alt+Enter toggles windowed mode.
// Controls: Left / Right    switch scenes
//           M               cycle through the exported scale modes
//           C               show / hide box colliders
//           Space           with physics: make a dynamic body called "Player" jump
//           R               reload the scene from its exported data
//           Esc             quit
// Options:  --windowed      start in a window (design size, fitted to the monitor)
//
// Test builds (compiled with -DSCENE_TESTS, as `make test` does) also accept:
//           --frames N      smoke test: draws every scene in every scale mode in a window,
//                           then exits
#include <string.h>

#ifdef SCENE_TESTS
#include <stdio.h>
#include <stdlib.h>
#endif

#include <raylib.h>

#include "scenes.h"

static const char *const scale_mode_names[] = {"Expand", "Fit", "Fill"}; /* by SceneScaleMode value */

/* The next of the exported scale modes (scene_scale_modes), for the M key. */
static SceneScaleMode next_scale_mode(SceneScaleMode mode) {
    int i;
    for (i = 0; i < SCENE_SCALE_MODE_COUNT; ++i) {
        if (scene_scale_modes[i] == mode) {
            return scene_scale_modes[(i + 1) % SCENE_SCALE_MODE_COUNT];
        }
    }
    return scene_scale_modes[0];
}

/* Borderless fullscreen: an undecorated window covering the current monitor. (raylib's
   ToggleFullscreen / ToggleBorderlessWindowed use exclusive fullscreen, which on Windows
   stays on top after Alt+Tab.) Windowed: the design size, shrunk to fit the monitor. */
static void set_fullscreen(int fullscreen) {
    const int monitor = GetCurrentMonitor();
    const Vector2 origin = GetMonitorPosition(monitor);
    const int monitor_w = GetMonitorWidth(monitor);
    const int monitor_h = GetMonitorHeight(monitor);
    if (fullscreen) {
        SetWindowState(FLAG_WINDOW_UNDECORATED);
        SetWindowPosition((int)origin.x, (int)origin.y);
        SetWindowSize(monitor_w, monitor_h);
    } else {
        float scale = 1.0f;
        int w, h;
        if (SCENE_DESIGN_WIDTH * scale > monitor_w * 0.85f) scale = monitor_w * 0.85f / SCENE_DESIGN_WIDTH;
        if (SCENE_DESIGN_HEIGHT * scale > monitor_h * 0.85f) scale = monitor_h * 0.85f / SCENE_DESIGN_HEIGHT;
        w = (int)(SCENE_DESIGN_WIDTH * scale);
        h = (int)(SCENE_DESIGN_HEIGHT * scale);
        ClearWindowState(FLAG_WINDOW_UNDECORATED);
        SetWindowSize(w, h);
        SetWindowPosition((int)origin.x + (monitor_w - w) / 2, (int)origin.y + (monitor_h - h) / 2);
    }
}

int main(int argc, char **argv) {
    int i;
    int fullscreen = 1;
    int show_colliders = 0;
    static SceneManager manager; /* every scene's struct */
    SceneScaleMode mode = SCENE_DEFAULT_SCALE_MODE;
#ifdef SCENE_TESTS
    int frame = 0;
    int frames_left = -1; /* >= 0 in smoke-test mode */
#endif

    for (i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--windowed") == 0) {
            fullscreen = 0;
        }
#ifdef SCENE_TESTS
        else if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
            frames_left = atoi(argv[++i]);
            fullscreen = 0; /* don't take over the screen while testing */
        }
#endif
    }

    /* scene_begin() scales the scene to whatever size the window ends up. */
    SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_VSYNC_HINT | FLAG_MSAA_4X_HINT);
    InitWindow((int)SCENE_DESIGN_WIDTH, (int)SCENE_DESIGN_HEIGHT, "{{project}}");
    set_fullscreen(fullscreen);
    SetTargetFPS(60);

    scene_manager_init(&manager, 0);

    while (!WindowShouldClose()) {
        int shown;
#ifdef SCENE_TESTS
        if (frames_left == 0) {
            break;
        }
        if (frames_left > 0) {
            /* Smoke test: 10 frames per scene, then the next scale mode. */
            manager.current = (frame / 10) % SCENE_ID_COUNT;
            mode = scene_scale_modes[(frame / (10 * SCENE_ID_COUNT)) % SCENE_SCALE_MODE_COUNT];
            show_colliders = frame % 2;
            --frames_left;
            ++frame;
        } else
#endif
        {
            const int alt = IsKeyDown(KEY_LEFT_ALT) || IsKeyDown(KEY_RIGHT_ALT);
            if (IsKeyPressed(KEY_F11) || (alt && IsKeyPressed(KEY_ENTER))) {
                fullscreen = !fullscreen;
                set_fullscreen(fullscreen);
            }
            if (IsKeyPressed(KEY_M)) mode = next_scale_mode(mode);
            if (IsKeyPressed(KEY_C)) show_colliders = !show_colliders;
            if (IsKeyPressed(KEY_RIGHT)) manager.current = (manager.current + 1) % SCENE_ID_COUNT;
            if (IsKeyPressed(KEY_LEFT)) manager.current = (manager.current + SCENE_ID_COUNT - 1) % SCENE_ID_COUNT;
            if (IsKeyPressed(KEY_R)) scene_manager_reload(&manager, manager.current);
{{jump}}        }

        /* Physics of the current scene (scenes with bodies). */
        scene_manager_update(&manager, GetFrameTime());
{{score}}
        BeginDrawing();
        ClearBackground((Color){20, 20, 23, 255});
        shown = manager.current;

        /* The scene, in scene coordinates: draw your own things here too, at the entities'
           poses (e.g. manager.<scene>.<entity>.pose.position). */
        scene_begin(mode);
        scene_manager_draw(&manager);
        if (show_colliders) {
            scene_manager_draw_colliders(&manager, GREEN);
        }
        scene_end();

        /* The UI on top, in window pixels: draw your own widgets into the elements' rects. */
        scene_manager_draw_ui(&manager, mode);
{{widget}}
        DrawText(TextFormat("%s  (%d/%d)   scale: %s", scene_names[shown], shown + 1, SCENE_ID_COUNT,
                            scale_mode_names[mode]),
                 10, 10, 20, RAYWHITE);
        DrawText(TextFormat("Left/Right scene   M scale   C colliders   R reload%s   F11 %s", {{jump_help}},
                            fullscreen ? "windowed" : "fullscreen"),
                 10, 34, 20, GRAY);
        EndDrawing();
    }

    scene_manager_deinit(&manager);
    CloseWindow();
#ifdef SCENE_TESTS
    if (frames_left == 0) {
        printf("smoke test passed: %d scene(s), %d frames in %d scale mode(s)\n", SCENE_ID_COUNT, frame,
               SCENE_SCALE_MODE_COUNT);
    }
#endif
    return 0;
}
)CODE",
                               {{"marker", kGeneratedMarker},
                                {"project", cString(projectName)},
                                {"jump", jump},
                                {"jump_help", jump.empty() ? "\"\"" : "\"   Space jump\""},
                                {"score", score},
                                {"widget", widget}});
    return writeFile(outDir / kSrcDir / "project_main.c", c, error);
}

namespace {
// Some editors save UTF-8 files with a byte-order mark; the marker check looks past it.
void skipByteOrderMark(std::string& line) {
    if (line.rfind("\xEF\xBB\xBF", 0) == 0) {
        line.erase(0, 3);
    }
}
} // namespace

bool writeGitignore(const std::filesystem::path& outDir, const std::string& projectName, std::string& error) {
    const std::filesystem::path path = outDir / ".gitignore";
    const std::string marker = "# " + std::string(kGeneratedMarker).substr(3); // "# Generated by Scene Editor"
    const std::string text = marker + " for project \"" + projectName +
                             "\". To keep your own changes, set .gitignore\n"
                             "# to \"Only if missing\" in Project Settings > Export.\n"
                             "#\n"
                             "# Everything else here is source: the generated scenes and utils are needed to build\n"
                             "# the game without the editor, so they stay tracked.\n"
                             "\n"
                             "# Build output: executables, object and dependency files (make clean deletes it)\n"
                             "/bin/\n";
    return writeFile(path, text, error);
}

bool writeBox2D(const std::filesystem::path& outDir, const std::filesystem::path& sourceDir, std::string& error) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(sourceDir / "include" / "box2d" / "box2d.h", ec) ||
        !std::filesystem::is_directory(sourceDir / "src", ec)) {
        error = "Box2D (include/box2d/box2d.h and src/) not found in " + sourceDir.string();
        return false;
    }
    const std::filesystem::path dir = outDir / "box2d";
    const auto options = std::filesystem::copy_options::recursive | std::filesystem::copy_options::overwrite_existing;
    std::filesystem::create_directories(dir, ec);
    for (const char* part : {"include", "src"}) {
        std::filesystem::copy(sourceDir / part, dir / part, options, ec);
        if (ec) {
            error = "can't copy Box2D's " + std::string(part) + " from " + sourceDir.string() + ": " + ec.message();
            return false;
        }
    }
    if (std::filesystem::is_regular_file(sourceDir / "LICENSE", ec)) {
        std::filesystem::copy_file(sourceDir / "LICENSE", dir / "LICENSE", std::filesystem::copy_options::overwrite_existing, ec);
    }
    return true;
}

bool writeVSCodeFiles(const std::filesystem::path& outDir, const std::string& projectName, std::string& error) {
    using json = nlohmann::ordered_json;
    const std::filesystem::path path = outDir / ".vscode" / "c_cpp_properties.json";

    // Only paths inside the project, so the file works on any machine. raylib.h is found
    // through the same environment variables the Makefile reads (set either one); the C/C++
    // extension finds a compiler by itself. "Test" defines SCENE_TESTS so the test code in
    // project_main.c isn't greyed out.
    const auto configuration = [](const char* name, json defines) {
        return json{{"name", name},
                    {"includePath",
                     {"${workspaceFolder}/src", "${workspaceFolder}/generated", "${workspaceFolder}/utils",
                      "${workspaceFolder}/box2d/include", "${env:RAYLIB_INCLUDE}", "${env:RAYLIB_SRC}"}},
                    {"defines", std::move(defines)},
                    {"cStandard", "c11"},
                    {"cppStandard", "c++17"}};
    };
    const json properties{
        {"configurations", {configuration("Release", {"NDEBUG"}), configuration("Test", {"SCENE_TESTS"})}},
        {"version", 4}};

    const std::string text = std::string(kGeneratedMarker) + ": VS Code IntelliSense settings for project \"" +
                             projectName +
                             "\".\n"
                             "// raylib.h comes from the RAYLIB_INCLUDE or RAYLIB_SRC environment variable (the folder\n"
                             "// containing raylib.h), the same variables the Makefile uses. To keep your own changes,\n"
                             "// set .vscode settings to \"Only if missing\" in Project Settings > Export.\n" +
                             properties.dump(4) + "\n";
    return writeFile(path, text, error);
}

int removeGeneratedFiles(const std::filesystem::path& outDir, const std::vector<std::filesystem::path>& keep) {
    int removed = 0;
    std::error_code ec;
    std::vector<std::filesystem::path> generated;
    // The top level too: the Makefile, and every file of exports from before the folders.
    for (const std::filesystem::path& dir :
         {outDir, outDir / kSrcDir, outDir / kGeneratedDir, outDir / kUtilsDir, outDir / ".vscode"}) {
        for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
            if (!entry.is_regular_file()) {
                continue;
            }
            std::string firstLine;
            {
                std::ifstream file(entry.path(), std::ios::binary);
                std::getline(file, firstLine);
                skipByteOrderMark(firstLine);
            }
            // "// Generated by Scene Editor..." in C files, "# Generated by Scene Editor..." in the Makefile.
            const std::string text = std::string(kGeneratedMarker).substr(3); // "Generated by Scene Editor"
            const bool kept = std::find(keep.begin(), keep.end(), entry.path()) != keep.end();
            if (!kept && (firstLine.rfind("// " + text, 0) == 0 || firstLine.rfind("# " + text, 0) == 0)) {
                generated.push_back(entry.path());
            }
        }
    }
    for (const auto& path : generated) {
        if (std::filesystem::remove(path, ec)) {
            ++removed;
        }
    }
    return removed;
}

bool writeMakefile(const std::string& projectName, const std::vector<std::string>& sceneNames, int scaleModeCount,
                   const std::filesystem::path& outDir, std::string& error) {
    // Sources are found by the Makefile itself (every .c in src/, generated/ and utils/).
    // Enough smoke-test frames to draw every scene in every scale mode (10 frames each).
    const std::string frames =
        std::to_string(std::max<std::size_t>(10 * sceneNames.size() * static_cast<std::size_t>(scaleModeCount), 60));

    // Recipe lines must start with a tab.
    const std::string makefile = fill(R"MAKE(# Generated by Scene Editor for project "{{project}}". Do not edit:
# re-export from the editor instead.
#
# Builds the exported scenes into a small raylib program.
#
#   src/         project_main.c (an example main) and your own code: every src/*.c is built
#   generated/   the scenes (one struct each), scenes.h and the SceneManager; rewritten by
#                every export
#   box2d/       Box2D (physics), when Project Settings > Physics is on
#   utils/       scene_common: screen scaling, the entity and UI types, and drawing helpers
#   bin/         everything the build makes: bin/{{target}} and bin/{{target}}_test, and
#                their object (.o) and dependency (.d) files in bin/release/ and bin/test/
# All three source folders are include directories, so any header can be included by name.
# Builds are incremental: only sources that changed, or include a header that changed,
# are recompiled. After changing CFLAGS or the compiler, run make clean.
#
#   make release  build bin/{{target}} without the test code (also plain `make`)
#   make run      build the release and run it fullscreen (F11 / Alt+Enter: windowed;
#                 controls are listed in src/project_main.c); ARGS=--windowed starts windowed
#   make test     strict test build (bin/{{target}}_test, compiled with -DSCENE_TESTS,
#                 warnings are errors), then a smoke run that draws every scene in every
#                 exported scale mode
#   make clean    delete bin/
#
# raylib: an installed raylib is used by default (-lraylib). Otherwise, either
#   make RAYLIB_SRC=path/to/raylib/src
#       builds raylib from its sources with the same compiler (once, into bin/raylib/).
#       Always links, whatever the compiler: the simplest choice.
#   make RAYLIB_INCLUDE=path/to/raylib/src RAYLIB_LIB_DIR=path/to/folder/with/libraylib.a
#       links a prebuilt raylib. RAYLIB_LIB_DIR is searched for libraylib.a or raylib.lib
#       (else -L... -lraylib is used, for shared libraries); RAYLIB_LIBS=path/to/libraylib.a
#       gives the exact link input instead.
#       A prebuilt library only links with the compiler family that built it: MinGW gcc
#       can't use one built for MSVC (by MSVC or clang targeting MSVC), and the other way
#       round. Errors like "undefined reference to __chkstk" mean such a mismatch; use
#       RAYLIB_SRC, or the CC (and C runtime flags) the library was built with.
# Your own main(): make EXCLUDE=src/project_main.c
# Compiler: make CC=clang  (needs GNU make; on Windows e.g. mingw32-make).
# Paths: forward slashes, no spaces.

TARGET_NAME := {{target}}
SRC_DIR := src
GENERATED_DIR := generated
UTILS_DIR := utils
BIN_DIR := bin
INCLUDE_DIRS := $(SRC_DIR) $(GENERATED_DIR) $(UTILS_DIR)

EXCLUDE ?=
SOURCES := $(filter-out $(EXCLUDE),$(foreach dir,$(INCLUDE_DIRS),$(wildcard $(dir)/*.c)))

CFLAGS ?= -std=c11 -O2 -Wall -Wextra
RELEASE_CFLAGS := -DNDEBUG
TEST_CFLAGS := -DSCENE_TESTS -Wpedantic -Werror
CPPFLAGS += $(addprefix -I,$(INCLUDE_DIRS))

RAYLIB_INCLUDE ?=
ifneq ($(strip $(RAYLIB_INCLUDE)),)
    CPPFLAGS += -I$(RAYLIB_INCLUDE)
endif
# The raylib library: RAYLIB_LIBS if given, else the one in RAYLIB_LIB_DIR, else -lraylib.
RAYLIB_LIB_DIR ?=
ifeq ($(origin RAYLIB_LIBS),undefined)
    ifneq ($(strip $(RAYLIB_LIB_DIR)),)
        RAYLIB_LIBS := $(or $(firstword $(wildcard $(RAYLIB_LIB_DIR)/libraylib.a $(RAYLIB_LIB_DIR)/raylib.lib)),-L$(RAYLIB_LIB_DIR) -lraylib)
    else
        RAYLIB_LIBS := -lraylib
    endif
endif

# Box2D (physics), when the export includes it (Project Settings > Physics): built from
# box2d/ with its own flags (it needs C17), once, into bin/box2d/.
BOX2D_DIR := box2d
BOX2D_SOURCES := $(wildcard $(BOX2D_DIR)/src/*.c)
ifneq ($(strip $(BOX2D_SOURCES)),)
    BOX2D_OBJECTS := $(BOX2D_SOURCES:$(BOX2D_DIR)/src/%.c=$(BIN_DIR)/box2d/%.o)
    BOX2D_CFLAGS := -std=gnu17 -ffp-contract=off -w -I$(BOX2D_DIR)/include -I$(BOX2D_DIR)/src
    CPPFLAGS += -I$(BOX2D_DIR)/include
endif

# Windows: make runs recipes in sh if it finds one (e.g. Git Bash), otherwise in cmd.
RAYLIB_PLATFORM_CFLAGS :=
MKDIR = mkdir -p $1
RMDIR = rm -rf $1
ifeq ($(OS),Windows_NT)
    EXE := .exe
    PLATFORM_LIBS ?= -lopengl32 -lgdi32 -lwinmm -lshell32 -luser32
    ifeq ($(shell echo %OS%),Windows_NT)
        MKDIR = if not exist $(subst /,\,$1) mkdir $(subst /,\,$1)
        RMDIR = if exist $(subst /,\,$1) rmdir /S /Q $(subst /,\,$1)
    endif
else ifeq ($(shell uname -s),Darwin)
    PLATFORM_LIBS ?= -framework CoreVideo -framework IOKit -framework Cocoa -framework GLUT -framework OpenGL
    RAYLIB_DARWIN := 1
else
    PLATFORM_LIBS ?= -lGL -lm -lpthread -ldl -lrt -lX11
    RAYLIB_PLATFORM_CFLAGS := -D_GLFW_X11
endif

# RAYLIB_SRC: raylib's own sources, compiled with this compiler and CFLAGS (so the C
# runtime matches), but as gnu99 and without warnings.
RAYLIB_SRC ?=
ifneq ($(strip $(RAYLIB_SRC)),)
    RAYLIB_MODULES := rcore rshapes rtextures rtext rmodels raudio rglfw
    RAYLIB_OBJECTS := $(RAYLIB_MODULES:%=$(BIN_DIR)/raylib/%.o)
    RAYLIB_CFLAGS := -std=gnu99 -w -DPLATFORM_DESKTOP -DGRAPHICS_API_OPENGL_33 $(RAYLIB_PLATFORM_CFLAGS)                      -I$(RAYLIB_SRC) -I$(RAYLIB_SRC)/external/glfw/include
    CPPFLAGS += -I$(RAYLIB_SRC)
    RAYLIB_LIBS :=
endif

ARGS ?=

TARGET := $(BIN_DIR)/$(TARGET_NAME)$(EXE)
TEST_TARGET := $(BIN_DIR)/$(TARGET_NAME)_test$(EXE)
# Object files keep the source's folder: src/a.c -> bin/release/src/a.o (and a.d).
RELEASE_OBJECTS := $(SOURCES:%.c=$(BIN_DIR)/release/%.o)
TEST_OBJECTS := $(SOURCES:%.c=$(BIN_DIR)/test/%.o)
# -MMD -MP: also write a .d file listing the headers each source includes.
DEPFLAGS := -MMD -MP
LINK = $(LDFLAGS) $(RAYLIB_OBJECTS) $(BOX2D_OBJECTS) $(RAYLIB_LIBS) $(PLATFORM_LIBS)

.PHONY: all release run test clean

all: release

release: $(TARGET)

$(TARGET): $(RELEASE_OBJECTS) $(RAYLIB_OBJECTS) $(BOX2D_OBJECTS)
	$(CC) $(CFLAGS) $(RELEASE_OBJECTS) -o $@ $(LINK)

$(BIN_DIR)/release/%.o: %.c
	@$(call MKDIR,$(@D))
	$(CC) $(CPPFLAGS) $(CFLAGS) $(RELEASE_CFLAGS) $(DEPFLAGS) -c $< -o $@

run: release
	"$(CURDIR)/$(TARGET)" $(ARGS)

test: $(TEST_TARGET)
	"$(CURDIR)/$(TEST_TARGET)" --frames {{frames}}

$(TEST_TARGET): $(TEST_OBJECTS) $(RAYLIB_OBJECTS) $(BOX2D_OBJECTS)
	$(CC) $(CFLAGS) $(TEST_OBJECTS) -o $@ $(LINK)

$(BIN_DIR)/test/%.o: %.c
	@$(call MKDIR,$(@D))
	$(CC) $(CPPFLAGS) $(CFLAGS) $(TEST_CFLAGS) $(DEPFLAGS) -c $< -o $@

# raylib from source (RAYLIB_SRC), shared by the release and test builds.
$(BIN_DIR)/raylib/%.o: $(RAYLIB_SRC)/%.c
	@$(call MKDIR,$(@D))
	$(CC) $(CFLAGS) $(RAYLIB_CFLAGS) $(DEPFLAGS) -c $(RAYLIB_LANG) $< -o $@

# Box2D from box2d/ (when the export has it).
$(BIN_DIR)/box2d/%.o: $(BOX2D_DIR)/src/%.c
	@$(call MKDIR,$(@D))
	$(CC) $(CFLAGS) $(BOX2D_CFLAGS) $(DEPFLAGS) -c $< -o $@

# macOS: GLFW's Cocoa backend is Objective-C.
ifdef RAYLIB_DARWIN
$(BIN_DIR)/raylib/rglfw.o: RAYLIB_LANG := -x objective-c
endif

clean:
	-$(call RMDIR,$(BIN_DIR))

# Header dependencies from earlier builds (missing on the first build, which is fine).
-include $(RELEASE_OBJECTS:.o=.d) $(TEST_OBJECTS:.o=.d) $(RAYLIB_OBJECTS:.o=.d) $(BOX2D_OBJECTS:.o=.d)
)MAKE",
                                      {{"project", cString(projectName)},
                                       {"target", sceneIdentifier(projectName)},
                                       {"frames", frames}});
    return writeFile(outDir / "Makefile", makefile, error);
}
