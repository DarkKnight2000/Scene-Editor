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

const char* alignConstant(TextAlign align) {
    switch (align) {
    case TextAlign::Start: return "SCENE_ALIGN_START";
    case TextAlign::End: return "SCENE_ALIGN_END";
    case TextAlign::Center: break;
    }
    return "SCENE_ALIGN_CENTER";
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

// "{x, y}" for a Vector2 inside an initializer.
std::string pair(Vector2 v) {
    return "{" + floatLiteral(v.x) + ", " + floatLiteral(v.y) + "}";
}

// "{r, g, b, a}" for a Color inside an initializer.
std::string rgba(Color c) {
    return "{" + std::to_string(c.r) + ", " + std::to_string(c.g) + ", " + std::to_string(c.b) + ", " +
           std::to_string(c.a) + "}";
}

// A SceneTransform initializer, leaving out what is zero.
std::string transformInit(const TransformComponent& t) {
    std::string s = "{";
    if (t.position.x != 0.0f || t.position.y != 0.0f) {
        s += ".position = " + pair(t.position) + ", ";
    }
    if (t.rotation != 0.0f) {
        s += ".rotation = " + floatLiteral(t.rotation) + ", ";
    }
    s += ".scale = " + pair(t.scale);
    if (!t.inheritRotation) {
        s += ", .ignore_parent_rotation = true";
    }
    if (!t.inheritScale) {
        s += ", .ignore_parent_scale = true";
    }
    return s + "}";
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
    bool anyLabel = false;
    for (const entt::entity e : ui) {
        anyLabel = anyLabel || registry.all_of<UILabelComponent>(e);
    }
    // A platformer controller moves a dynamic body; without one there's nothing to move.
    const auto hasPlatformer = [&](entt::entity e) {
        const auto* rb = registry.try_get<RigidbodyComponent>(e);
        return physics && registry.all_of<PlatformerControllerComponent>(e) && rb != nullptr &&
               rb->type == BodyType::Dynamic;
    };

    info = ExportedScene{};
    info.name = sceneName;
    info.id = id;
    info.type = type;
    info.member = fieldIdentifier(id) == "current" ? "current_scene" : fieldIdentifier(id);
    info.hasEntities = !drawn.empty();
    info.hasUI = !ui.empty();
    info.hasBodies = !bodies.empty();
    for (const entt::entity e : drawn) {
        info.entities.push_back(field.at(e));
        if (registry.all_of<BoxColliderComponent>(e)) {
            info.colliders.push_back(field.at(e));
        }
    }
    for (const entt::entity e : ui) {
        info.elements.push_back(field.at(e));
    }

    // --- Header ---
    std::ostringstream fields;
    for (const entt::entity e : drawn) {
        fields << "    SceneEntity " << field.at(e) << "; /* \"" << commentText(nameOf(e)) << "\" */\n";
    }
    for (const entt::entity e : ui) {
        fields << "    SceneUIElement " << field.at(e) << "; /* \"" << commentText(nameOf(e)) << "\" */\n";
    }
    if (info.hasUI) {
        fields << "    Rectangle ui_canvas; /* the Canvas in window pixels, from the last _update_ui */\n"
                  "    float ui_scale;      /* window pixels per canvas unit, from the last _update_ui */\n";
    }
    if (anyLabel) {
        fields << "    const Font *ui_font; /* the labels' font; NULL: raylib's default */\n";
    }
    if (info.hasBodies) {
        fields << "    b2WorldId physics_world;\n"
                  "    float physics_time; /* time not stepped yet */\n";
    }

    std::ostringstream decls;
    decls << "void " << prefix << "_init(" << type << " *s);\n";
    if (info.hasEntities) {
        decls << "void " << prefix << "_update_poses(" << type << " *s);\n"
              << "void " << prefix << "_draw(" << type << " *s);\n";
    }
    if (info.hasUI) {
        decls << "void " << prefix << "_update_ui(" << type << " *s, SceneScaleMode mode);\n"
              << "void " << prefix << "_draw_ui(" << type << " *s, SceneScaleMode mode);\n";
    }
    if (info.hasBodies) {
        decls << "void " << prefix << "_step(" << type << " *s, float dt);\n"
              << "void " << prefix << "_deinit(" << type << " *s);\n";
    }

    const std::string h = fill(R"GEN({{marker}} from scene "{{name}}". Do not edit:
// re-export from the editor instead.
#ifndef {{guard}}
#define {{guard}}

#include "scene_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Scene "{{name}}": one field per entity and UI element, named after it in the editor. */
typedef struct {{type}} {
    SceneTransform world; /* the World root: moves, rotates and zooms the whole scene */
{{fields}}} {{type}};

{{decls}}
#ifdef __cplusplus
}
#endif

#endif /* {{guard}} */
)GEN",
                               {{"marker", kGeneratedMarker},
                                {"name", commentText(sceneName)},
                                {"guard", guard},
                                {"type", type},
                                {"fields", fields.str()},
                                {"decls", decls.str()}});

    // --- Source ---
    std::ostringstream c;
    c << kGeneratedMarker << " from scene \"" << commentText(sceneName) << "\". Do not edit:\n"
      << "// re-export from the editor instead.\n"
      << "#include \"" << id << ".h\"\n";

    // init: every designed value at once.
    c << "\nvoid " << prefix << "_init(" << type << " *s) {\n"
      << "    *s = (" << type << "){\n"
      << "        .world = " << transformInit(registry.get<TransformComponent>(scene.world())) << ",\n";
    for (const entt::entity e : drawn) {
        c << "        ." << field.at(e) << " = {.transform = " << transformInit(registry.get<TransformComponent>(e));
        if (const auto* sprite = registry.try_get<SpriteComponent>(e); sprite != nullptr && sprite->exported) {
            c << ", .sprite = {" << pair(sprite->size) << ", " << rgba(sprite->tint) << "}";
        }
        if (const auto* col = registry.try_get<BoxColliderComponent>(e)) {
            c << ", .collider = {.offset = " << pair(col->offset) << ", .size = " << pair(col->size);
            if (col->isTrigger) {
                c << ", .is_trigger = true";
            }
            c << ", .density = " << floatLiteral(col->density) << ", .friction = " << floatLiteral(col->friction)
              << ", .restitution = " << floatLiteral(col->restitution) << "}";
        }
        if (const auto* pc = registry.try_get<PlatformerControllerComponent>(e); pc != nullptr && hasPlatformer(e)) {
            c << ", .platformer = {.move_speed = " << floatLiteral(pc->moveSpeed)
              << ", .acceleration = " << floatLiteral(pc->acceleration) << ", .air_control = " << floatLiteral(pc->airControl)
              << ", .jump_height = " << floatLiteral(pc->jumpHeight) << ", .coyote_time = " << floatLiteral(pc->coyoteTime)
              << ", .jump_buffer = " << floatLiteral(pc->jumpBuffer) << ", .jump_cut = " << floatLiteral(pc->jumpCut)
              << ", .max_fall_speed = " << floatLiteral(pc->maxFallSpeed) << "}";
        }
        c << ", .visible = true},\n";
    }
    for (const entt::entity e : ui) {
        const auto& u = registry.get<UIRectTransformComponent>(e);
        c << "        ." << field.at(e) << " = {.layout = {" << pair(u.anchorMin) << ", " << pair(u.anchorMax) << ", "
          << pair(u.pivot) << ", " << pair(u.position) << ", " << pair(u.size) << "}";
        if (const auto* rect = registry.try_get<UIRectangleComponent>(e)) {
            c << ", .color = " << rgba(rect->color);
        }
        if (const auto* label = registry.try_get<UILabelComponent>(e)) {
            c << ", .label = {\"" << cString(label->text) << "\", " << floatLiteral(label->fontSize) << ", "
              << floatLiteral(label->spacing) << ", " << alignConstant(label->alignH) << ", "
              << alignConstant(label->alignV) << ", " << rgba(label->color) << "}";
        }
        c << ", .visible = true},\n";
    }
    c << "    };\n";
    if (info.hasBodies) {
        // Bodies are placed in the World's own space: their parent chain without the World
        // transform. Only bodies and their ancestors need that placement.
        std::unordered_set<entt::entity> needed;
        for (const entt::entity e : bodies) {
            for (entt::entity a = e; a != entt::null; a = parentOf(a)) {
                needed.insert(a);
            }
        }
        c << "\n    /* Physics: a body for each entity with a rigidbody or a box collider. */\n"
          << "    s->physics_world = scene_physics_create_world();\n"
          << "    {\n";
        for (const entt::entity e : tree) {
            if (needed.count(e) > 0) {
                const entt::entity p = parentOf(e);
                c << "        const ScenePose p_" << field.at(e) << " = scene_child_pose("
                  << (p == entt::null ? std::string("SCENE_POSE_IDENTITY") : "p_" + field.at(p)) << ", " << ref(e)
                  << ".transform);\n";
            }
        }
        c << "        b2BodyDef def;\n";
        for (const entt::entity e : bodies) {
            const auto* rb = registry.try_get<RigidbodyComponent>(e);
            const std::string pose = "p_" + field.at(e);
            c << "        def = scene_body_def(" << pose << ", "
              << bodyTypeConstant(rb != nullptr ? rb->type : BodyType::Static) << ", &" << ref(e) << ");\n";
            if (rb != nullptr) {
                const RigidbodyComponent defaults;
                if (rb->gravityScale != defaults.gravityScale) {
                    c << "        def.gravityScale = " << floatLiteral(rb->gravityScale) << ";\n";
                }
                if (rb->linearDamping != defaults.linearDamping) {
                    c << "        def.linearDamping = " << floatLiteral(rb->linearDamping) << ";\n";
                }
                if (rb->angularDamping != defaults.angularDamping) {
                    c << "        def.angularDamping = " << floatLiteral(rb->angularDamping) << ";\n";
                }
                if (rb->fixedRotation) {
                    c << "        def.fixedRotation = true;\n";
                }
                if (rb->isBullet) {
                    c << "        def.isBullet = true;\n";
                }
            }
            c << "        " << ref(e) << ".body = b2CreateBody(s->physics_world, &def);\n";
            if (registry.all_of<BoxColliderComponent>(e)) {
                c << "        scene_add_box_collider(" << ref(e) << ".body, " << pose << ".scale, " << ref(e)
                  << ".collider, &" << ref(e) << ");\n";
            }
        }
        c << "    }\n";
    }
    if (info.hasEntities) {
        c << "    " << prefix << "_update_poses(s);\n";
    }
    c << "}\n";

    if (info.hasEntities) {
        // Poses, parents first.
        c << "\nvoid " << prefix << "_update_poses(" << type << " *s) {\n"
          << "    const ScenePose world = scene_child_pose(SCENE_POSE_IDENTITY, s->world);\n";
        for (const entt::entity e : tree) {
            const entt::entity p = parentOf(e);
            c << "    " << ref(e) << ".pose = scene_child_pose("
              << (p == entt::null ? std::string("world") : ref(p) + ".pose") << ", " << ref(e) << ".transform);\n";
        }
        c << "}\n";

        // Sprites, in drawing order.
        c << "\nvoid " << prefix << "_draw(" << type << " *s) {\n"
          << "    " << prefix << "_update_poses(s);\n";
        for (const entt::entity e : drawn) {
            const auto* sprite = registry.try_get<SpriteComponent>(e);
            if (sprite != nullptr && sprite->exported) {
                c << "    if (" << ref(e) << ".visible) scene_draw_" << shapeName(sprite->shape) << "(" << ref(e)
                  << ".sprite, " << ref(e) << ".pose);\n";
            }
        }
        c << "}\n";
    }

    if (info.hasUI) {
        // Layout, parents first; then drawing in the same order.
        c << "\nvoid " << prefix << "_update_ui(" << type << " *s, SceneScaleMode mode) {\n"
          << "    s->ui_canvas = scene_ui_canvas(mode);\n"
          << "    s->ui_scale = scene_ui_scale(s->ui_canvas);\n";
        for (const entt::entity e : ui) {
            const entt::entity p = parentOf(e);
            c << "    " << ref(e) << ".rect = scene_ui_layout("
              << (p == entt::null ? std::string("s->ui_canvas") : ref(p) + ".rect") << ", " << ref(e)
              << ".layout, s->ui_scale);\n";
        }
        c << "}\n";

        c << "\nvoid " << prefix << "_draw_ui(" << type << " *s, SceneScaleMode mode) {\n";
        if (anyLabel) {
            c << "    const Font font = s->ui_font != NULL ? *s->ui_font : GetFontDefault();\n";
        }
        c << "    " << prefix << "_update_ui(s, mode);\n";
        for (const entt::entity e : ui) {
            const bool rect = registry.all_of<UIRectangleComponent>(e);
            const bool label = registry.all_of<UILabelComponent>(e);
            if (rect && label) {
                c << "    if (" << ref(e) << ".visible) {\n"
                  << "        DrawRectangleRec(" << ref(e) << ".rect, " << ref(e) << ".color);\n"
                  << "        scene_draw_ui_label(" << ref(e) << ".label, " << ref(e) << ".rect, s->ui_scale, font);\n"
                  << "    }\n";
            } else if (rect) {
                c << "    if (" << ref(e) << ".visible) DrawRectangleRec(" << ref(e) << ".rect, " << ref(e)
                  << ".color);\n";
            } else if (label) {
                c << "    if (" << ref(e) << ".visible) scene_draw_ui_label(" << ref(e) << ".label, " << ref(e)
                  << ".rect, s->ui_scale, font);\n";
            }
        }
        c << "}\n";
    }

    if (info.hasBodies) {
        // Fixed steps, then moving bodies back into their entities, parents first.
        c << "\nvoid " << prefix << "_step(" << type << " *s, float dt) {\n";
        for (const entt::entity e : drawn) {
            if (hasPlatformer(e)) {
                c << "    scene_platformer_update(&" << ref(e) << ", dt);\n";
            }
        }
        c << "    scene_physics_advance(s->physics_world, &s->physics_time, dt);\n";
        for (const entt::entity e : tree) {
            const auto* rb = registry.try_get<RigidbodyComponent>(e);
            if (rb == nullptr || rb->type == BodyType::Static) {
                continue;
            }
            std::string base = "SCENE_POSE_IDENTITY";
            std::vector<entt::entity> chain;
            for (entt::entity a = parentOf(e); a != entt::null; a = parentOf(a)) {
                chain.push_back(a);
            }
            for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
                base = "scene_child_pose(" + base + ", " + ref(*it) + ".transform)";
            }
            c << "    scene_body_to_entity(&" << ref(e) << ", " << base << ");\n";
        }
        c << "}\n"
          << "\nvoid " << prefix << "_deinit(" << type << " *s) {\n"
          << "    b2DestroyWorld(s->physics_world); /* and every body in it */\n"
          << "}\n";
    }

    const std::filesystem::path dir = outDir / kGeneratedDir;
    return writeFile(dir / (id + ".h"), h, error) && writeFile(dir / (id + ".c"), c.str(), error);
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
    std::ostringstream init;
    std::ostringstream update;
    std::ostringstream draw;
    std::ostringstream drawUI;
    std::ostringstream deinit;
    for (const ExportedScene& s : scenes) {
        const std::string prefix = "scene_" + s.id;
        const std::string constant = sceneIdConstant(s.name);
        const std::string member = "&m->" + s.member;
        includes << "#include \"" << s.id << ".h\"\n";
        ids << "    " << constant << ",\n";
        members << "    " << s.type << " " << s.member << ";\n";
        init << "    " << prefix << "_init(" << member << ");\n";
        if (s.hasBodies) {
            update << "    case " << constant << ": " << prefix << "_step(" << member << ", dt); break;\n";
            deinit << "    " << prefix << "_deinit(" << member << ");\n";
        }
        if (s.hasEntities) {
            draw << "    case " << constant << ": " << prefix << "_draw(" << member << "); break;\n";
        }
        if (s.hasUI) {
            drawUI << "    case " << constant << ": " << prefix << "_draw_ui(" << member << ", mode); break;\n";
        }
    }
    // Switches without cases still need their parameters used.
    const auto body = [](const std::ostringstream& cases, const std::string& unused) {
        if (cases.str().empty()) {
            return unused;
        }
        return "    switch (m->current) {\n" + cases.str() + "    default: break;\n    }\n";
    };
    const std::vector<std::pair<std::string, std::string>> values{
        {"marker", kGeneratedMarker},
        {"includes", includes.str()},
        {"ids", ids.str()},
        {"members", members.str()},
        {"init", init.str()},
        {"update", body(update, "    (void)m;\n    (void)dt;\n")},
        {"draw", body(draw, "    (void)m;\n")},
        {"draw_ui", body(drawUI, "    (void)m;\n    (void)mode;\n")},
        {"deinit", deinit.str().empty() ? "    (void)m;\n" : deinit.str()},
    };

    const std::string h = fill(R"GEN({{marker}}: every scene of the project. Do not edit.
#ifndef SCENE_MANAGER_H
#define SCENE_MANAGER_H

{{includes}}
#ifdef __cplusplus
extern "C" {
#endif

typedef enum SceneId {
{{ids}}    SCENE_ID_COUNT
} SceneId;

/* Every scene, and the one shown: set `current` to switch scenes. */
typedef struct SceneManager {
{{members}}    int current;
} SceneManager;

void scene_manager_init(SceneManager *m, int first);
void scene_manager_update(SceneManager *m, float dt); /* physics of the current scene */
void scene_manager_draw(SceneManager *m);             /* between scene_begin and scene_end */
void scene_manager_draw_ui(SceneManager *m, SceneScaleMode mode); /* after scene_end */
void scene_manager_deinit(SceneManager *m);

#ifdef __cplusplus
}
#endif

#endif /* SCENE_MANAGER_H */
)GEN",
                               values);

    const std::string c = fill(R"GEN({{marker}}: every scene of the project. Do not edit.
#include "scene_manager.h"

void scene_manager_init(SceneManager *m, int first) {
{{init}}    m->current = first;
}

void scene_manager_update(SceneManager *m, float dt) {
{{update}}}

void scene_manager_draw(SceneManager *m) {
{{draw}}}

void scene_manager_draw_ui(SceneManager *m, SceneScaleMode mode) {
{{draw_ui}}}

void scene_manager_deinit(SceneManager *m) {
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

#include <stdbool.h>

#include <raylib.h>

#define SCENE_PHYSICS {{physics}} /* Box2D (Project Settings > Physics); its headers need C11 */
#if SCENE_PHYSICS
#include <box2d/box2d.h>
#define SCENE_PIXELS_PER_METER {{pixels_per_meter}}
#define SCENE_GRAVITY_X {{gravity_x}} /* metres per second squared, y down */
#define SCENE_GRAVITY_Y {{gravity_y}}
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ----- Screen scaling: the design area, fitted to any window ----- */

#define SCENE_DESIGN_WIDTH {{width}}
#define SCENE_DESIGN_HEIGHT {{height}}

typedef enum SceneScaleMode {
{{mode_enum}}} SceneScaleMode;

#define SCENE_SCALE_MODE_COUNT {{mode_count}}
extern const SceneScaleMode scene_scale_modes[SCENE_SCALE_MODE_COUNT];
#define SCENE_DEFAULT_SCALE_MODE {{mode}}

/* The camera scene_begin uses: scene origin at the window centre, scaled for `mode`. */
Camera2D scene_camera(SceneScaleMode mode);
/* Between these, everything is drawn in scene coordinates. */
void scene_begin(SceneScaleMode mode);
void scene_end(void);

/* The Canvas UI is laid out on, in window pixels, and window pixels per canvas unit. */
Rectangle scene_ui_canvas(SceneScaleMode mode);
float scene_ui_scale(Rectangle canvas);

/* ----- Entities ----- */

/* Relative to the parent. Scene coordinates: (0, 0) is the scene origin, y points down;
   degrees. */
typedef struct SceneTransform {
    Vector2 position;
    float rotation;
    Vector2 scale;
    bool ignore_parent_rotation;
    bool ignore_parent_scale;
} SceneTransform;

/* Where an entity ends up in the scene. */
typedef struct ScenePose {
    Vector2 position;
    float rotation;
    Vector2 scale;
} ScenePose;

#define SCENE_POSE_IDENTITY ((ScenePose){.scale = {1.0f, 1.0f}})

typedef struct SceneSprite {
    Vector2 size; /* before scaling */
    Color tint;
} SceneSprite;

typedef struct SceneBoxCollider {
    Vector2 offset; /* from the entity's position, before scaling */
    Vector2 size;
    bool is_trigger;
    float density;
    float friction;
    float restitution;
} SceneBoxCollider;

#if SCENE_PHYSICS
/* A player-controlled platformer character (a dynamic body with a box collider): moves
   left / right and jumps. Speeds and heights in scene units.
     coyote time  a jump still works for a moment after leaving a ledge
     jump buffer  a jump pressed just before landing happens on landing
     jump cut     releasing jump early ends the jump lower */
typedef struct ScenePlatformer {
    float move_speed;     /* per second */
    float acceleration;   /* per second squared, on the ground */
    float air_control;    /* share of the acceleration in the air (0..1) */
    float jump_height;
    float coyote_time;    /* seconds */
    float jump_buffer;    /* seconds */
    float jump_cut;       /* upward speed kept when jump is released early (0..1) */
    float max_fall_speed; /* per second */

    /* Input. Read from the keyboard on each step (A / D or Left / Right; Space, W or Up)
       unless manual_input is set: then set these yourself before the step, e.g. from a
       gamepad. jump_pressed is cleared by the step. */
    bool manual_input;
    float move;        /* -1 left .. 1 right */
    bool jump_pressed; /* pressed since the last step */
    bool jump_held;

    /* State, for reading (e.g. to pick an animation). */
    bool grounded;
    float coyote_left; /* seconds */
    float buffer_left; /* seconds */
    bool rising;       /* in a jump that a release can still cut */
} ScenePlatformer;
#endif

typedef struct SceneEntity {
    SceneTransform transform;
    ScenePose pose;            /* from the last _update_poses or _draw */
    SceneSprite sprite;        /* if it has one */
    SceneBoxCollider collider; /* if it has one */
    bool visible;              /* false: its sprite isn't drawn */
#if SCENE_PHYSICS
    b2BodyId body;              /* if it has a rigidbody or a collider */
    ScenePlatformer platformer; /* if it has a platformer controller */
#endif
} SceneEntity;

/* A box placed in the scene: centre, scaled size, rotation in degrees. */
typedef struct SceneBox {
    Vector2 center;
    Vector2 size;
    float rotation;
} SceneBox;

/* Where a child with local transform `t` ends up. */
ScenePose scene_child_pose(ScenePose parent, SceneTransform t);

/* A box of `size` at `offset` from an entity at `pose`, e.g. its collider:
   scene_box(e.pose, e.collider.offset, e.collider.size). */
SceneBox scene_box(ScenePose pose, Vector2 offset, Vector2 size);

void scene_draw_rectangle(SceneSprite sprite, ScenePose pose);
void scene_draw_circle(SceneSprite sprite, ScenePose pose);
void scene_draw_triangle(SceneSprite sprite, ScenePose pose);

/* ----- UI: laid out on the Canvas in window pixels, sizes in canvas units ----- */

typedef struct SceneUIRectTransform {
    Vector2 anchor_min; /* points on the parent rectangle, as fractions (0..1) */
    Vector2 anchor_max;
    Vector2 pivot;      /* the point of the element (fractions of its size) that is placed */
    Vector2 position;   /* where the pivot goes, from the anchor point */
    Vector2 size;       /* on a stretched axis, added to the distance between the anchors */
} SceneUIRectTransform;

typedef enum SceneTextAlign {
    SCENE_ALIGN_START,
    SCENE_ALIGN_CENTER,
    SCENE_ALIGN_END
} SceneTextAlign;

typedef struct SceneUILabel {
    const char *text;
    float font_size; /* canvas units */
    float spacing;
    SceneTextAlign align_h;
    SceneTextAlign align_v;
    Color color;
} SceneUILabel;

typedef struct SceneUIElement {
    SceneUIRectTransform layout;
    Rectangle rect;     /* window pixels, from the last _update_ui or _draw_ui */
    Color color;        /* its UI rectangle, if it has one */
    SceneUILabel label; /* if it has one */
    bool visible;       /* false: its rectangle and label aren't drawn */
} SceneUIElement;

Rectangle scene_ui_layout(Rectangle parent, SceneUIRectTransform layout, float scale);
void scene_draw_ui_label(SceneUILabel label, Rectangle rect, float ui_scale, Font font);
/* `text` (lines split at '\n') aligned inside `rect`; sizes in pixels. */
void scene_draw_ui_text(Font font, const char *text, Rectangle rect, float font_size, float spacing,
                        SceneTextAlign align_h, SceneTextAlign align_v, Color color);

#if SCENE_PHYSICS
/* ----- Physics: Box2D works in metres, in the World's own space ----- */

b2Vec2 scene_to_meters(Vector2 v);
Vector2 scene_to_scene_units(b2Vec2 m);

/* Used by the generated scenes. */
b2WorldId scene_physics_create_world(void);
b2BodyDef scene_body_def(ScenePose pose, b2BodyType type, SceneEntity *entity);
void scene_add_box_collider(b2BodyId body, Vector2 scale, SceneBoxCollider collider, SceneEntity *entity);
void scene_physics_advance(b2WorldId world, float *time, float dt);
void scene_body_to_entity(SceneEntity *entity, ScenePose parent);

/* Moves a platformer character for this frame (before the physics steps): input, ground,
   coyote time, jump buffer, jump cut, acceleration. The scene's _step calls it. */
void scene_platformer_update(SceneEntity *entity, float dt);
#endif

#ifdef __cplusplus
}
#endif

#endif /* SCENE_COMMON_H */
)GEN",
                               values);

    const std::string c = fill(R"GEN({{marker}}: code shared by every exported scene. Do not edit.
#include "scene_common.h"

#include <math.h>
#include <string.h>

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

/* A point `p` (entity units, before scaling) of an entity at `pose`, in the scene. */
static Vector2 place(ScenePose pose, Vector2 p) {
    const float c = cosf(pose.rotation * DEG2RAD);
    const float s = sinf(pose.rotation * DEG2RAD);
    const float x = p.x * pose.scale.x;
    const float y = p.y * pose.scale.y;
    return (Vector2){pose.position.x + x * c - y * s, pose.position.y + x * s + y * c};
}

ScenePose scene_child_pose(ScenePose parent, SceneTransform t) {
    ScenePose base = parent;
    ScenePose pose;
    if (t.ignore_parent_rotation) {
        base.rotation = 0.0f;
    }
    if (t.ignore_parent_scale) {
        base.scale = (Vector2){1.0f, 1.0f};
    }
    pose.position = place(base, t.position);
    pose.rotation = base.rotation + t.rotation;
    pose.scale = (Vector2){base.scale.x * t.scale.x, base.scale.y * t.scale.y};
    return pose;
}

SceneBox scene_box(ScenePose pose, Vector2 offset, Vector2 size) {
    SceneBox box;
    box.center = place(pose, offset);
    box.size = (Vector2){size.x * fabsf(pose.scale.x), size.y * fabsf(pose.scale.y)};
    box.rotation = pose.rotation;
    return box;
}

/* raylib only draws triangles whose corners run counter-clockwise on screen. */
static void draw_triangle(Vector2 a, Vector2 b, Vector2 c, Color color) {
    if ((b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x) > 0.0f) {
        DrawTriangle(a, c, b, color);
    } else {
        DrawTriangle(a, b, c, color);
    }
}

void scene_draw_rectangle(SceneSprite sprite, ScenePose pose) {
    const SceneBox box = scene_box(pose, (Vector2){0.0f, 0.0f}, sprite.size);
    DrawRectanglePro((Rectangle){box.center.x, box.center.y, box.size.x, box.size.y},
                     (Vector2){box.size.x / 2.0f, box.size.y / 2.0f}, box.rotation, sprite.tint);
}

void scene_draw_circle(SceneSprite sprite, ScenePose pose) {
    const int segments = 36;
    const float rx = sprite.size.x / 2.0f;
    const float ry = sprite.size.y / 2.0f;
    Vector2 previous = place(pose, (Vector2){rx, 0.0f});
    int i;
    for (i = 1; i <= segments; ++i) {
        const float angle = 2.0f * PI * (float)i / (float)segments;
        const Vector2 next = place(pose, (Vector2){rx * cosf(angle), ry * sinf(angle)});
        draw_triangle(pose.position, previous, next, sprite.tint);
        previous = next;
    }
}

void scene_draw_triangle(SceneSprite sprite, ScenePose pose) {
    const float hw = sprite.size.x / 2.0f;
    const float hh = sprite.size.y / 2.0f;
    draw_triangle(place(pose, (Vector2){0.0f, -hh}), place(pose, (Vector2){-hw, hh}), place(pose, (Vector2){hw, hh}),
                  sprite.tint);
}

Rectangle scene_ui_layout(Rectangle parent, SceneUIRectTransform t, float scale) {
    const float min_x = parent.x + t.anchor_min.x * parent.width;
    const float min_y = parent.y + t.anchor_min.y * parent.height;
    const float span_x = (t.anchor_max.x - t.anchor_min.x) * parent.width;
    const float span_y = (t.anchor_max.y - t.anchor_min.y) * parent.height;
    Rectangle r;
    r.width = span_x + t.size.x * scale;
    r.height = span_y + t.size.y * scale;
    r.x = min_x + span_x * t.pivot.x + t.position.x * scale - t.pivot.x * r.width;
    r.y = min_y + span_y * t.pivot.y + t.position.y * scale - t.pivot.y * r.height;
    return r;
}

void scene_draw_ui_label(SceneUILabel label, Rectangle rect, float ui_scale, Font font) {
    scene_draw_ui_text(font, label.text, rect, label.font_size * ui_scale, label.spacing * ui_scale, label.align_h,
                       label.align_v, label.color);
}

static float align_fraction(SceneTextAlign align) {
    return align == SCENE_ALIGN_START ? 0.0f : align == SCENE_ALIGN_END ? 1.0f : 0.5f;
}

void scene_draw_ui_text(Font font, const char *text, Rectangle rect, float font_size, float spacing,
                        SceneTextAlign align_h, SceneTextAlign align_v, Color color) {
    const float line_height = font_size * 1.2f;
    char line[1024];
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
        size_t n = strcspn(text, "\n");
        const size_t kept = n < sizeof(line) - 1 ? n : sizeof(line) - 1;
        float x;
        memcpy(line, text, kept);
        line[kept] = '\0';
        x = rect.x + (rect.width - MeasureTextEx(font, line, font_size, spacing).x) * align_fraction(align_h);
        DrawTextEx(font, line, (Vector2){x, y}, font_size, spacing, color);
        if (text[n] == '\0') {
            break;
        }
        text += n + 1;
        y += line_height;
    }
}

