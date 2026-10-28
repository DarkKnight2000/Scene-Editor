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

std::string entityTypeName(EntityFields fields) {
    std::string name = "Scene";
    if (fields.sprite) {
        name += "Sprite";
    }
    if (fields.collider) {
        name += "Collider";
    }
    if (fields.body) {
        name += "Body";
    }
    if (fields.platformer) {
        name += "Platformer";
    }
    return name + "Entity";
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

const char* colliderShapeConstant(ColliderShape shape) {
    switch (shape) {
    case ColliderShape::Circle: return "SCENE_COLLIDER_CIRCLE";
    case ColliderShape::Triangle: return "SCENE_COLLIDER_TRIANGLE";
    case ColliderShape::Box: break;
    }
    return "SCENE_COLLIDER_BOX";
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

    // Scene entities in drawing order; scene entities and UI elements in tree order (parents
    // first: poses and rectangles are computed in that order).
    std::vector<const Entity*> drawn;
    for (const Entity& e : scene.entities()) {
        if (!scene.isRoot(e.id) && !e.rectTransform) {
            drawn.push_back(&e);
        }
    }
    std::vector<const Entity*> tree;
    std::vector<const Entity*> ui;
    for (const HierarchyRow& row : scene.hierarchy()) {
        if (!scene.isRoot(row.entity)) {
            const Entity& e = scene.get(row.entity);
            (e.rectTransform ? ui : tree).push_back(&e);
        }
    }

    // A field per entity and UI element, named after it.
    std::unordered_set<std::string> used{"world", "ui_font", "ui_canvas", "ui_scale", "physics_world", "physics_time"};
    std::unordered_map<EntityId, std::string> field;
    for (const Entity* e : drawn) {
        field[e->id] = unique(fieldIdentifier(e->name), used);
    }
    for (const Entity* e : ui) {
        field[e->id] = unique(fieldIdentifier(e->name), used);
    }
    const auto ref = [&](const Entity* e) { return "s->" + field.at(e->id); };
    // The parent entity or element; nullptr for the World / the Canvas.
    const auto parentOf = [&](const Entity* e) -> const Entity* {
        return field.count(e->parent) > 0 ? &scene.get(e->parent) : nullptr;
    };

    // Physics bodies: entities with a rigidbody or a collider (a collider alone: static), made
    // in drawing order.
    std::vector<const Entity*> bodies;
    if (physics) {
        for (const Entity* e : drawn) {
            if (e->rigidbody || e->collider) {
                bodies.push_back(e);
            }
        }
    }
    bool anyLabel = false;
    for (const Entity* e : ui) {
        anyLabel = anyLabel || e->uiLabel;
    }
    // A platformer controller moves a dynamic body; without one there's nothing to move.
    const auto hasPlatformer = [&](const Entity* e) {
        return physics && e->platformer && e->rigidbody && e->rigidbody->type == BodyType::Dynamic;
    };
    const auto fieldsOf = [&](const Entity* e) {
        return EntityFields{.sprite = e->sprite && e->sprite->exported,
                            .collider = e->collider.has_value(),
                            .body = physics && (e->rigidbody || e->collider),
                            .platformer = hasPlatformer(e)};
    };

    info = ExportedScene{};
    info.name = sceneName;
    info.id = id;
    info.type = type;
    info.member = fieldIdentifier(id) == "current" ? "current_scene" : fieldIdentifier(id);
    info.hasEntities = !drawn.empty();
    info.hasUI = !ui.empty();
    info.hasBodies = !bodies.empty();
    for (const Entity* e : drawn) {
        info.entities.push_back(field.at(e->id));
        info.entityTypes.insert(fieldsOf(e));
        if (e->collider) {
            info.colliders.push_back(field.at(e->id));
        }
    }
    for (const Entity* e : ui) {
        info.elements.push_back(field.at(e->id));
    }

    // --- Header ---
    std::ostringstream fields;
    for (const Entity* e : drawn) {
        fields << "    " << entityTypeName(fieldsOf(e)) << " " << field.at(e->id) << "; /* \"" << commentText(e->name)
               << "\" */\n";
    }
    for (const Entity* e : ui) {
        fields << "    SceneUIElement " << field.at(e->id) << "; /* \"" << commentText(e->name) << "\" */\n";
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
        decls << "void " << prefix << "_update_ui(" << type << " *s);\n"
              << "void " << prefix << "_draw_ui(" << type << " *s);\n";
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
      << "        .world = " << transformInit(*scene.get(scene.world()).transform) << ",\n";
    for (const Entity* e : drawn) {
        c << "        ." << field.at(e->id) << " = {.transform = " << transformInit(*e->transform);
        if (const auto& sprite = e->sprite; sprite && sprite->exported) {
            c << ", .sprite = {" << pair(sprite->size) << ", " << rgba(sprite->tint) << "}, .visible = true";
        }
        if (const auto& col = e->collider) {
            c << ", .collider = {.shape = " << colliderShapeConstant(col->shape) << ", .offset = " << pair(col->offset);
            if (col->shape == ColliderShape::Circle) {
                c << ", .radius = " << floatLiteral(col->radius);
            } else {
                c << ", .size = " << pair(col->size);
            }
            if (col->isTrigger) {
                c << ", .is_trigger = true";
            }
            c << ", .density = " << floatLiteral(col->density) << ", .friction = " << floatLiteral(col->friction)
              << ", .restitution = " << floatLiteral(col->restitution) << "}";
        }
        if (const auto& pc = e->platformer; pc && hasPlatformer(e)) {
            c << ", .platformer = {.move_speed = " << floatLiteral(pc->moveSpeed)
              << ", .acceleration = " << floatLiteral(pc->acceleration) << ", .air_control = " << floatLiteral(pc->airControl)
              << ", .jump_height = " << floatLiteral(pc->jumpHeight) << ", .coyote_time = " << floatLiteral(pc->coyoteTime)
              << ", .jump_buffer = " << floatLiteral(pc->jumpBuffer) << ", .jump_cut = " << floatLiteral(pc->jumpCut)
              << ", .max_fall_speed = " << floatLiteral(pc->maxFallSpeed) << "}";
        }
        c << "},\n";
    }
    for (const Entity* e : ui) {
        const UIRectTransformComponent& u = *e->rectTransform;
        c << "        ." << field.at(e->id) << " = {.layout = {" << pair(u.anchorMin) << ", " << pair(u.anchorMax) << ", "
          << pair(u.pivot) << ", " << pair(u.position) << ", " << pair(u.size) << "}";
        if (const auto& rect = e->uiRectangle) {
            c << ", .color = " << rgba(rect->color);
        }
        if (const auto& label = e->uiLabel) {
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
        std::unordered_set<const Entity*> needed;
        for (const Entity* e : bodies) {
            for (const Entity* a = e; a != nullptr; a = parentOf(a)) {
                needed.insert(a);
            }
        }
        c << "\n    /* Physics: a body for each entity with a rigidbody or a collider. */\n"
          << "    s->physics_world = scene_physics_create_world();\n"
          << "    {\n";
        for (const Entity* e : tree) {
            if (needed.count(e) > 0) {
                const Entity* p = parentOf(e);
                c << "        const ScenePose p_" << field.at(e->id) << " = scene_child_pose("
                  << (p == nullptr ? std::string("SCENE_POSE_IDENTITY") : "p_" + field.at(p->id)) << ", " << ref(e)
                  << ".transform);\n";
            }
        }
        c << "        b2BodyDef def;\n";
        for (const Entity* e : bodies) {
            const auto& rb = e->rigidbody;
            const std::string pose = "p_" + field.at(e->id);
            c << "        def = scene_body_def(" << pose << ", "
              << bodyTypeConstant(rb ? rb->type : BodyType::Static) << ", &" << ref(e) << ".entity);\n";
            if (rb) {
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
            if (e->collider) {
                c << "        scene_add_collider(" << ref(e) << ".body, " << pose << ".scale, " << ref(e)
                  << ".collider, &" << ref(e) << ".entity);\n";
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
        for (const Entity* e : tree) {
            const Entity* p = parentOf(e);
            c << "    " << ref(e) << ".pose = scene_child_pose("
              << (p == nullptr ? std::string("world") : ref(p) + ".pose") << ", " << ref(e) << ".transform);\n";
        }
        c << "}\n";

        // Sprites, in drawing order.
        c << "\nvoid " << prefix << "_draw(" << type << " *s) {\n"
          << "    " << prefix << "_update_poses(s);\n";
        for (const Entity* e : drawn) {
            const auto& sprite = e->sprite;
            if (sprite && sprite->exported) {
                c << "    if (" << ref(e) << ".visible) scene_draw_" << shapeName(sprite->shape) << "(" << ref(e)
                  << ".sprite, " << ref(e) << ".pose);\n";
            }
        }
        c << "}\n";
    }

    if (info.hasUI) {
        // Layout, parents first; then drawing in the same order.
        c << "\nvoid " << prefix << "_update_ui(" << type << " *s) {\n"
          << "    s->ui_canvas = scene_ui_canvas();\n"
          << "    s->ui_scale = scene_ui_scale(s->ui_canvas);\n";
        for (const Entity* e : ui) {
            const Entity* p = parentOf(e);
            c << "    " << ref(e) << ".rect = scene_ui_layout("
              << (p == nullptr ? std::string("s->ui_canvas") : ref(p) + ".rect") << ", " << ref(e)
              << ".layout, s->ui_scale);\n";
        }
        c << "}\n";

        c << "\nvoid " << prefix << "_draw_ui(" << type << " *s) {\n";
        if (anyLabel) {
            c << "    const Font font = s->ui_font != NULL ? *s->ui_font : GetFontDefault();\n";
        }
        c << "    " << prefix << "_update_ui(s);\n";
        for (const Entity* e : ui) {
            const bool rect = e->uiRectangle.has_value();
            const bool label = e->uiLabel.has_value();
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
        for (const Entity* e : drawn) {
            if (hasPlatformer(e)) {
                c << "    scene_platformer_update(&" << ref(e) << ".platformer, " << ref(e) << ".body, dt);\n";
            }
        }
        c << "    scene_physics_advance(s->physics_world, &s->physics_time, dt);\n";
        for (const Entity* e : tree) {
            if (!e->rigidbody || e->rigidbody->type == BodyType::Static) {
                continue;
            }
            std::string base = "SCENE_POSE_IDENTITY";
            std::vector<const Entity*> chain;
            for (const Entity* a = parentOf(e); a != nullptr; a = parentOf(a)) {
                chain.push_back(a);
            }
            for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
                base = "scene_child_pose(" + base + ", " + ref(*it) + ".transform)";
            }
            c << "    scene_body_to_entity(" << ref(e) << ".body, &" << ref(e) << ".entity, " << base << ");\n";
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

bool writeSceneManager(const std::vector<ExportedScene>& scenes, const std::filesystem::path& outDir,
                       const std::string& startScene, std::string& error) {
    std::string start = sceneIdConstant(scenes.front().name);
    for (const ExportedScene& s : scenes) {
        if (s.name == startScene) {
            start = sceneIdConstant(s.name);
        }
    }
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
            drawUI << "    case " << constant << ": " << prefix << "_draw_ui(" << member << "); break;\n";
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
        {"draw_ui", body(drawUI, "    (void)m;\n")},
        {"deinit", deinit.str().empty() ? "    (void)m;\n" : deinit.str()},
        {"start", start},
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

/* The scene the game starts in: the first one, or with Play in the editor the one shown. */
#define SCENE_ID_START {{start}}

/* Every scene, and the one shown: set `current` to switch scenes. */
typedef struct SceneManager {
{{members}}    int current;
} SceneManager;

void scene_manager_init(SceneManager *m, int first);
void scene_manager_update(SceneManager *m, float dt); /* physics of the current scene */
void scene_manager_draw(SceneManager *m);             /* between scene_begin and scene_end */
void scene_manager_draw_ui(SceneManager *m); /* after scene_end */
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

void scene_manager_draw_ui(SceneManager *m) {
{{draw_ui}}}

void scene_manager_deinit(SceneManager *m) {
{{deinit}}}
)GEN",
                               values);

    const std::filesystem::path dir = outDir / kGeneratedDir;
    return writeFile(dir / "scene_manager.h", h, error) && writeFile(dir / "scene_manager.c", c, error);
}

namespace {
// What scene_common says about the project's scale mode.
const char* scaleModeNote(ScaleMode mode) {
    switch (mode) {
    case ScaleMode::Fit: return "the whole design area is visible; extra space becomes black bars";
    case ScaleMode::Fill: return "the window is fully covered; the design area is cropped to fit";
    case ScaleMode::Expand: break;
    }
    return "the whole design area is visible; a wider or taller window shows more of the scene";
}

// scene_camera, scene_begin, scene_end and scene_ui_canvas for the project's scale mode.
std::string scalingCode(ScaleMode mode) {
    const bool fit = mode == ScaleMode::Fit;
    std::string code = std::string("Camera2D scene_camera(void) {\n"
                                   "    const float screen_w = (float)GetScreenWidth();\n"
                                   "    const float screen_h = (float)GetScreenHeight();\n"
                                   "    const float scale_x = screen_w / SCENE_DESIGN_WIDTH;\n"
                                   "    const float scale_y = screen_h / SCENE_DESIGN_HEIGHT;\n"
                                   "    Camera2D camera = {0};\n"
                                   "    camera.offset.x = screen_w * 0.5f;\n"
                                   "    camera.offset.y = screen_h * 0.5f;\n") +
                       (mode == ScaleMode::Fill ? "    camera.zoom = scale_x > scale_y ? scale_x : scale_y; /* covers the window */\n"
                                                : "    camera.zoom = scale_x < scale_y ? scale_x : scale_y; /* fits the design area */\n") +
                       "    return camera;\n"
                       "}\n\n";
    if (fit) {
        code += "/* The design area in window pixels: where the scene is drawn, inside the bars. */\n"
                "static Rectangle design_rect_on_screen(void) {\n"
                "    const Camera2D camera = scene_camera();\n"
                "    const float w = SCENE_DESIGN_WIDTH * camera.zoom;\n"
                "    const float h = SCENE_DESIGN_HEIGHT * camera.zoom;\n"
                "    Rectangle r;\n"
                "    r.x = camera.offset.x - w * 0.5f;\n"
                "    r.y = camera.offset.y - h * 0.5f;\n"
                "    r.width = w;\n"
                "    r.height = h;\n"
                "    return r;\n"
                "}\n\n"
                "void scene_begin(void) {\n"
                "    const Rectangle r = design_rect_on_screen();\n"
                "    BeginScissorMode((int)r.x, (int)r.y, (int)r.width, (int)r.height);\n"
                "    BeginMode2D(scene_camera());\n"
                "}\n\n"
                "void scene_end(void) {\n"
                "    const Rectangle r = design_rect_on_screen();\n"
                "    const int screen_w = GetScreenWidth();\n"
                "    const int screen_h = GetScreenHeight();\n"
                "    EndMode2D();\n"
                "    EndScissorMode();\n"
                "    /* Black bars outside the design area. */\n"
                "    DrawRectangle(0, 0, screen_w, (int)r.y, BLACK);\n"
                "    DrawRectangle(0, (int)(r.y + r.height), screen_w, screen_h, BLACK);\n"
                "    DrawRectangle(0, 0, (int)r.x, screen_h, BLACK);\n"
                "    DrawRectangle((int)(r.x + r.width), 0, screen_w, screen_h, BLACK);\n"
                "}\n\n"
                "/* The Canvas: the design area inside the bars. One canvas unit is ui_scale pixels. */\n"
                "Rectangle scene_ui_canvas(void) {\n"
                "    return design_rect_on_screen();\n"
                "}\n";
    } else {
        code += "void scene_begin(void) {\n"
                "    BeginMode2D(scene_camera());\n"
                "}\n\n"
                "void scene_end(void) {\n"
                "    EndMode2D();\n"
                "}\n\n"
                "/* The Canvas: the window. One canvas unit is ui_scale pixels: the design area just fits\n"
                "   inside the canvas. */\n"
                "Rectangle scene_ui_canvas(void) {\n"
                "    Rectangle canvas;\n"
                "    canvas.x = 0.0f;\n"
                "    canvas.y = 0.0f;\n"
                "    canvas.width = (float)GetScreenWidth();\n"
                "    canvas.height = (float)GetScreenHeight();\n"
                "    return canvas;\n"
                "}\n";
    }
    return code;
}
// The struct type for entities with `fields`: the SceneEntity fields, also reachable as one
// SceneEntity through a C11 anonymous union, then only the fields it needs.
std::string entityTypeCode(EntityFields fields) {
    const std::string name = entityTypeName(fields);
    std::string code = "\ntypedef struct " + name + " {\n"
                       "    union {\n"
                       "        SceneEntity entity;\n"
                       "        struct {\n"
                       "            SceneTransform transform;\n"
                       "            ScenePose pose;\n"
                       "        };\n"
                       "    };\n";
    if (fields.sprite) {
        code += "    SceneSprite sprite;\n";
    }
    if (fields.collider) {
        code += "    SceneCollider collider;\n";
    }
    if (fields.body) {
        code += "    b2BodyId body;\n";
    }
    if (fields.platformer) {
        code += "    ScenePlatformer platformer;\n";
    }
    if (fields.sprite) {
        code += "    bool visible; /* false: its sprite isn't drawn */\n";
    }
    return code + "} " + name + ";\n";
}

} // namespace

bool writeSceneCommon(const std::filesystem::path& outDir, const DisplaySettings& display,
                      const PhysicsSettings& physics, const std::set<EntityFields>& entityTypes,
                      std::string& error) {
    std::string entityTypesCode;
    for (const EntityFields& fields : entityTypes) {
        if (fields != EntityFields{}) {
            entityTypesCode += entityTypeCode(fields);
        }
    }
    const std::vector<std::pair<std::string, std::string>> values{
        {"marker", kGeneratedMarker},
        {"width", floatLiteral(static_cast<float>(display.width))},
        {"height", floatLiteral(static_cast<float>(display.height))},
        {"mode", scaleModeName(display.scaleMode)},
        {"mode_note", scaleModeNote(display.scaleMode)},
        {"scaling_source", scalingCode(display.scaleMode)},
        {"physics", physics.enabled ? "1" : "0"},
        {"pixels_per_meter", floatLiteral(physics.pixelsPerMeter)},
        {"gravity_x", floatLiteral(physics.gravityX)},
        {"gravity_y", floatLiteral(physics.gravityY)},
        {"entity_types", entityTypesCode},
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

/* ----- Screen scaling: the design area, fitted to any window ({{mode}}) ----- */

#define SCENE_DESIGN_WIDTH {{width}}
#define SCENE_DESIGN_HEIGHT {{height}}

/* The camera scene_begin uses: the scene origin at the window centre, and
   {{mode_note}}. */
Camera2D scene_camera(void);
/* Between these, everything is drawn in scene coordinates. */
void scene_begin(void);
void scene_end(void);

/* The Canvas UI is laid out on, in window pixels, and window pixels per canvas unit. */
Rectangle scene_ui_canvas(void);
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

typedef enum SceneColliderShape {
    SCENE_COLLIDER_BOX,
    SCENE_COLLIDER_CIRCLE,  /* radius times the larger of the entity's scales: stays round */
    SCENE_COLLIDER_TRIANGLE /* in the box `size`, its tip at the top centre */
} SceneColliderShape;

typedef struct SceneCollider {
    SceneColliderShape shape;
    Vector2 offset; /* its centre, from the entity's position, before scaling */
    Vector2 size;   /* box and triangle */
    float radius;   /* circle */
    bool is_trigger;
    float density;
    float friction;
    float restitution;
} SceneCollider;

#if SCENE_PHYSICS
/* A player-controlled platformer character (a dynamic body with a collider): moves
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

/* What every entity has. An entity with more components has a type of its own with just
   the fields it needs (below, one for each combination the project uses); their `entity`
   member is the same transform and pose as a SceneEntity, for code that takes any entity. */
typedef struct SceneEntity {
    SceneTransform transform;
    ScenePose pose; /* from the last _update_poses or _draw */
} SceneEntity;
{{entity_types}}
/* A box placed in the scene: centre, scaled size, rotation in degrees. */
typedef struct SceneBox {
    Vector2 center;
    Vector2 size;
    float rotation;
} SceneBox;

/* Where a child with local transform `t` ends up. */
ScenePose scene_child_pose(ScenePose parent, SceneTransform t);

/* A box of `size` at `offset` from an entity at `pose`, e.g. its box collider:
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
void scene_add_collider(b2BodyId body, Vector2 scale, SceneCollider collider, SceneEntity *entity);
void scene_physics_advance(b2WorldId world, float *time, float dt);
void scene_body_to_entity(b2BodyId body, SceneEntity *entity, ScenePose parent);

/* Moves a platformer character for this frame (before the physics steps): input, ground,
   coyote time, jump buffer, jump cut, acceleration. The scene's _step calls it. */
void scene_platformer_update(ScenePlatformer *p, b2BodyId body, float dt);
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

{{scaling_source}}

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

/* A point of the collider (before scaling, from the entity's position) in the body, in metres. */
static b2Vec2 collider_point(Vector2 scale, float x, float y) {
    return scene_to_meters((Vector2){x * scale.x, y * scale.y});
}

void scene_add_collider(b2BodyId body, Vector2 scale, SceneCollider collider, SceneEntity *entity) {
    const float hw = collider.size.x / 2.0f;
    const float hh = collider.size.y / 2.0f;
    b2ShapeDef shape = b2DefaultShapeDef();
    shape.density = collider.density;
    shape.material.friction = collider.friction;
    shape.material.restitution = collider.restitution;
    shape.isSensor = collider.is_trigger;
    shape.enableSensorEvents = true;
    shape.enableContactEvents = true;
    shape.userData = entity;
    switch (collider.shape) {
    case SCENE_COLLIDER_CIRCLE: {
        const float scale_max = fmaxf(fabsf(scale.x), fabsf(scale.y));
        const b2Circle circle = {collider_point(scale, collider.offset.x, collider.offset.y),
                                 collider.radius * scale_max / SCENE_PIXELS_PER_METER};
        b2CreateCircleShape(body, &shape, &circle);
        break;
    }
    case SCENE_COLLIDER_TRIANGLE: {
        const b2Vec2 points[3] = {collider_point(scale, collider.offset.x, collider.offset.y - hh),
                                  collider_point(scale, collider.offset.x - hw, collider.offset.y + hh),
                                  collider_point(scale, collider.offset.x + hw, collider.offset.y + hh)};
        const b2Hull hull = b2ComputeHull(points, 3);
        if (hull.count == 3) { /* not a flat triangle */
            const b2Polygon triangle = b2MakePolygon(&hull, 0.0f);
            b2CreatePolygonShape(body, &shape, &triangle);
        }
        break;
    }
    case SCENE_COLLIDER_BOX: {
        const b2Polygon box = b2MakeOffsetBox(hw * fabsf(scale.x) / SCENE_PIXELS_PER_METER,
                                              hh * fabsf(scale.y) / SCENE_PIXELS_PER_METER,
                                              collider_point(scale, collider.offset.x, collider.offset.y), b2Rot_identity);
        b2CreatePolygonShape(body, &shape, &box);
        break;
    }
    }
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
void scene_body_to_entity(b2BodyId body, SceneEntity *entity, ScenePose parent) {
    SceneTransform *t = &entity->transform;
    const Vector2 position = scene_to_scene_units(b2Body_GetPosition(body));
    const float rotation = b2Rot_GetAngle(b2Body_GetRotation(body)) * RAD2DEG;
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

void scene_platformer_update(ScenePlatformer *p, b2BodyId body, float dt) {
    b2Vec2 v;
    float vx, vy;
    bool want_jump;
    if (!b2Body_IsValid(body)) {
        return;
    }
    if (!p->manual_input) {
        const bool right = IsKeyDown(KEY_D) || IsKeyDown(KEY_RIGHT);
        const bool left = IsKeyDown(KEY_A) || IsKeyDown(KEY_LEFT);
        p->move = (float)right - (float)left;
        p->jump_pressed = p->jump_pressed || IsKeyPressed(KEY_SPACE) || IsKeyPressed(KEY_W) || IsKeyPressed(KEY_UP);
        p->jump_held = IsKeyDown(KEY_SPACE) || IsKeyDown(KEY_W) || IsKeyDown(KEY_UP);
    }
    v = b2Body_GetLinearVelocity(body);
    vx = v.x * SCENE_PIXELS_PER_METER;
    vy = v.y * SCENE_PIXELS_PER_METER;

    /* On the ground (and not on the way up): coyote time starts over. */
    p->grounded = vy > -1.0f && on_ground(body);
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
        const float gravity = fabsf(SCENE_GRAVITY_Y * b2Body_GetGravityScale(body)) * SCENE_PIXELS_PER_METER;
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
    b2Body_SetLinearVelocity(body, (b2Vec2){vx / SCENE_PIXELS_PER_METER, vy / SCENE_PIXELS_PER_METER});
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

    SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_VSYNC_HINT | FLAG_MSAA_4X_HINT);
    InitWindow((int)SCENE_DESIGN_WIDTH, (int)SCENE_DESIGN_HEIGHT, "{{project}}");
    SetTargetFPS(60);
    scene_manager_init(&scenes, SCENE_ID_START);

    while (!WindowShouldClose()) {
        if (IsKeyPressed(KEY_TAB)) {
            const int step = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT) ? SCENE_ID_COUNT - 1 : 1;
            scenes.current = (scenes.current + step) % SCENE_ID_COUNT;
        }
        scene_manager_update(&scenes, GetFrameTime());

        BeginDrawing();
        ClearBackground((Color){20, 20, 23, 255});
        scene_begin();
        scene_manager_draw(&scenes);
        /* Your drawing in scene coordinates, e.g. at an entity's pose.position. */
        scene_end();
        scene_manager_draw_ui(&scenes);
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