#if SCENE_PHYSICS
#define SCENE_PHYSICS_TIME_STEP (1.0f / 60.0f)

b2Vec2 scene_to_meters(Vector2 v) {
    return (b2Vec2){v.x / SCENE_PIXELS_PER_METER, v.y / SCENE_PIXELS_PER_METER};
}

Vector2 scene_to_scene_units(b2Vec2 m) {
    return (Vector2){m.x * SCENE_PIXELS_PER_METER, m.y * SCENE_PIXELS_PER_METER};
}

b2WorldId scene_physics_create_world(void) {
    b2WorldDef def = b2DefaultWorldDef();
    def.gravity = (b2Vec2){SCENE_GRAVITY_X, SCENE_GRAVITY_Y};
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

void scene_add_box_collider(b2BodyId body, Vector2 scale, SceneBoxCollider collider, SceneEntity *entity) {
    const Vector2 centre = {collider.offset.x * scale.x, collider.offset.y * scale.y};
    const b2Polygon box = b2MakeOffsetBox(collider.size.x * fabsf(scale.x) / 2.0f / SCENE_PIXELS_PER_METER,
                                          collider.size.y * fabsf(scale.y) / 2.0f / SCENE_PIXELS_PER_METER,
                                          scene_to_meters(centre), b2Rot_identity);
    b2ShapeDef shape = b2DefaultShapeDef();
    shape.density = collider.density;
    shape.material.friction = collider.friction;
    shape.material.restitution = collider.restitution;
    shape.isSensor = collider.is_trigger;
    shape.enableSensorEvents = true;
    shape.enableContactEvents = true;
    shape.userData = entity;
    b2CreatePolygonShape(body, &shape, &box);
}

/* Fixed steps of 1/60 s for the time passed (at most a quarter second's worth). */
void scene_physics_advance(b2WorldId world, float *time, float dt) {
    *time += dt < 0.25f ? dt : 0.25f;
    while (*time >= SCENE_PHYSICS_TIME_STEP) {
        b2World_Step(world, SCENE_PHYSICS_TIME_STEP, 4);
        *time -= SCENE_PHYSICS_TIME_STEP;
    }
}

/* The entity's transform, from where its body is; `parent` is the parent's placement in
   the World's own space. */
void scene_body_to_entity(SceneEntity *entity, ScenePose parent) {
    SceneTransform *t = &entity->transform;
    const Vector2 position = scene_to_scene_units(b2Body_GetPosition(entity->body));
    const float rotation = b2Rot_GetAngle(b2Body_GetRotation(entity->body)) * RAD2DEG;
    const float parent_rotation = t->ignore_parent_rotation ? 0.0f : parent.rotation;
    const Vector2 scale = t->ignore_parent_scale ? (Vector2){1.0f, 1.0f} : parent.scale;
    const float c = cosf(-parent_rotation * DEG2RAD);
    const float s = sinf(-parent_rotation * DEG2RAD);
    const float dx = position.x - parent.position.x;
    const float dy = position.y - parent.position.y;
    if (scale.x != 0.0f) {
        t->position.x = (dx * c - dy * s) / scale.x;
    }
    if (scale.y != 0.0f) {
        t->position.y = (dx * s + dy * c) / scale.y;
    }
    t->rotation = rotation - parent_rotation;
}

/* Standing on something: a contact that pushes the body up (y points down). */
static bool on_ground(b2BodyId body) {
    b2ContactData contacts[16];
    const int count = b2Body_GetContactData(body, contacts, 16);
    int i;
    for (i = 0; i < count; ++i) {
        const b2Manifold *m = &contacts[i].manifold;
        /* The normal points from shape A to shape B: turn it to point from this body to
           the other one; the ground is below when it points down. */
        const b2BodyId a = b2Shape_GetBody(contacts[i].shapeIdA);
        const float down = B2_ID_EQUALS(a, body) ? m->normal.y : -m->normal.y;
        if (m->pointCount > 0 && down > 0.7f) {
            return true;
        }
    }
    return false;
}

static float approach(float value, float target, float step) {
    return value < target ? fminf(value + step, target) : fmaxf(value - step, target);
}

void scene_platformer_update(SceneEntity *entity, float dt) {
    ScenePlatformer *p = &entity->platformer;
    b2Vec2 v;
    float vx, vy;
    bool want_jump;
    if (!b2Body_IsValid(entity->body)) {
        return;
    }
    if (!p->manual_input) {
        const bool right = IsKeyDown(KEY_D) || IsKeyDown(KEY_RIGHT);
        const bool left = IsKeyDown(KEY_A) || IsKeyDown(KEY_LEFT);
        p->move = (float)right - (float)left;
        p->jump_pressed = p->jump_pressed || IsKeyPressed(KEY_SPACE) || IsKeyPressed(KEY_W) || IsKeyPressed(KEY_UP);
        p->jump_held = IsKeyDown(KEY_SPACE) || IsKeyDown(KEY_W) || IsKeyDown(KEY_UP);
    }
    v = b2Body_GetLinearVelocity(entity->body);
    vx = v.x * SCENE_PIXELS_PER_METER;
    vy = v.y * SCENE_PIXELS_PER_METER;

    /* On the ground (and not on the way up): coyote time starts over. */
    p->grounded = vy > -1.0f && on_ground(entity->body);
    if (p->grounded) {
        p->coyote_left = p->coyote_time;
        p->rising = false;
    } else {
        p->coyote_left = fmaxf(0.0f, p->coyote_left - dt);
    }

    /* A press is remembered for jump_buffer seconds. */
    p->buffer_left = p->jump_pressed ? p->jump_buffer : fmaxf(0.0f, p->buffer_left - dt);
    want_jump = p->jump_pressed || p->buffer_left > 0.0f;
    if (want_jump && (p->grounded || p->coyote_left > 0.0f)) {
        /* The speed that reaches jump_height against this body's gravity. */
        const float gravity = fabsf(SCENE_GRAVITY_Y * b2Body_GetGravityScale(entity->body)) * SCENE_PIXELS_PER_METER;
        vy = -sqrtf(2.0f * gravity * p->jump_height);
        p->grounded = false;
        p->coyote_left = 0.0f;
        p->buffer_left = 0.0f;
        p->rising = true;
    }

    /* Jump cut: let go early, jump lower. */
    if (p->rising && vy >= 0.0f) {
        p->rising = false;
    } else if (p->rising && !p->jump_held) {
        vy *= p->jump_cut;
        p->rising = false;
    }

    /* Run: towards move * move_speed, with less grip in the air. */
    vx = approach(vx, p->move * p->move_speed, p->acceleration * (p->grounded ? 1.0f : p->air_control) * dt);
    vy = fminf(vy, p->max_fall_speed);

    p->jump_pressed = false;
    b2Body_SetLinearVelocity(entity->body, (b2Vec2){vx / SCENE_PIXELS_PER_METER, vy / SCENE_PIXELS_PER_METER});
}
#endif
)GEN",
                               values);

    const std::filesystem::path dir = outDir / kUtilsDir;
    return writeFile(dir / "scene_common.h", h, error) && writeFile(dir / "scene_common.c", c, error);
}

bool writeProjectMain(const std::string& projectName, const std::filesystem::path& outDir, std::string& error) {
    const std::string c = fill(R"CODE({{marker}}: example main() for project "{{project}}". Do not edit:
// re-export from the editor instead. To change it, copy it to your own file in src/ and
// build with make EXCLUDE=src/project_main.c. Tab / Shift+Tab switch scenes.
#include <raylib.h>

#include "scene_manager.h"

int main(void) {
    static SceneManager scenes;
    const SceneScaleMode mode = SCENE_DEFAULT_SCALE_MODE;

    SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_VSYNC_HINT | FLAG_MSAA_4X_HINT);
    InitWindow((int)SCENE_DESIGN_WIDTH, (int)SCENE_DESIGN_HEIGHT, "{{project}}");
    SetTargetFPS(60);
    scene_manager_init(&scenes, 0);

    while (!WindowShouldClose()) {
        if (IsKeyPressed(KEY_TAB)) {
            const int step = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT) ? SCENE_ID_COUNT - 1 : 1;
            scenes.current = (scenes.current + step) % SCENE_ID_COUNT;
        }
        scene_manager_update(&scenes, GetFrameTime());

        BeginDrawing();
        ClearBackground((Color){20, 20, 23, 255});
        scene_begin(mode);
        scene_manager_draw(&scenes);
        /* Your drawing in scene coordinates, e.g. at an entity's pose.position. */
        scene_end();
        scene_manager_draw_ui(&scenes, mode);
        /* Your UI in window pixels, e.g. inside a UI element's rect. */
        EndDrawing();
    }

    scene_manager_deinit(&scenes);
    CloseWindow();
    return 0;
}
)CODE",
                               {{"marker", kGeneratedMarker}, {"project", cString(projectName)}});
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
    // extension finds a compiler by itself.
    const json configuration{{"name", "Release"},
                             {"includePath",
                              {"${workspaceFolder}/src", "${workspaceFolder}/generated", "${workspaceFolder}/utils",
                               "${workspaceFolder}/box2d/include", "${env:RAYLIB_INCLUDE}", "${env:RAYLIB_SRC}"}},
                             {"defines", {"NDEBUG"}},
                             {"cStandard", "c11"},
                             {"cppStandard", "c++17"}};
    const json properties{{"configurations", {configuration}}, {"version", 4}};

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
         {outDir, outDir / kSrcDir, outDir / kGeneratedDir, outDir / kUtilsDir, outDir / ".vscode", outDir / "debug"}) {
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

bool writeMakefile(const std::string& projectName, const std::filesystem::path& outDir, std::string& error) {
    // Recipe lines must start with a tab.
    const std::string makefile = fill(R"MAKE(# Generated by Scene Editor for project "{{project}}". Do not edit:
# re-export from the editor instead.
#
# Builds every .c in src/, generated/ and utils/ (and Box2D from box2d/, when physics is
# on) into bin/{{target}}. Object files go to bin/obj/; after changing CFLAGS or the
# compiler, run make clean.
#
#   make          build bin/{{target}}
#   make run      build and run it
#   make clean    delete bin/
#
# raylib: an installed raylib is used by default (-lraylib). Otherwise, either
#   make RAYLIB_SRC=path/to/raylib/src
#       builds raylib from its sources with the same compiler (once, into bin/raylib/).
#       Always links, whatever the compiler: the simplest choice.
#   make RAYLIB_INCLUDE=path/to/raylib/src RAYLIB_LIB_DIR=path/to/folder/with/libraylib.a
#       links a prebuilt raylib (libraylib.a or raylib.lib in that folder); RAYLIB_LIBS=...
#       gives the exact link input instead. A prebuilt library only links with the compiler
#       family that built it ("undefined reference to __chkstk": use RAYLIB_SRC instead).
# Your own main(): make EXCLUDE=src/project_main.c
# Compiler: make CC=clang  (needs GNU make; on Windows e.g. mingw32-make).

TARGET_NAME := {{target}}
BIN_DIR := bin
INCLUDE_DIRS := src generated utils
EXCLUDE ?=
SOURCES := $(filter-out $(EXCLUDE),$(foreach dir,$(INCLUDE_DIRS),$(wildcard $(dir)/*.c)))
CFLAGS ?= -std=c11 -O2 -Wall -Wextra
CPPFLAGS += $(addprefix -I,$(INCLUDE_DIRS))

RAYLIB_INCLUDE ?=
ifneq ($(strip $(RAYLIB_INCLUDE)),)
    CPPFLAGS += -I$(RAYLIB_INCLUDE)
endif
RAYLIB_LIB_DIR ?=
ifeq ($(origin RAYLIB_LIBS),undefined)
    ifneq ($(strip $(RAYLIB_LIB_DIR)),)
        RAYLIB_LIBS := $(or $(firstword $(wildcard $(RAYLIB_LIB_DIR)/libraylib.a $(RAYLIB_LIB_DIR)/raylib.lib)),-L$(RAYLIB_LIB_DIR) -lraylib)
    else
        RAYLIB_LIBS := -lraylib
    endif
endif

# Box2D, when the export has it: built once into bin/box2d/, with its own flags (C17).
BOX2D_SOURCES := $(wildcard box2d/src/*.c)
ifneq ($(strip $(BOX2D_SOURCES)),)
    BOX2D_OBJECTS := $(BOX2D_SOURCES:box2d/src/%.c=$(BIN_DIR)/box2d/%.o)
    CPPFLAGS += -Ibox2d/include
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

# RAYLIB_SRC: raylib's own sources, compiled with this compiler, as gnu99 without warnings.
RAYLIB_SRC ?=
ifneq ($(strip $(RAYLIB_SRC)),)
    RAYLIB_OBJECTS := $(addprefix $(BIN_DIR)/raylib/,rcore.o rshapes.o rtextures.o rtext.o rmodels.o raudio.o rglfw.o)
    RAYLIB_CFLAGS := -std=gnu99 -w -DPLATFORM_DESKTOP -DGRAPHICS_API_OPENGL_33 $(RAYLIB_PLATFORM_CFLAGS) -I$(RAYLIB_SRC) -I$(RAYLIB_SRC)/external/glfw/include
    CPPFLAGS += -I$(RAYLIB_SRC)
    RAYLIB_LIBS :=
endif

TARGET := $(BIN_DIR)/$(TARGET_NAME)$(EXE)
OBJECTS := $(SOURCES:%.c=$(BIN_DIR)/obj/%.o)

.PHONY: all run clean
all: $(TARGET)

$(TARGET): $(OBJECTS) $(RAYLIB_OBJECTS) $(BOX2D_OBJECTS)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS) $(RAYLIB_LIBS) $(PLATFORM_LIBS)

$(BIN_DIR)/obj/%.o: %.c
	@$(call MKDIR,$(@D))
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c $< -o $@

$(BIN_DIR)/raylib/%.o: $(RAYLIB_SRC)/%.c
	@$(call MKDIR,$(@D))
	$(CC) $(CFLAGS) $(RAYLIB_CFLAGS) -c $(RAYLIB_LANG) $< -o $@

$(BIN_DIR)/box2d/%.o: box2d/src/%.c
	@$(call MKDIR,$(@D))
	$(CC) $(CFLAGS) -std=gnu17 -ffp-contract=off -w -Ibox2d/include -Ibox2d/src -c $< -o $@

# macOS: GLFW's Cocoa backend is Objective-C.
ifdef RAYLIB_DARWIN
$(BIN_DIR)/raylib/rglfw.o: RAYLIB_LANG := -x objective-c
endif

run: $(TARGET)
	"$(CURDIR)/$(TARGET)"

clean:
	-$(call RMDIR,$(BIN_DIR))

-include $(OBJECTS:.o=.d)
)MAKE",
                                      {{"project", cString(projectName)}, {"target", sceneIdentifier(projectName)}});
    return writeFile(outDir / "Makefile", makefile, error);
}
