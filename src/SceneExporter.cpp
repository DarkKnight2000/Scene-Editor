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

bool exportScene(const Scene& scene, const std::string& sceneName, const std::filesystem::path& outDir,
                 bool physics, std::string& error) {
    const std::string id = sceneIdentifier(sceneName);
    const std::string prefix = "scene_" + id;
    const std::string guard = "SCENE_" + upper(id) + "_H";
    const entt::registry& registry = scene.registry();

    // --- Header ---
    const std::string h = fill(R"GEN({{marker}} from scene "{{name}}". Do not edit:
// re-export from the editor instead.
#ifndef {{guard}}
#define {{guard}}

#include "scene_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Builds the scene's entities into `scene`, replacing whatever it held (it may be a
   zero-initialised SceneData; its on_draw hook and user_data are kept). */
void {{prefix}}_load(SceneData *scene);

/* Draws the scene, then runs its on_draw hook (if set), which may set *current to another
   SceneId to switch scenes. Call between scene_begin(mode) and scene_end(); the
   SceneManager (scene_manager.h) calls it for the current scene. */
void {{prefix}}_draw(SceneData *scene, int *current);

#ifdef __cplusplus
}
#endif

#endif /* {{guard}} */
)GEN",
                               {{"marker", kGeneratedMarker}, {"name", commentText(sceneName)}, {"guard", guard},
                                {"prefix", prefix}});

    // --- Source: build the entities through the runtime API ---
    // Scene entities in drawing order, then UI elements in tree order (parents first, so
    // they're drawn underneath their children).
    std::unordered_map<entt::entity, int> index;
    std::vector<entt::entity> entities;
    for (const entt::entity e : scene.entities()) {
        if (!scene.isRoot(e) && !scene.isUI(e)) {
            index.emplace(e, static_cast<int>(entities.size()));
            entities.push_back(e);
        }
    }
    const std::size_t uiStart = entities.size();
    for (const HierarchyRow& row : scene.hierarchy()) {
        if (!scene.isRoot(row.entity) && scene.isUI(row.entity)) {
            index.emplace(row.entity, static_cast<int>(entities.size()));
            entities.push_back(row.entity);
        }
    }

    std::ostringstream body;
    const auto& world = registry.get<TransformComponent>(scene.world());
    body << "    scene_unload(scene);\n"
         << "    scene->world = " << transformCall(world) << ";\n";
    if (uiStart > 0) {
        body << "\n    /* Entities, in drawing order. Transforms are relative to the parent. */\n";
    }
    for (std::size_t i = 0; i < entities.size(); ++i) {
        const entt::entity e = entities[i];
        const std::string ref = "e[" + std::to_string(i) + "]";
        if (i >= uiStart) {
            if (i == uiStart) {
                body << "\n    /* UI elements, laid out on the screen. Rectangles are relative to the parent. */\n";
            }
            const auto& u = registry.get<UIRectTransformComponent>(e);
            body << "    " << ref << " = scene_add_ui_element(scene, \"" << cString(registry.get<NameComponent>(e).name)
                 << "\", SCENE_CANVAS);\n"
                 << "    *scene_ui_transform(scene, " << ref << ") = scene_ui_rect_transform(" << vector2(u.anchorMin)
                 << ", " << vector2(u.anchorMax) << ", " << vector2(u.pivot) << ", " << vector2(u.position) << ", "
                 << vector2(u.size) << ");\n";
            if (const auto* rect = registry.try_get<UIRectangleComponent>(e)) {
                body << "    scene_set_ui_rectangle(scene, " << ref << ", " << color(rect->color) << ");\n";
            }
            if (const auto* label = registry.try_get<UILabelComponent>(e)) {
                body << "    scene_set_ui_label(scene, " << ref << ", \"" << cString(label->text) << "\", "
                     << floatLiteral(label->fontSize) << ", " << floatLiteral(label->spacing) << ", "
                     << alignConstant(label->alignH) << ", " << alignConstant(label->alignV) << ", "
                     << color(label->color) << ");\n";
            }
            continue;
        }
        const auto& t = registry.get<TransformComponent>(e);
        body << "    " << ref << " = scene_add_entity(scene, \"" << cString(registry.get<NameComponent>(e).name)
             << "\", SCENE_WORLD);\n"
             << "    scene_entity(scene, " << ref << ")->transform = " << transformCall(t) << ";\n";
        if (!t.inheritRotation) {
            body << "    scene_entity(scene, " << ref << ")->transform.inherit_rotation = false;\n";
        }
        if (!t.inheritScale) {
            body << "    scene_entity(scene, " << ref << ")->transform.inherit_scale = false;\n";
        }
        if (const auto* sprite = registry.try_get<SpriteComponent>(e)) {
            body << "    scene_set_sprite(scene, " << ref << ", " << vector2(sprite->size) << ", " << color(sprite->tint)
                 << ");\n";
            if (sprite->shape != SpriteShape::Rectangle) {
                body << "    scene_set_sprite_shape(scene, " << ref << ", "
                     << (sprite->shape == SpriteShape::Circle ? "SCENE_SHAPE_CIRCLE" : "SCENE_SHAPE_TRIANGLE") << ");\n";
            }
        }
        if (const auto* collider = registry.try_get<BoxColliderComponent>(e)) {
            body << "    scene_set_collider(scene, " << ref << ", " << vector2(collider->offset) << ", "
                 << vector2(collider->size) << ", " << (collider->isTrigger ? "true" : "false") << ");\n";
            const BoxColliderComponent defaults;
            if (collider->density != defaults.density || collider->friction != defaults.friction ||
                collider->restitution != defaults.restitution) {
                body << "    scene_set_collider_material(scene, " << ref << ", " << floatLiteral(collider->density) << ", "
                     << floatLiteral(collider->friction) << ", " << floatLiteral(collider->restitution) << ");\n";
            }
        }
        if (const auto* rb = registry.try_get<RigidbodyComponent>(e); rb != nullptr && physics) {
            const char* const types[] = {"SCENE_BODY_STATIC", "SCENE_BODY_KINEMATIC", "SCENE_BODY_DYNAMIC"};
            const RigidbodyComponent defaults;
            std::ostringstream fields;
            if (rb->gravityScale != defaults.gravityScale) {
                fields << "            rb->gravity_scale = " << floatLiteral(rb->gravityScale) << ";\n";
            }
            if (rb->linearDamping != defaults.linearDamping) {
                fields << "            rb->linear_damping = " << floatLiteral(rb->linearDamping) << ";\n";
            }
            if (rb->angularDamping != defaults.angularDamping) {
                fields << "            rb->angular_damping = " << floatLiteral(rb->angularDamping) << ";\n";
            }
            if (rb->fixedRotation) {
                fields << "            rb->fixed_rotation = true;\n";
            }
            if (rb->isBullet) {
                fields << "            rb->is_bullet = true;\n";
            }
            const std::string call = "scene_set_rigidbody(scene, " + ref + ", " + types[static_cast<int>(rb->type)] + ")";
            if (fields.str().empty()) {
                body << "    " << call << ";\n";
            } else {
                body << "    {\n        SceneRigidbody *rb = " << call << ";\n        if (rb != NULL) {\n"
                     << fields.str() << "        }\n    }\n";
            }
        }
    }

    // Parents last: an entity may come before its parent in drawing order.
    bool parentHeader = false;
    for (std::size_t i = 0; i < entities.size(); ++i) {
        const auto found = index.find(scene.parentOf(entities[i]));
        if (found == index.end()) {
            continue; // follows the World / the Canvas
        }
        if (!parentHeader) {
            body << "\n    /* Parents (local transforms and rectangles are kept). */\n";
            parentHeader = true;
        }
        body << "    scene_set_parent(scene, e[" << i << "], e[" << found->second << "], false);\n";
    }

    const std::string c = fill(R"GEN({{marker}} from scene "{{name}}". Do not edit:
// re-export from the editor instead.
#include "{{id}}.h"

void {{prefix}}_load(SceneData *scene) {
{{declare}}{{body}}}

void {{prefix}}_draw(SceneData *scene, int *current) {
    scene_draw(scene);
    if (scene->on_draw != NULL) {
        scene->on_draw(scene, current);
    }
}
)GEN",
                               {{"marker", kGeneratedMarker},
                                {"name", commentText(sceneName)},
                                {"id", id},
                                {"prefix", prefix},
                                {"declare", entities.empty() ? std::string()
                                                             : "    int e[" + std::to_string(entities.size()) + "];\n"},
                                {"body", body.str()}});

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

bool writeSceneManager(const std::vector<std::string>& sceneNames, const std::filesystem::path& outDir,
                       std::string& error) {
    std::ostringstream ids;
    std::ostringstream names;
    std::ostringstream loads;
    std::ostringstream draws;
    for (const std::string& name : sceneNames) {
        const std::string prefix = "scene_" + sceneIdentifier(name);
        ids << "    " << sceneIdConstant(name) << ", /* \"" << commentText(name) << "\" */\n";
        names << "    \"" << cString(name) << "\",\n";
        loads << "    " << prefix << "_load,\n";
        draws << "    " << prefix << "_draw,\n";
    }
    const std::vector<std::pair<std::string, std::string>> values{
        {"marker", kGeneratedMarker}, {"ids", ids.str()},     {"names", names.str()},
        {"loads", loads.str()},       {"draws", draws.str()}, {"first", sceneIdConstant(sceneNames.front())},
    };

    const std::string h = fill(R"GEN({{marker}}: every scene of the project in one place. Do not edit.
#ifndef SCENE_MANAGER_H
#define SCENE_MANAGER_H

#include "scene_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/* One id per exported scene: an index into SceneManager.scenes. */
typedef enum SceneId {
{{ids}}    SCENE_ID_COUNT
} SceneId;

/* Scene names as in the editor, indexed by SceneId. */
extern const char *const scene_names[SCENE_ID_COUNT];

/* Every scene's data, and which one is shown:

       static SceneManager manager;                 zero-initialised
       manager.scenes[{{first}}].on_draw = my_logic;    optional, any time
       scene_manager_init(&manager, {{first}});         after InitWindow
       ...each frame:
       scene_manager_update(&manager, GetFrameTime());
       scene_begin(mode);  scene_manager_draw(&manager);  scene_end();
       scene_manager_draw_ui(&manager, mode);
       ...
       scene_manager_deinit(&manager);

   Scenes keep their state while another one is shown. To switch scenes, a scene's
   on_draw hook sets *current to another SceneId (e.g. when the player reaches a door);
   the game can also set manager.current directly. The new scene is drawn from the next
   scene_manager_draw on. */
typedef struct SceneManager {
    SceneData scenes[SCENE_ID_COUNT];
    int current; /* SceneId of the scene shown */
    int drawn;   /* SceneId the last scene_manager_draw drew (its UI is drawn next) */
} SceneManager;

/* Loads every scene (keeping hooks already set) and shows `first`. */
void scene_manager_init(SceneManager *manager, int first);

/* The scene shown (or about to be, if `current` was just changed). */
SceneData *scene_manager_current(SceneManager *manager);

/* Rebuilds scene `id` from its exported data; its hooks are kept. */
void scene_manager_reload(SceneManager *manager, int id);

/* Advances the current scene by `dt` seconds (e.g. GetFrameTime()): its physics, when the
   export has physics. Call once per frame, before drawing. */
void scene_manager_update(SceneManager *manager, float dt);

/* Draws the current scene with its draw function, passing `current` so the scene can
   switch to another. A value that isn't a SceneId is ignored. */
void scene_manager_draw(SceneManager *manager);

/* Draws the UI of the scene scene_manager_draw just drew (even if it switched scenes), on
   top of everything: call after scene_end(), with the mode given to scene_begin(). */
void scene_manager_draw_ui(SceneManager *manager, SceneScaleMode mode);

/* Frees every scene. */
void scene_manager_deinit(SceneManager *manager);

#ifdef __cplusplus
}
#endif

#endif /* SCENE_MANAGER_H */
)GEN",
                               values);

    const std::string c = fill(R"GEN({{marker}}: every scene of the project in one place. Do not edit.
#include "scenes.h"

const char *const scene_names[SCENE_ID_COUNT] = {
{{names}}};

static void (*const scene_loads[SCENE_ID_COUNT])(SceneData *scene) = {
{{loads}}};

static void (*const scene_draws[SCENE_ID_COUNT])(SceneData *scene, int *current) = {
{{draws}}};

static int is_scene_id(int id) {
    return id >= 0 && id < SCENE_ID_COUNT;
}

void scene_manager_init(SceneManager *manager, int first) {
    int id;
    for (id = 0; id < SCENE_ID_COUNT; ++id) {
        scene_loads[id](&manager->scenes[id]);
    }
    manager->current = is_scene_id(first) ? first : 0;
    manager->drawn = manager->current;
}

SceneData *scene_manager_current(SceneManager *manager) {
    return &manager->scenes[is_scene_id(manager->current) ? manager->current : 0];
}

void scene_manager_reload(SceneManager *manager, int id) {
    if (is_scene_id(id)) {
        scene_loads[id](&manager->scenes[id]);
    }
}

void scene_manager_update(SceneManager *manager, float dt) {
#if SCENE_PHYSICS
    scene_physics_step(scene_manager_current(manager), dt);
#else
    (void)manager;
    (void)dt;
#endif
}

void scene_manager_draw(SceneManager *manager) {
    const int shown = is_scene_id(manager->current) ? manager->current : 0;
    int next = shown;
    scene_draws[shown](&manager->scenes[shown], &next);
    manager->current = is_scene_id(next) ? next : shown;
    manager->drawn = shown;
}

void scene_manager_draw_ui(SceneManager *manager, SceneScaleMode mode) {
    scene_draw_ui(&manager->scenes[is_scene_id(manager->drawn) ? manager->drawn : 0], mode);
}

void scene_manager_deinit(SceneManager *manager) {
    int id;
    for (id = 0; id < SCENE_ID_COUNT; ++id) {
        scene_unload(&manager->scenes[id]);
    }
}
)GEN",
                               values);
    const std::filesystem::path dir = outDir / kGeneratedDir;
    return writeFile(dir / "scene_manager.h", h, error) && writeFile(dir / "scene_manager.c", c, error);
}

namespace {
// One sparse-set pool per component type, generated from these templates.
// {{Pool}}: pool type, {{T}}: component type, {{pool}}: function prefix.
struct ComponentPoolSpec {
    const char* pool;      // e.g. "SceneSpritePool"
    const char* type;      // e.g. "SceneSprite"
    const char* prefix;    // e.g. "sprite_pool"
};
constexpr ComponentPoolSpec kPools[] = {
    {"SceneSpritePool", "SceneSprite", "sprite_pool"},
    {"SceneBoxColliderPool", "SceneBoxCollider", "collider_pool"},
    {"SceneUIRectTransformPool", "SceneUIRectTransform", "ui_transform_pool"},
    {"SceneUIRectanglePool", "SceneUIRectangle", "ui_rectangle_pool"},
    {"SceneUILabelPool", "SceneUILabel", "ui_label_pool"},
};

std::string poolHeader(const ComponentPoolSpec& spec) {
    return fill(R"GEN(/* {{T}} components, packed: data[0 .. count-1] belong to entity[0 .. count-1].
   sparse[id] is the index of entity `id`'s component, or -1. Removing a component moves
   the last one into its slot, so the packed order changes; loop over it for systems
   (physics, animation...), not for drawing order. */
typedef struct {{Pool}} {
    {{T}} *data;
    int *entity;
    int count;
    int capacity;
    int *sparse; /* indexed by entity id (sized like the scene's entity arrays) */
} {{Pool}};
)GEN",
                {{"Pool", spec.pool}, {"T", spec.type}});
}

std::string poolSource(const ComponentPoolSpec& spec) {
    return fill(R"GEN(/* --- {{Pool}}: sparse set of {{T}} --- */

static {{T}} *{{pool}}_get(const {{Pool}} *pool, int id) {
    return pool->sparse != NULL && id >= 0 && pool->sparse[id] >= 0 ? &pool->data[pool->sparse[id]] : NULL;
}

/* Adds a slot for entity `id` (or returns the existing one), growing the arrays in `em`;
   NULL if out of memory. */
static {{T}} *{{pool}}_emplace(EM *em, {{Pool}} *pool, int id) {
    {{T}} *existing = {{pool}}_get(pool, id);
    if (existing != NULL) {
        return existing;
    }
    if (pool->count == pool->capacity) {
        const int capacity = pool->capacity > 0 ? pool->capacity * 2 : 16;
        const size_t used = (size_t)pool->count;
        {{T}} *data = ({{T}} *)grow(em, pool->data, used * sizeof({{T}}), (size_t)capacity * sizeof({{T}}));
        int *entity;
        if (data == NULL) {
            return NULL;
        }
        pool->data = data;
        entity = (int *)grow(em, pool->entity, used * sizeof(int), (size_t)capacity * sizeof(int));
        if (entity == NULL) {
            return NULL;
        }
        pool->entity = entity;
        pool->capacity = capacity;
    }
    pool->sparse[id] = pool->count;
    pool->entity[pool->count] = id;
    return &pool->data[pool->count++];
}

/* Swap-and-pop: the last component moves into the freed slot, keeping the data packed. */
static void {{pool}}_remove({{Pool}} *pool, int id) {
    const int index = pool->sparse != NULL && id >= 0 ? pool->sparse[id] : -1;
    const int last = pool->count - 1;
    if (index < 0) {
        return;
    }
    pool->data[index] = pool->data[last];
    pool->entity[index] = pool->entity[last];
    pool->sparse[pool->entity[index]] = index;
    pool->sparse[id] = -1;
    pool->count = last;
}

/* Grows `sparse` to `capacity` entity ids in `em`, new ids having no component. */
static bool {{pool}}_reserve_ids(EM *em, {{Pool}} *pool, int old_capacity, int capacity) {
    int *sparse = (int *)grow(em, pool->sparse, (size_t)old_capacity * sizeof(int), (size_t)capacity * sizeof(int));
    int i;
    if (sparse == NULL) {
        return false;
    }
    for (i = old_capacity; i < capacity; ++i) {
        sparse[i] = -1;
    }
    pool->sparse = sparse;
    return true;
}

static void {{pool}}_free({{Pool}} *pool) {
    release(pool->data);
    release(pool->entity);
    release(pool->sparse);
}

)GEN",
                {{"Pool", spec.pool}, {"T", spec.type}, {"pool", spec.prefix}});
}
} // namespace

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
    std::string poolHeaders;
    std::string poolSources;
    for (const ComponentPoolSpec& spec : kPools) {
        poolHeaders += poolHeader(spec) + "\n";
        poolSources += poolSource(spec);
    }
    // Rigidbodies only exist with physics.
    constexpr ComponentPoolSpec kRigidbodyPool{"SceneRigidbodyPool", "SceneRigidbody", "rigidbody_pool"};
    poolHeaders += "#if SCENE_PHYSICS\n" + poolHeader(kRigidbodyPool) + "#endif\n\n";
    poolSources += "#if SCENE_PHYSICS\n" + poolSource(kRigidbodyPool) + "#endif /* SCENE_PHYSICS */\n\n";
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
        {"pool_headers", poolHeaders},
        {"pool_sources", poolSources},
        {"physics", physics.enabled ? "1" : "0"},
        {"pixels_per_meter", floatLiteral(physics.pixelsPerMeter)},
        {"gravity_x", floatLiteral(physics.gravityX)},
        {"gravity_y", floatLiteral(physics.gravityY)},
    };

    const std::string h = fill(R"GEN({{marker}}: code shared by every exported scene. Do not edit.
#ifndef SCENE_COMMON_H
#define SCENE_COMMON_H

#include <stdbool.h>
#include <stddef.h>

#include <raylib.h>

/* Box2D physics (Project Settings > Physics). Box2D's headers need C11. */
#define SCENE_PHYSICS {{physics}}
#if SCENE_PHYSICS
#include <box2d/box2d.h>
#define SCENE_PIXELS_PER_METER {{pixels_per_meter}} /* scene units in one metre (Box2D works in metres) */
#define SCENE_GRAVITY_X {{gravity_x}}        /* metres per second squared, y down */
#define SCENE_GRAVITY_Y {{gravity_y}}
#define SCENE_PHYSICS_TIME_STEP (1.0f / 60.0f) /* fixed step of scene_physics_step */
#define SCENE_PHYSICS_SUB_STEPS 4
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------------------
   Screen scaling
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

/* The scale mode chosen in Project Settings, as a suggested default. The mode itself is
   picked at runtime: pass this or any other SceneScaleMode to scene_begin(). */
#define SCENE_DEFAULT_SCALE_MODE {{mode}}

/* Camera showing the scene on the current window: scene origin at the window centre,
   zoomed so the design area fits according to `mode`. Also converts between window
   pixels and scene coordinates, e.g. GetScreenToWorld2D(GetMousePosition(), camera). */
Camera2D scene_camera(SceneScaleMode mode);

/* Wrap scene drawing in these (between BeginDrawing and EndDrawing):
       scene_begin(SCENE_DEFAULT_SCALE_MODE);  scene_demo_draw();  scene_end();
   The mode can differ per call, e.g. from an options menu. */
void scene_begin(SceneScaleMode mode);
void scene_end(void);

/* ---------------------------------------------------------------------------------------
   Runtime scene: entities and components you can read and change while the game runs
   --------------------------------------------------------------------------------------- */

/* ---------------------------------------------------------------------------------------
   Memory: every scene's entities and components live in one easy_memory region (an EM, see
   easy_memory.h), created on first use with SCENE_MEMORY_SIZE bytes. It doesn't grow: when
   it's full, adding entities or components fails (-1 / NULL). Compile with
   -DSCENE_MEMORY_SIZE=... for a different size, or give a scene its own region by setting
   SceneData.memory (e.g. em_create_nested(scene_memory(), size), or em_create_static over
   your own buffer) before loading it.
   --------------------------------------------------------------------------------------- */

#ifndef SCENE_MEMORY_SIZE
#if SCENE_PHYSICS
#define SCENE_MEMORY_SIZE (32u * 1024u * 1024u) /* Box2D's worlds live here too */
#else
#define SCENE_MEMORY_SIZE (8u * 1024u * 1024u)
#endif
#endif

struct EM;

/* The shared region (created on first use; NULL if that fails). */
struct EM *scene_memory(void);

/* Frees the shared region. Call once every scene using it is unloaded, e.g. at exit. */
void scene_memory_destroy(void);

/* --------------------------------------------------------------------------------------- */

#define SCENE_WORLD (-1)    /* parent id meaning "the World root" */
#define SCENE_CANVAS (-2)   /* parent id meaning "the Canvas", the root of the UI */
#define SCENE_NAME_MAX 64   /* entity names are truncated to SCENE_NAME_MAX - 1 bytes */

/* Position, rotation (degrees) and scale relative to the entity's parent. Positions are
   in scene coordinates: (0, 0) is the scene origin, y points down. */
typedef struct SceneTransform {
    Vector2 position;
    float rotation;
    Vector2 scale;
    bool inherit_rotation; /* false: ignore the parents' rotation (e.g. a health bar) */
    bool inherit_scale;    /* false: ignore the parents' scale */
} SceneTransform;

/* Final placement after the parents and the World transform. */
typedef struct ScenePose {
    Vector2 position;
    float rotation;
    Vector2 scale;
} ScenePose;

/* Every entity has a name, a parent and a transform; other components live in pools.
   UI elements are entities too, but placed by their UI rect transform (see below); their
   `transform` is unused. */
typedef struct SceneEntity {
    char name[SCENE_NAME_MAX];
    int parent; /* entity id, or SCENE_WORLD (UI elements: UI element id, or SCENE_CANVAS) */
    SceneTransform transform;
    bool alive; /* false once removed */
} SceneEntity;

/* Components */
/* What a sprite draws, filling its size: a rectangle, a circle (an ellipse when the size
   isn't square) or a triangle pointing up (down when flipped by a negative y scale). */
typedef enum SceneShape {
    SCENE_SHAPE_RECTANGLE,
    SCENE_SHAPE_CIRCLE,
    SCENE_SHAPE_TRIANGLE
} SceneShape;

typedef struct SceneSprite {
    Vector2 size; /* before scaling */
    Color tint;
    SceneShape shape; /* a new sprite is a rectangle */
} SceneSprite;

typedef struct SceneBoxCollider {
    Vector2 offset; /* from the entity's position, before scaling */
    Vector2 size;
    bool is_trigger;  /* with physics: a sensor that reports overlaps but doesn't collide */
    float density;    /* physics material: kg per square metre (default 1) */
    float friction;   /* default 0.6 */
    float restitution; /* bounciness 0..1 (default 0) */
} SceneBoxCollider;

/* ---------------------------------------------------------------------------------------
   Physics (Box2D): only when SCENE_PHYSICS is 1 (Project Settings > Physics)
   --------------------------------------------------------------------------------------- */

#if SCENE_PHYSICS

/* Body type: STATIC never moves, KINEMATIC moves by its velocity only, DYNAMIC is moved by
   gravity, forces and collisions. */
typedef enum SceneBodyType {
    SCENE_BODY_STATIC,
    SCENE_BODY_KINEMATIC,
    SCENE_BODY_DYNAMIC
} SceneBodyType;

/* Makes an entity a Box2D body; its box collider, if any, is the body's shape. An entity
   with a box collider but no rigidbody is a static body. Change a rigidbody's fields before
   its body exists (the first scene_physics_step after it was added), or call
   scene_physics_rebuild afterwards. */
typedef struct SceneRigidbody {
    SceneBodyType type;
    float gravity_scale;   /* 1 = normal gravity */
    float linear_damping;  /* 0 = none */
    float angular_damping;
    bool fixed_rotation;   /* never rotates (e.g. a platformer character) */
    bool is_bullet;        /* continuous collision for fast movers */
} SceneRigidbody;

/* What happened during the last scene_physics_step. */
typedef enum SceneContactKind {
    SCENE_TRIGGER_ENTER, /* `other` started overlapping trigger `entity` */
    SCENE_TRIGGER_EXIT,  /* ...and stopped */
    SCENE_CONTACT_BEGIN, /* `entity` and `other` started touching (solid colliders) */
    SCENE_CONTACT_END    /* ...and stopped */
} SceneContactKind;

typedef struct SceneContactEvent {
    SceneContactKind kind;
    int entity; /* the trigger, for trigger events */
    int other;
} SceneContactEvent;

#endif /* SCENE_PHYSICS */


/* UI: elements under the Canvas are laid out on the screen instead of placed in the scene,
   so the camera and the World transform never move them. The Canvas is the window (with
   SCENE_SCALE_FIT: the design area inside the black bars), measured in canvas units: the
   design resolution's units, with the window's extra length on one axis as extra canvas.
   So UI keeps its size relative to the design and stays pinned to its anchors on any
   window shape. Coordinates use y down and (0, 0) at the top-left.

   A UI element's rectangle, relative to its parent's (the Canvas's or another element's):
     anchor_min / anchor_max  points on the parent rectangle, as fractions (0..1). Equal on
                              an axis: pinned to that point (corner, edge or centre). Apart:
                              stretched with the parent along that axis.
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

/* A filled rectangle covering the UI element. */
typedef struct SceneUIRectangle {
    Color color;
} SceneUIRectangle;

/* Text alignment inside a rectangle, per axis: START = left / top, END = right / bottom. */
typedef enum SceneTextAlign {
    SCENE_ALIGN_START,
    SCENE_ALIGN_CENTER,
    SCENE_ALIGN_END
} SceneTextAlign;

#define SCENE_TEXT_MAX 128 /* label text is truncated to SCENE_TEXT_MAX - 1 bytes */

/* Text drawn inside the UI element's rectangle. font_size and spacing are in canvas units,
   so text scales with the rest of the UI. Change the text with scene_set_ui_label_text (or
   write into `text`, e.g. snprintf(label->text, SCENE_TEXT_MAX, "Score %d", score)). */
typedef struct SceneUILabel {
    char text[SCENE_TEXT_MAX];
    float font_size;
    float spacing; /* between characters */
    SceneTextAlign align_h;
    SceneTextAlign align_v;
    Color color;
    const Font *font; /* NULL: the scene's ui_font, or raylib's default font */
} SceneUILabel;

struct SceneData;

/* Game logic hook for a scene, run by the scene's draw function right after drawing (still
   inside scene_begin/scene_end). `current` is the SceneId being shown: set it to another
   SceneId to switch scenes. */
typedef void (*SceneDrawHook)(struct SceneData *scene, int *current);

/* UI logic hook for a scene, run by scene_draw_ui() after the scene's UI is drawn, in
   window pixels: draw anything on top of the UI here (e.g. text with your own Font, using
   scene_ui_text() and scene_draw_ui_text()). */
typedef void (*SceneDrawUIHook)(struct SceneData *scene);

{{pool_headers}}/* A scene. (Named SceneData so it doesn't clash with a game's own Scene type.)
   A zero-initialised SceneData is valid and empty. Entity ids index `entities` (and
   `poses`) and stay valid until the entity is removed or the scene is unloaded; ids of
   removed entities are never reused.

   Every entity in drawing order:
       for (i = 0; i < scene->order_count; ++i) { SceneEntity *e = &scene->entities[scene->order[i]]; }
   Every sprite, packed (any order), e.g. for a system:
       for (i = 0; i < scene->sprites.count; ++i) {
           SceneSprite *sprite = &scene->sprites.data[i];
           int id = scene->sprites.entity[i];
       } */
typedef struct SceneData {
    SceneTransform world; /* the World root: moves, rotates and zooms everything */
    SceneEntity *entities;
    int entity_count; /* ids in use: 0 .. entity_count - 1 (including removed ones) */
    int entity_capacity;
    int *order; /* ids of live entities, in drawing order */
    int order_count;

    SceneSpritePool sprites;
    SceneBoxColliderPool colliders;
    SceneUIRectTransformPool ui_transforms; /* exactly the UI elements have one */
    SceneUIRectanglePool ui_rectangles;
    SceneUILabelPool ui_labels;
#if SCENE_PHYSICS
    SceneRigidbodyPool rigidbodies;
#endif

    /* World-pose cache, indexed by entity id: poses[id] is valid after
       scene_update_poses() until a transform, parent or the World changes. */
    ScenePose *poses;
    unsigned int *pose_stamps; /* pass number that computed poses[id] */
    unsigned int pose_pass;    /* current pass; bumping it invalidates every cached pose */

    /* UI layout cache, indexed by entity id: ui_rects[id] is a UI element's rectangle in
       window pixels after scene_update_ui(), until the UI or the window changes. */
    Rectangle *ui_rects;
    unsigned int *ui_stamps; /* pass number that computed ui_rects[id] */
    unsigned int ui_pass;
    Rectangle ui_canvas; /* the Canvas in window pixels, from the last scene_update_ui() */
    float ui_scale;      /* window pixels per canvas unit, from the last scene_update_ui() */

    /* Your own additions: kept by scene_unload, so they survive reloading the scene. */
    SceneDrawHook on_draw;      /* optional; see SceneDrawHook */
    SceneDrawUIHook on_draw_ui; /* optional; see SceneDrawUIHook */
    const Font *ui_font;        /* labels without their own font use this; NULL: raylib's default */
    void *user_data;            /* anything the hooks need */
    struct EM *memory;          /* region for this scene's arrays; NULL: scene_memory() */

#if SCENE_PHYSICS
    /* Physics state, made by scene_physics_step (b2_nullWorldId / no bodies before). */
    b2WorldId physics_world;
    b2BodyId *bodies;          /* by entity id: its body, or a null id */
    ScenePose *body_poses;     /* by entity id: its placement when last synced with its body */
    float physics_time;        /* time not yet stepped */
    SceneContactEvent *contacts; /* events of the last scene_physics_step */
    int contact_count;
    int contact_capacity;
#endif
} SceneData;

/* A transform with the given values that inherits rotation and scale. */
SceneTransform scene_transform(float x, float y, float rotation, float scale_x, float scale_y);

/* Adds an entity at the parent's position, drawn after (on top of) the others. Returns
   its id, or -1 if out of memory. An invalid `parent` (or a UI element) means SCENE_WORLD. */
int scene_add_entity(SceneData *scene, const char *name, int parent);

/* Adds a UI element under `parent` (SCENE_CANVAS or a UI element; anything else means
   SCENE_CANVAS), drawn after (on top of) the others: 200 x 100, centred on its parent.
   Returns its id, or -1 if out of memory. */
int scene_add_ui_element(SceneData *scene, const char *name, int parent);

/* A UI rect transform with the given values. */
SceneUIRectTransform scene_ui_rect_transform(Vector2 anchor_min, Vector2 anchor_max, Vector2 pivot, Vector2 position,
                                             Vector2 size);

/* The UI element's rect transform, to read or change; NULL if `id` isn't a UI element. */
SceneUIRectTransform *scene_ui_transform(SceneData *scene, int id);

/* Removes an entity and its components. Its children are re-attached to its parent
   without moving. Returns false if `id` isn't a live entity. */
bool scene_remove_entity(SceneData *scene, int id);

/* The entity, to read or change its name, parent or transform; NULL if `id` isn't a live
   entity. */
SceneEntity *scene_entity(SceneData *scene, int id);

/* Id of the first live entity with this name in drawing order, or -1. */
int scene_find_entity(const SceneData *scene, const char *name);

/* Makes `parent` (an entity id or SCENE_WORLD; for UI elements a UI element or
   SCENE_CANVAS) the entity's parent. With keep_placement the entity stays where it is on
   screen (UI elements: on a canvas of the design resolution); otherwise its transform keeps
   its values and is now relative to the new parent. Returns false for invalid ids, for a
   parent in the other tree, or if it would make a loop (parenting an entity to itself or
   one of its children). */
bool scene_set_parent(SceneData *scene, int id, int parent, bool keep_placement);

/* Components: set (add or replace) returns the component, get returns it or NULL,
   remove drops it. Pointers stay valid until a component of the same type is added or
   removed. */
SceneSprite *scene_set_sprite(SceneData *scene, int id, Vector2 size, Color tint);
SceneSprite *scene_get_sprite(SceneData *scene, int id);
/* Sets the shape of the entity's sprite (nothing happens if it has none). */
void scene_set_sprite_shape(SceneData *scene, int id, SceneShape shape);
void scene_remove_sprite(SceneData *scene, int id);
SceneBoxCollider *scene_set_collider(SceneData *scene, int id, Vector2 offset, Vector2 size, bool is_trigger);
SceneBoxCollider *scene_get_collider(SceneData *scene, int id);
void scene_remove_collider(SceneData *scene, int id);
/* The collider's physics material (see SceneBoxCollider); nothing happens if it has none. */
void scene_set_collider_material(SceneData *scene, int id, float density, float friction, float restitution);
SceneUIRectangle *scene_set_ui_rectangle(SceneData *scene, int id, Color color); /* UI elements only */
SceneUIRectangle *scene_get_ui_rectangle(SceneData *scene, int id);
void scene_remove_ui_rectangle(SceneData *scene, int id);
SceneUILabel *scene_set_ui_label(SceneData *scene, int id, const char *text, float font_size, float spacing,
                                 SceneTextAlign align_h, SceneTextAlign align_v, Color color); /* UI elements only */
SceneUILabel *scene_get_ui_label(SceneData *scene, int id);
void scene_remove_ui_label(SceneData *scene, int id);
/* Replaces a label's text (copied, truncated to SCENE_TEXT_MAX - 1 bytes). */
void scene_set_ui_label_text(SceneData *scene, int id, const char *text);

/* Recomputes every entity's final placement into scene->poses, each one exactly once
   (parents before children). scene_draw() and scene_draw_colliders() call it; call it
   yourself before reading scene->poses after changing transforms. */
void scene_update_poses(SceneData *scene);

/* Where one entity ends up (SCENE_WORLD: the World's own transform), computed directly
   from its parent chain; always up to date, without touching the cache. */
ScenePose scene_entity_pose(const SceneData *scene, int id);

/* Moves the entity so its final position is `point`, in scene coordinates (e.g. from
   GetScreenToWorld2D(GetMousePosition(), scene_camera(mode))). */
void scene_place_entity(SceneData *scene, int id, Vector2 point);

/* Draws every entity's sprite in drawing order (between scene_begin and scene_end). */
void scene_draw(SceneData *scene);

/* Debug view: outlines of every box collider. */
void scene_draw_colliders(SceneData *scene, Color color);

/* Lays out every UI element for the current window and `mode` (the same mode as
   scene_begin()) into scene->ui_rects, each one once, parents first. scene_draw_ui() calls
   it; call it yourself before reading rectangles after changing the UI. */
void scene_update_ui(SceneData *scene, SceneScaleMode mode);

/* A UI element's rectangle in window pixels, from the last scene_update_ui(); an empty
   rectangle if `id` isn't a UI element. */
Rectangle scene_ui_rect(const SceneData *scene, int id);

/* The topmost UI element with a UI rectangle or label under `point` (window pixels, e.g.
   GetMousePosition()), from the last scene_update_ui(); -1 if none. For buttons, and to
   keep clicks on the UI from reaching the game. */
int scene_ui_element_at(const SceneData *scene, Vector2 point);

/* Text that scales with the UI, for drawing your own text into a UI element: the element's
   rectangle in window pixels, and `font_size` and `spacing` (canvas units) converted to
   pixels with the UI's scale. From the last scene_update_ui(). For example, in on_draw_ui:
       SceneUIText t = scene_ui_text(scene, id, 32.0f, 2.0f);
       DrawTextEx(my_font, "Hi", (Vector2){t.rect.x, t.rect.y}, t.font_size, t.spacing, WHITE);
   or aligned inside the rectangle:
       scene_draw_ui_text(my_font, "Hi", t.rect, t.font_size, t.spacing,
                          SCENE_ALIGN_CENTER, SCENE_ALIGN_CENTER, WHITE); */
typedef struct SceneUIText {
    Rectangle rect;
    float font_size;
    float spacing;
} SceneUIText;
SceneUIText scene_ui_text(const SceneData *scene, int id, float font_size, float spacing);

/* Draws `text` (lines split at '\n') inside `rect` with any font, aligned per axis. Sizes are
   in pixels, as DrawTextEx takes them. Text isn't clipped or wrapped. */
void scene_draw_ui_text(Font font, const char *text, Rectangle rect, float font_size, float spacing,
                        SceneTextAlign align_h, SceneTextAlign align_v, Color color);

/* Lays out and draws the UI on top of everything (rectangles, then labels, element by
   element), then runs the scene's on_draw_ui hook: call after scene_end(), with the mode
   given to scene_begin(). */
void scene_draw_ui(SceneData *scene, SceneScaleMode mode);

#if SCENE_PHYSICS
/* ----- Physics (Box2D) -------------------------------------------------------------------
   Box2D works in metres: SCENE_PIXELS_PER_METER scene units are one metre. These functions
   take and return scene units, and convert. Physics happens in the World's own space, so
   moving the World transform (e.g. screen shake) doesn't move bodies. */

/* Rigidbody component: set (add, or change the type) returns it, get returns it or NULL. */
SceneRigidbody *scene_set_rigidbody(SceneData *scene, int id, SceneBodyType type);
SceneRigidbody *scene_get_rigidbody(SceneData *scene, int id);
void scene_remove_rigidbody(SceneData *scene, int id);

/* Advances the physics by `dt` seconds (e.g. GetFrameTime()) in fixed steps of
   SCENE_PHYSICS_TIME_STEP. It creates the Box2D world and the bodies it needs, pushes
   entities the game moved into their bodies, steps, writes moved bodies back into the
   transforms, and records scene->contacts. The SceneManager calls it for the current scene
   in scene_manager_update. */
void scene_physics_step(SceneData *scene, float dt);

/* Re-creates the entity's body from its rigidbody and box collider on the next step, after
   you changed their fields (scene_set_rigidbody and scene_set_collider do it themselves). */
void scene_physics_rebuild(SceneData *scene, int id);

/* The entity's Box2D body, for the full Box2D API (in metres); b2_nullBodyId if it has
   none yet (bodies are created by scene_physics_step). */
b2BodyId scene_physics_body(const SceneData *scene, int id);

/* Velocity in scene units per second; impulses in kg * scene units per second; forces in
   kg * scene units per second squared. They do nothing for entities without a body. */
Vector2 scene_get_velocity(const SceneData *scene, int id);
void scene_set_velocity(SceneData *scene, int id, Vector2 velocity);
void scene_apply_impulse(SceneData *scene, int id, Vector2 impulse);
void scene_apply_force(SceneData *scene, int id, Vector2 force);
#endif /* SCENE_PHYSICS */

/* Returns the scene's memory to its region; the scene is empty (and reusable) afterwards,
   keeping on_draw, on_draw_ui, ui_font, user_data and memory. */
void scene_unload(SceneData *scene);

#ifdef __cplusplus
}
#endif

#endif /* SCENE_COMMON_H */
)GEN",
                               values);

    const std::string c = fill(R"GEN({{marker}}: code shared by every exported scene. Do not edit.
#include "scene_common.h"

#include <math.h>
#include <stdint.h>
#include <string.h>

#include "easy_memory.h"

#define SCENE_MAX_DEPTH 256 /* guards against parent loops in hand-edited data */

/* ----- Screen scaling ----- */

const SceneScaleMode scene_scale_modes[SCENE_SCALE_MODE_COUNT] = {{{mode_list}}};

{{scaling_source}}
/* ----- Memory ----- */

/* The region every scene allocates from unless it has its own (SceneData.memory). */
static EM *shared_memory = NULL;

struct EM *scene_memory(void) {
    if (shared_memory == NULL) {
        shared_memory = em_create(SCENE_MEMORY_SIZE);
    }
    return shared_memory;
}

void scene_memory_destroy(void) {
    if (shared_memory != NULL) {
        em_destroy(shared_memory);
        shared_memory = NULL;
    }
}

static EM *memory_of(const SceneData *scene) {
    return scene->memory != NULL ? scene->memory : scene_memory();
}

/* easy_memory has no realloc: a bigger block, the old contents copied over, the old block
   freed. NULL (and `old` untouched) if the region is full. */
static void *grow(EM *em, void *old, size_t old_size, size_t new_size) {
    void *block = em != NULL ? em_alloc(em, new_size) : NULL;
    if (block == NULL) {
        return NULL;
    }
    if (old != NULL) {
        memcpy(block, old, old_size);
        em_free(old);
    }
    return block;
}

static void release(void *block) {
    if (block != NULL) {
        em_free(block); /* finds its own region */
    }
}

#if SCENE_PHYSICS
static void physics_forget(SceneData *scene, int id); /* destroys the entity's body, if any */
#else
static void physics_forget(SceneData *scene, int id) {
    (void)scene;
    (void)id;
}
#endif

/* ----- Component pools ----- */

{{pool_sources}}/* ----- Transform maths ----- */

static bool is_live(const SceneData *scene, int id) {
    return id >= 0 && id < scene->entity_count && scene->entities[id].alive;
}

static bool is_ui(const SceneData *scene, int id) {
    return is_live(scene, id) && ui_transform_pool_get(&scene->ui_transforms, id) != NULL;
}

static Vector2 rotate(Vector2 v, float degrees) {
    const float rad = degrees * (PI / 180.0f);
    const float c = cosf(rad);
    const float s = sinf(rad);
    Vector2 r;
    r.x = v.x * c - v.y * s;
    r.y = v.x * s + v.y * c;
    return r;
}

static ScenePose pose_of(SceneTransform t) {
    ScenePose p;
    p.position = t.position;
    p.rotation = t.rotation;
    p.scale = t.scale;
    return p;
}

/* Child pose = parent pose * local transform (translate, rotate, scale), honouring the
   inherit flags. Same rule as the editor. */
static ScenePose compose(ScenePose parent, SceneTransform t) {
    const float rotation = t.inherit_rotation ? parent.rotation : 0.0f;
    Vector2 scale;
    Vector2 offset;
    ScenePose p;
    scale.x = t.inherit_scale ? parent.scale.x : 1.0f;
    scale.y = t.inherit_scale ? parent.scale.y : 1.0f;
    offset.x = t.position.x * scale.x;
    offset.y = t.position.y * scale.y;
    offset = rotate(offset, rotation);
    p.position.x = parent.position.x + offset.x;
    p.position.y = parent.position.y + offset.y;
    p.rotation = rotation + t.rotation;
    p.scale.x = scale.x * t.scale.x;
    p.scale.y = scale.y * t.scale.y;
    return p;
}

static float divide_or(float value, float divisor, float fallback) {
    return divisor != 0.0f ? value / divisor : fallback;
}

/* ----- UI layout ----- */

/* Child rectangle = the anchors on the parent, plus size, with the pivot at position.
   `scale` converts canvas units to the parent's units (1 for canvas units, ui_scale for
   window pixels). */
static Rectangle ui_layout(Rectangle parent, const SceneUIRectTransform *t, float scale) {
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

/* Solves ui_layout(parent, t, 1) == target for t's position and size (canvas units). */
static void ui_place_at(SceneUIRectTransform *t, Rectangle parent, Rectangle target) {
    const float min_x = parent.x + t->anchor_min.x * parent.width;
    const float min_y = parent.y + t->anchor_min.y * parent.height;
    const float span_x = (t->anchor_max.x - t->anchor_min.x) * parent.width;
    const float span_y = (t->anchor_max.y - t->anchor_min.y) * parent.height;
    t->size.x = target.width - span_x;
    t->size.y = target.height - span_y;
    t->position.x = target.x + t->pivot.x * target.width - (min_x + span_x * t->pivot.x);
    t->position.y = target.y + t->pivot.y * target.height - (min_y + span_y * t->pivot.y);
}

/* A UI element's rectangle on a canvas of the design resolution, in canvas units, computed
   from its parent chain (SCENE_CANVAS: the whole canvas). */
static Rectangle ui_rect_on_design(const SceneData *scene, int id) {
    int chain[SCENE_MAX_DEPTH];
    int count = 0;
    int current;
    Rectangle rect;
    rect.x = 0.0f;
    rect.y = 0.0f;
    rect.width = SCENE_DESIGN_WIDTH;
    rect.height = SCENE_DESIGN_HEIGHT;
    for (current = id; is_ui(scene, current) && count < SCENE_MAX_DEPTH; current = scene->entities[current].parent) {
        chain[count++] = current;
    }
    while (count > 0) {
        rect = ui_layout(rect, ui_transform_pool_get(&scene->ui_transforms, chain[--count]), 1.0f);
    }
    return rect;
}

/* ----- Entities ----- */

SceneTransform scene_transform(float x, float y, float rotation, float scale_x, float scale_y) {
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

/* Grows every per-entity array (entities, order, the pose and UI caches and each pool's
   sparse index) in the scene's memory. */
static bool reserve_entity_ids(SceneData *scene, int capacity) {
    EM *const em = memory_of(scene);
    const int old_capacity = scene->entity_capacity;
    if (old_capacity == 0 && scene->world.scale.x == 0.0f && scene->world.scale.y == 0.0f) {
        /* A zero-initialised scene: its World starts as the identity, not scale 0. */
        scene->world = scene_transform(0.0f, 0.0f, 0.0f, 1.0f, 1.0f);
    }
    const size_t old_n = (size_t)old_capacity;
    const size_t new_n = (size_t)capacity;
    SceneEntity *entities = (SceneEntity *)grow(em, scene->entities, old_n * sizeof(SceneEntity), new_n * sizeof(SceneEntity));
    int *order;
    ScenePose *poses;
    Rectangle *rects;
    unsigned int *stamps;
    if (entities == NULL) {
        return false;
    }
    scene->entities = entities;
    order = (int *)grow(em, scene->order, old_n * sizeof(int), new_n * sizeof(int));
    if (order == NULL) {
        return false;
    }
    scene->order = order;
    poses = (ScenePose *)grow(em, scene->poses, old_n * sizeof(ScenePose), new_n * sizeof(ScenePose));
    if (poses == NULL) {
        return false;
    }
    scene->poses = poses;
    stamps = (unsigned int *)grow(em, scene->pose_stamps, old_n * sizeof(unsigned int), new_n * sizeof(unsigned int));
    if (stamps == NULL) {
        return false;
    }
    memset(stamps + old_capacity, 0, (new_n - old_n) * sizeof(unsigned int));
    scene->pose_stamps = stamps;
    rects = (Rectangle *)grow(em, scene->ui_rects, old_n * sizeof(Rectangle), new_n * sizeof(Rectangle));
    if (rects == NULL) {
        return false;
    }
    scene->ui_rects = rects;
    stamps = (unsigned int *)grow(em, scene->ui_stamps, old_n * sizeof(unsigned int), new_n * sizeof(unsigned int));
    if (stamps == NULL) {
        return false;
    }
    memset(stamps + old_capacity, 0, (new_n - old_n) * sizeof(unsigned int));
    scene->ui_stamps = stamps;
    if (!sprite_pool_reserve_ids(em, &scene->sprites, old_capacity, capacity) ||
        !collider_pool_reserve_ids(em, &scene->colliders, old_capacity, capacity) ||
        !ui_transform_pool_reserve_ids(em, &scene->ui_transforms, old_capacity, capacity) ||
        !ui_rectangle_pool_reserve_ids(em, &scene->ui_rectangles, old_capacity, capacity) ||
        !ui_label_pool_reserve_ids(em, &scene->ui_labels, old_capacity, capacity)) {
        return false;
    }
#if SCENE_PHYSICS
    {
        b2BodyId *bodies = (b2BodyId *)grow(em, scene->bodies, old_n * sizeof(b2BodyId), new_n * sizeof(b2BodyId));
        ScenePose *body_poses;
        if (bodies == NULL) {
            return false;
        }
        memset(bodies + old_capacity, 0, (new_n - old_n) * sizeof(b2BodyId)); /* zero = no body */
        scene->bodies = bodies;
        body_poses = (ScenePose *)grow(em, scene->body_poses, old_n * sizeof(ScenePose), new_n * sizeof(ScenePose));
        if (body_poses == NULL) {
            return false;
        }
        scene->body_poses = body_poses;
        if (!rigidbody_pool_reserve_ids(em, &scene->rigidbodies, old_capacity, capacity)) {
            return false;
        }
    }
#endif
    scene->entity_capacity = capacity;
    return true;
}

/* A new entity at the end of the arrays and of the drawing order; -1 if out of memory. */
static int add_entity(SceneData *scene, const char *name, int parent) {
    SceneEntity *e;
    const int id = scene->entity_count;
    if (id == scene->entity_capacity &&
        !reserve_entity_ids(scene, scene->entity_capacity > 0 ? scene->entity_capacity * 2 : 16)) {
        return -1;
    }
    e = &scene->entities[id];
    memset(e, 0, sizeof(*e));
    if (name != NULL) {
        /* Copy and truncate by hand (strncpy is flagged as deprecated by some C runtimes). */
        size_t n = 0;
        while (n < SCENE_NAME_MAX - 1 && name[n] != '\0') {
            e->name[n] = name[n];
            ++n;
        }
    }
    e->parent = parent;
    e->transform = scene_transform(0.0f, 0.0f, 0.0f, 1.0f, 1.0f);
    e->alive = true;
    scene->pose_stamps[id] = 0; /* nothing cached yet */
    scene->ui_stamps[id] = 0;
    scene->order[scene->order_count++] = id;
    return scene->entity_count++;
}

int scene_add_entity(SceneData *scene, const char *name, int parent) {
    return add_entity(scene, name, is_live(scene, parent) && !is_ui(scene, parent) ? parent : SCENE_WORLD);
}

SceneUIRectTransform scene_ui_rect_transform(Vector2 anchor_min, Vector2 anchor_max, Vector2 pivot, Vector2 position,
                                             Vector2 size) {
    SceneUIRectTransform t;
    t.anchor_min = anchor_min;
    t.anchor_max = anchor_max;
    t.pivot = pivot;
    t.position = position;
    t.size = size;
    return t;
}

int scene_add_ui_element(SceneData *scene, const char *name, int parent) {
    const Vector2 centre = {0.5f, 0.5f};
    const Vector2 zero = {0.0f, 0.0f};
    const Vector2 size = {200.0f, 100.0f};
    SceneUIRectTransform *t;
    const int id = add_entity(scene, name, is_ui(scene, parent) ? parent : SCENE_CANVAS);
    if (id < 0) {
        return -1;
    }
    t = ui_transform_pool_emplace(memory_of(scene), &scene->ui_transforms, id);
    if (t == NULL) {
        scene_remove_entity(scene, id);
        return -1;
    }
    *t = scene_ui_rect_transform(centre, centre, centre, zero, size);
    return id;
}

SceneUIRectTransform *scene_ui_transform(SceneData *scene, int id) {
    return is_live(scene, id) ? ui_transform_pool_get(&scene->ui_transforms, id) : NULL;
}

bool scene_remove_entity(SceneData *scene, int id) {
    int i;
    if (!is_live(scene, id)) {
        return false;
    }
    /* Children move up to this entity's parent, staying where they are on screen. */
    for (i = 0; i < scene->entity_count; ++i) {
        if (scene->entities[i].alive && scene->entities[i].parent == id) {
            scene_set_parent(scene, i, scene->entities[id].parent, true);
        }
    }
    physics_forget(scene, id);
    sprite_pool_remove(&scene->sprites, id);
    collider_pool_remove(&scene->colliders, id);
    ui_transform_pool_remove(&scene->ui_transforms, id);
    ui_rectangle_pool_remove(&scene->ui_rectangles, id);
    ui_label_pool_remove(&scene->ui_labels, id);
#if SCENE_PHYSICS
    rigidbody_pool_remove(&scene->rigidbodies, id);
#endif
    scene->entities[id].alive = false;
    for (i = 0; i < scene->order_count; ++i) {
        if (scene->order[i] == id) {
            memmove(&scene->order[i], &scene->order[i + 1], (size_t)(scene->order_count - i - 1) * sizeof(int));
            --scene->order_count;
            break;
        }
    }
    return true;
}

SceneEntity *scene_entity(SceneData *scene, int id) {
    return is_live(scene, id) ? &scene->entities[id] : NULL;
}

int scene_find_entity(const SceneData *scene, const char *name) {
    int i;
    for (i = 0; i < scene->order_count; ++i) {
        if (strncmp(scene->entities[scene->order[i]].name, name, SCENE_NAME_MAX - 1) == 0) {
            return scene->order[i];
        }
    }
    return -1;
}

bool scene_set_parent(SceneData *scene, int id, int parent, bool keep_placement) {
    int ancestor;
    int depth;
    const bool ui = is_ui(scene, id);
    if (!is_live(scene, id)) {
        return false;
    }
    /* The parent must be in the same tree. */
    if (ui ? parent != SCENE_CANVAS && !is_ui(scene, parent)
           : parent != SCENE_WORLD && (!is_live(scene, parent) || is_ui(scene, parent))) {
        return false;
    }
    /* Refuse loops: walk up from the new parent. */
    for (ancestor = parent, depth = 0; is_live(scene, ancestor) && depth < SCENE_MAX_DEPTH;
         ancestor = scene->entities[ancestor].parent, ++depth) {
        if (ancestor == id) {
            return false;
        }
    }
    if (ui) {
        if (keep_placement) {
            /* Keep the rectangle it has on a canvas of the design resolution. */
            const Rectangle target = ui_rect_on_design(scene, id);
            ui_place_at(ui_transform_pool_get(&scene->ui_transforms, id), ui_rect_on_design(scene, parent), target);
        }
        scene->entities[id].parent = parent;
        return true;
    }
    if (keep_placement) {
        const ScenePose target = scene_entity_pose(scene, id);
        const ScenePose base = scene_entity_pose(scene, parent);
        SceneTransform *t = &scene->entities[id].transform;
        const float rotation = t->inherit_rotation ? base.rotation : 0.0f;
        const float scale_x = t->inherit_scale ? base.scale.x : 1.0f;
        const float scale_y = t->inherit_scale ? base.scale.y : 1.0f;
        Vector2 offset;
        offset.x = target.position.x - base.position.x;
        offset.y = target.position.y - base.position.y;
        offset = rotate(offset, -rotation);
        t->position.x = divide_or(offset.x, scale_x, 0.0f);
        t->position.y = divide_or(offset.y, scale_y, 0.0f);
        t->rotation = target.rotation - rotation;
        t->scale.x = divide_or(target.scale.x, scale_x, t->scale.x);
        t->scale.y = divide_or(target.scale.y, scale_y, t->scale.y);
    }
    scene->entities[id].parent = parent;
    return true;
}

/* ----- Components ----- */

SceneSprite *scene_set_sprite(SceneData *scene, int id, Vector2 size, Color tint) {
    const bool existed = is_live(scene, id) && sprite_pool_get(&scene->sprites, id) != NULL;
    SceneSprite *sprite = is_live(scene, id) ? sprite_pool_emplace(memory_of(scene), &scene->sprites, id) : NULL;
    if (sprite != NULL) {
        sprite->size = size;
        sprite->tint = tint;
        if (!existed) {
            sprite->shape = SCENE_SHAPE_RECTANGLE; /* replacing keeps the shape */
        }
    }
    return sprite;
}

void scene_set_sprite_shape(SceneData *scene, int id, SceneShape shape) {
    SceneSprite *sprite = scene_get_sprite(scene, id);
    if (sprite != NULL) {
        sprite->shape = shape;
    }
}

SceneSprite *scene_get_sprite(SceneData *scene, int id) {
    return is_live(scene, id) ? sprite_pool_get(&scene->sprites, id) : NULL;
}

void scene_remove_sprite(SceneData *scene, int id) {
    sprite_pool_remove(&scene->sprites, id);
}

SceneBoxCollider *scene_set_collider(SceneData *scene, int id, Vector2 offset, Vector2 size, bool is_trigger) {
    const bool existed = is_live(scene, id) && collider_pool_get(&scene->colliders, id) != NULL;
    SceneBoxCollider *collider = is_live(scene, id) ? collider_pool_emplace(memory_of(scene), &scene->colliders, id) : NULL;
    if (collider != NULL) {
        collider->offset = offset;
        collider->size = size;
        collider->is_trigger = is_trigger;
        if (!existed) {
            collider->density = 1.0f;
            collider->friction = 0.6f;
            collider->restitution = 0.0f;
        }
        physics_forget(scene, id); /* re-created with the new shape on the next physics step */
    }
    return collider;
}

void scene_set_collider_material(SceneData *scene, int id, float density, float friction, float restitution) {
    SceneBoxCollider *collider = scene_get_collider(scene, id);
    if (collider != NULL) {
        collider->density = density;
        collider->friction = friction;
        collider->restitution = restitution;
        physics_forget(scene, id);
    }
}

SceneBoxCollider *scene_get_collider(SceneData *scene, int id) {
    return is_live(scene, id) ? collider_pool_get(&scene->colliders, id) : NULL;
}

void scene_remove_collider(SceneData *scene, int id) {
    collider_pool_remove(&scene->colliders, id);
    physics_forget(scene, id);
}

SceneUIRectangle *scene_set_ui_rectangle(SceneData *scene, int id, Color color) {
    SceneUIRectangle *rect = is_ui(scene, id) ? ui_rectangle_pool_emplace(memory_of(scene), &scene->ui_rectangles, id) : NULL;
    if (rect != NULL) {
        rect->color = color;
    }
    return rect;
}

SceneUIRectangle *scene_get_ui_rectangle(SceneData *scene, int id) {
    return is_live(scene, id) ? ui_rectangle_pool_get(&scene->ui_rectangles, id) : NULL;
}

void scene_remove_ui_rectangle(SceneData *scene, int id) {
    ui_rectangle_pool_remove(&scene->ui_rectangles, id);
}

/* Copies `text` into `out` (SCENE_TEXT_MAX bytes), truncating. */
static void copy_text(char *out, const char *text) {
    size_t n = 0;
    while (text != NULL && n < SCENE_TEXT_MAX - 1 && text[n] != '\0') {
        out[n] = text[n];
        ++n;
    }
    out[n] = '\0';
}

SceneUILabel *scene_set_ui_label(SceneData *scene, int id, const char *text, float font_size, float spacing,
                                 SceneTextAlign align_h, SceneTextAlign align_v, Color color) {
    SceneUILabel *label = is_ui(scene, id) ? ui_label_pool_emplace(memory_of(scene), &scene->ui_labels, id) : NULL;
    if (label != NULL) {
        copy_text(label->text, text);
        label->font_size = font_size;
        label->spacing = spacing;
        label->align_h = align_h;
        label->align_v = align_v;
        label->color = color;
        label->font = NULL;
    }
    return label;
}

SceneUILabel *scene_get_ui_label(SceneData *scene, int id) {
    return is_live(scene, id) ? ui_label_pool_get(&scene->ui_labels, id) : NULL;
}

void scene_remove_ui_label(SceneData *scene, int id) {
    ui_label_pool_remove(&scene->ui_labels, id);
}

void scene_set_ui_label_text(SceneData *scene, int id, const char *text) {
    SceneUILabel *label = scene_get_ui_label(scene, id);
    if (label != NULL) {
        copy_text(label->text, text);
    }
}

/* ----- Poses ----- */

void scene_update_poses(SceneData *scene) {
    const ScenePose world = pose_of(scene->world);
    int chain[SCENE_MAX_DEPTH];
    int i;

    /* A new pass number makes every cached pose stale at once, without clearing arrays. */
    if (++scene->pose_pass == 0) {
        /* Wrapped around after ~4 billion passes: reset the stamps so 1 is unused again. */
        memset(scene->pose_stamps, 0, (size_t)scene->entity_capacity * sizeof(unsigned int));
        scene->pose_pass = 1;
    }

    for (i = 0; i < scene->order_count; ++i) {
        int id = scene->order[i];
        int count = 0;
        ScenePose pose;

        /* Walk up from the entity, collecting every ancestor not yet done in this pass. We
           stop at the World, or at an ancestor that an earlier entity already computed. */
        while (is_live(scene, id) && scene->pose_stamps[id] != scene->pose_pass && count < SCENE_MAX_DEPTH) {
            chain[count++] = id;
            id = scene->entities[id].parent;
        }

        /* Start from that ancestor's cached pose, or from the World. */
        pose = is_live(scene, id) && scene->pose_stamps[id] == scene->pose_pass ? scene->poses[id] : world;

        /* Walk back down (parents before children), computing and caching each pose once. */
        while (count > 0) {
            const int child = chain[--count];
            pose = compose(pose, scene->entities[child].transform);
            scene->poses[child] = pose;
            scene->pose_stamps[child] = scene->pose_pass;
        }
    }
}

ScenePose scene_entity_pose(const SceneData *scene, int id) {
    int chain[SCENE_MAX_DEPTH];
    int count = 0;
    int current;
    ScenePose pose = pose_of(scene->world);
    for (current = id; is_live(scene, current) && count < SCENE_MAX_DEPTH; current = scene->entities[current].parent) {
        chain[count++] = current;
    }
    while (count > 0) {
        pose = compose(pose, scene->entities[chain[--count]].transform);
    }
    return pose;
}

void scene_place_entity(SceneData *scene, int id, Vector2 point) {
    SceneEntity *e = scene_entity(scene, id);
    ScenePose base;
    float rotation;
    Vector2 offset;
    if (e == NULL) {
        return;
    }
    base = scene_entity_pose(scene, e->parent);
    rotation = e->transform.inherit_rotation ? base.rotation : 0.0f;
    offset.x = point.x - base.position.x;
    offset.y = point.y - base.position.y;
    offset = rotate(offset, -rotation);
    e->transform.position.x = divide_or(offset.x, e->transform.inherit_scale ? base.scale.x : 1.0f, 0.0f);
    e->transform.position.y = divide_or(offset.y, e->transform.inherit_scale ? base.scale.y : 1.0f, 0.0f);
}

/* ----- Drawing ----- */

#define SCENE_CIRCLE_SEGMENTS 36

/* A point in sprite units (before scaling, relative to the centre) placed in the scene. */
static Vector2 sprite_to_scene(ScenePose pose, float x, float y) {
    Vector2 p;
    p.x = x * pose.scale.x;
    p.y = y * pose.scale.y;
    p = rotate(p, pose.rotation);
    p.x += pose.position.x;
    p.y += pose.position.y;
    return p;
}

/* raylib only draws triangles whose corners run counter-clockwise on screen. */
static void draw_triangle_any_order(Vector2 a, Vector2 b, Vector2 c, Color color) {
    if ((b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x) > 0.0f) {
        const Vector2 t = b;
        b = c;
        c = t;
    }
    DrawTriangle(a, b, c, color);
}

static void draw_sprite(const SceneSprite *sprite, ScenePose pose) {
    const float hw = sprite->size.x / 2.0f;
    const float hh = sprite->size.y / 2.0f;
    if (sprite->shape == SCENE_SHAPE_CIRCLE) {
        Vector2 previous = sprite_to_scene(pose, hw, 0.0f);
        int i;
        for (i = 1; i <= SCENE_CIRCLE_SEGMENTS; ++i) {
            const float angle = 2.0f * PI * (float)i / SCENE_CIRCLE_SEGMENTS;
            const Vector2 next = sprite_to_scene(pose, hw * cosf(angle), hh * sinf(angle));
            draw_triangle_any_order(pose.position, previous, next, sprite->tint);
            previous = next;
        }
    } else if (sprite->shape == SCENE_SHAPE_TRIANGLE) {
        draw_triangle_any_order(sprite_to_scene(pose, 0.0f, -hh), sprite_to_scene(pose, -hw, hh),
                                sprite_to_scene(pose, hw, hh), sprite->tint);
    } else {
        const float w = sprite->size.x * fabsf(pose.scale.x);
        const float h = sprite->size.y * fabsf(pose.scale.y);
        DrawRectanglePro((Rectangle){pose.position.x, pose.position.y, w, h}, (Vector2){w / 2.0f, h / 2.0f},
                         pose.rotation, sprite->tint);
    }
}

void scene_draw(SceneData *scene) {
    int i;
    scene_update_poses(scene);
    /* Drawing order matters, so walk `order` and look each sprite up (the packed sprite
       array is in no particular order). */
    for (i = 0; i < scene->order_count; ++i) {
        const int id = scene->order[i];
        const SceneSprite *sprite = sprite_pool_get(&scene->sprites, id);
        if (sprite != NULL) {
            draw_sprite(sprite, scene->poses[id]);
        }
    }
}

void scene_draw_colliders(SceneData *scene, Color color) {
    int i;
    int k;
    scene_update_poses(scene);
    /* Order doesn't matter for outlines: loop over the packed collider array directly. */
    for (i = 0; i < scene->colliders.count; ++i) {
        const SceneBoxCollider *collider = &scene->colliders.data[i];
        const ScenePose pose = scene->poses[scene->colliders.entity[i]];
        const float hw = collider->size.x * fabsf(pose.scale.x) / 2.0f;
        const float hh = collider->size.y * fabsf(pose.scale.y) / 2.0f;
        const Vector2 local[4] = {{-hw, -hh}, {hw, -hh}, {hw, hh}, {-hw, hh}};
        Vector2 centre = {collider->offset.x * pose.scale.x, collider->offset.y * pose.scale.y};
        Vector2 corners[4];
        centre = rotate(centre, pose.rotation);
        for (k = 0; k < 4; ++k) {
            corners[k] = rotate(local[k], pose.rotation);
            corners[k].x += pose.position.x + centre.x;
            corners[k].y += pose.position.y + centre.y;
        }
        for (k = 0; k < 4; ++k) {
            DrawLineV(corners[k], corners[(k + 1) % 4], color);
        }
    }
}

/* ----- UI ----- */

void scene_update_ui(SceneData *scene, SceneScaleMode mode) {
    int chain[SCENE_MAX_DEPTH];
    int i;
{{ui_canvas}}    {
        const float scale_x = scene->ui_canvas.width / SCENE_DESIGN_WIDTH;
        const float scale_y = scene->ui_canvas.height / SCENE_DESIGN_HEIGHT;
        scene->ui_scale = scale_x < scale_y ? scale_x : scale_y;
    }

    /* Same caching as scene_update_poses: a new pass number invalidates every rectangle. */
    if (++scene->ui_pass == 0) {
        memset(scene->ui_stamps, 0, (size_t)scene->entity_capacity * sizeof(unsigned int));
        scene->ui_pass = 1;
    }
    for (i = 0; i < scene->order_count; ++i) {
        int id = scene->order[i];
        int count = 0;
        Rectangle rect;
        /* Walk up to the Canvas or to an element already laid out in this pass... */
        while (is_ui(scene, id) && scene->ui_stamps[id] != scene->ui_pass && count < SCENE_MAX_DEPTH) {
            chain[count++] = id;
            id = scene->entities[id].parent;
        }
        rect = is_ui(scene, id) && scene->ui_stamps[id] == scene->ui_pass ? scene->ui_rects[id] : scene->ui_canvas;
        /* ...then lay out back down, parents before children. */
        while (count > 0) {
            const int child = chain[--count];
            rect = ui_layout(rect, ui_transform_pool_get(&scene->ui_transforms, child), scene->ui_scale);
            scene->ui_rects[child] = rect;
            scene->ui_stamps[child] = scene->ui_pass;
        }
    }
}

Rectangle scene_ui_rect(const SceneData *scene, int id) {
    const Rectangle none = {0.0f, 0.0f, 0.0f, 0.0f};
    return is_ui(scene, id) && scene->ui_stamps[id] == scene->ui_pass ? scene->ui_rects[id] : none;
}

int scene_ui_element_at(const SceneData *scene, Vector2 point) {
    int i;
    for (i = scene->order_count - 1; i >= 0; --i) { /* last drawn = on top */
        const int id = scene->order[i];
        const bool visible = ui_rectangle_pool_get(&scene->ui_rectangles, id) != NULL ||
                             ui_label_pool_get(&scene->ui_labels, id) != NULL;
        if (visible && scene->ui_stamps[id] == scene->ui_pass && CheckCollisionPointRec(point, scene->ui_rects[id])) {
            return id;
        }
    }
    return -1;
}

SceneUIText scene_ui_text(const SceneData *scene, int id, float font_size, float spacing) {
    SceneUIText t;
    t.rect = scene_ui_rect(scene, id);
    t.font_size = font_size * scene->ui_scale;
    t.spacing = spacing * scene->ui_scale;
    return t;
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

void scene_draw_ui(SceneData *scene, SceneScaleMode mode) {
    int i;
    scene_update_ui(scene, mode);
    for (i = 0; i < scene->order_count; ++i) {
        const int id = scene->order[i];
        const SceneUIRectangle *rect = ui_rectangle_pool_get(&scene->ui_rectangles, id);
        const SceneUILabel *label = ui_label_pool_get(&scene->ui_labels, id);
        if (rect != NULL) {
            DrawRectangleRec(scene->ui_rects[id], rect->color);
        }
        if (label != NULL) {
            const Font font = label->font != NULL ? *label->font
                              : scene->ui_font != NULL ? *scene->ui_font
                                                       : GetFontDefault();
            scene_draw_ui_text(font, label->text, scene->ui_rects[id], label->font_size * scene->ui_scale,
                               label->spacing * scene->ui_scale, label->align_h, label->align_v, label->color);
        }
    }
    if (scene->on_draw_ui != NULL) {
        scene->on_draw_ui(scene);
    }
}


#if SCENE_PHYSICS
/* ----- Physics (Box2D) ----- */

/* Box2D allocates from the shared memory region too. */
static void *physics_alloc(unsigned int size, int alignment) {
    EM *em = scene_memory();
    return em != NULL ? em_alloc_aligned(em, size, (size_t)alignment) : NULL;
}

static void physics_free(void *block) {
    release(block);
}

static b2Vec2 to_meters(Vector2 v) {
    b2Vec2 m;
    m.x = v.x / SCENE_PIXELS_PER_METER;
    m.y = v.y / SCENE_PIXELS_PER_METER;
    return m;
}

static Vector2 to_scene_units(b2Vec2 m) {
    Vector2 v;
    v.x = m.x * SCENE_PIXELS_PER_METER;
    v.y = m.y * SCENE_PIXELS_PER_METER;
    return v;
}

/* Where an entity is in the World's own space (its pose without the World transform). */
static ScenePose physics_pose(const SceneData *scene, int id) {
    int chain[SCENE_MAX_DEPTH];
    int count = 0;
    int current;
    ScenePose pose;
    pose.position.x = 0.0f;
    pose.position.y = 0.0f;
    pose.rotation = 0.0f;
    pose.scale.x = 1.0f;
    pose.scale.y = 1.0f;
    for (current = id; is_live(scene, current) && count < SCENE_MAX_DEPTH; current = scene->entities[current].parent) {
        chain[count++] = current;
    }
    while (count > 0) {
        pose = compose(pose, scene->entities[chain[--count]].transform);
    }
    return pose;
}

/* Sets the entity's transform so that, in the World's space, it is at `position` with
   `rotation` (degrees); its scale is kept. */
static void set_physics_pose(SceneData *scene, int id, Vector2 position, float rotation) {
    SceneTransform *t = &scene->entities[id].transform;
    const ScenePose base = physics_pose(scene, scene->entities[id].parent);
    const float parent_rotation = t->inherit_rotation ? base.rotation : 0.0f;
    const float scale_x = t->inherit_scale ? base.scale.x : 1.0f;
    const float scale_y = t->inherit_scale ? base.scale.y : 1.0f;
    Vector2 offset;
    offset.x = position.x - base.position.x;
    offset.y = position.y - base.position.y;
    offset = rotate(offset, -parent_rotation);
    t->position.x = divide_or(offset.x, scale_x, t->position.x);
    t->position.y = divide_or(offset.y, scale_y, t->position.y);
    t->rotation = rotation - parent_rotation;
}

static bool has_body(const SceneData *scene, int id) {
    return scene->bodies != NULL && id >= 0 && id < scene->entity_capacity && B2_IS_NON_NULL(scene->bodies[id]);
}

static void physics_forget(SceneData *scene, int id) {
    if (has_body(scene, id)) {
        if (b2Body_IsValid(scene->bodies[id])) {
            b2DestroyBody(scene->bodies[id]);
        }
        scene->bodies[id] = b2_nullBodyId;
    }
}

/* Shapes and bodies carry their entity id + 1 as user data. */
static int shape_entity(b2ShapeId shape) {
    return b2Shape_IsValid(shape) ? (int)(intptr_t)b2Shape_GetUserData(shape) - 1 : -1;
}

static void create_body(SceneData *scene, int id) {
    const SceneRigidbody *rb = rigidbody_pool_get(&scene->rigidbodies, id);
    const SceneBoxCollider *collider = collider_pool_get(&scene->colliders, id);
    const ScenePose pose = physics_pose(scene, id);
    b2BodyDef def = b2DefaultBodyDef();
    b2BodyId body;
    def.type = rb == NULL || rb->type == SCENE_BODY_STATIC ? b2_staticBody
             : rb->type == SCENE_BODY_KINEMATIC              ? b2_kinematicBody
                                                             : b2_dynamicBody;
    def.position = to_meters(pose.position);
    def.rotation = b2MakeRot(pose.rotation * DEG2RAD);
    def.userData = (void *)(intptr_t)(id + 1);
    if (rb != NULL) {
        def.gravityScale = rb->gravity_scale;
        def.linearDamping = rb->linear_damping;
        def.angularDamping = rb->angular_damping;
        def.fixedRotation = rb->fixed_rotation;
        def.isBullet = rb->is_bullet;
    }
    body = b2CreateBody(scene->physics_world, &def);
    if (collider != NULL) {
        /* The box at the entity's current scale, offset from its position. */
        b2ShapeDef shape = b2DefaultShapeDef();
        Vector2 half;
        Vector2 centre;
        b2Polygon box;
        half.x = collider->size.x * fabsf(pose.scale.x) * 0.5f;
        half.y = collider->size.y * fabsf(pose.scale.y) * 0.5f;
        centre.x = collider->offset.x * pose.scale.x;
        centre.y = collider->offset.y * pose.scale.y;
        box = b2MakeOffsetBox(half.x / SCENE_PIXELS_PER_METER, half.y / SCENE_PIXELS_PER_METER, to_meters(centre),
                              b2Rot_identity);
        shape.density = collider->density;
        shape.material.friction = collider->friction;
        shape.material.restitution = collider->restitution;
        shape.isSensor = collider->is_trigger;
        shape.enableSensorEvents = true;
        shape.enableContactEvents = true;
        shape.userData = (void *)(intptr_t)(id + 1);
        b2CreatePolygonShape(body, &shape, &box);
    }
    scene->bodies[id] = body;
    scene->body_poses[id] = pose;
}

static void record_contact(SceneData *scene, SceneContactKind kind, b2ShapeId a, b2ShapeId b) {
    const int entity = shape_entity(a);
    const int other = shape_entity(b);
    if (entity < 0 || other < 0) {
        return; /* a shape destroyed meanwhile */
    }
    if (scene->contact_count == scene->contact_capacity) {
        const int capacity = scene->contact_capacity > 0 ? scene->contact_capacity * 2 : 32;
        SceneContactEvent *contacts =
            (SceneContactEvent *)grow(memory_of(scene), scene->contacts, (size_t)scene->contact_count * sizeof(SceneContactEvent),
                                      (size_t)capacity * sizeof(SceneContactEvent));
        if (contacts == NULL) {
            return;
        }
        scene->contacts = contacts;
        scene->contact_capacity = capacity;
    }
    scene->contacts[scene->contact_count].kind = kind;
    scene->contacts[scene->contact_count].entity = entity;
    scene->contacts[scene->contact_count].other = other;
    ++scene->contact_count;
}

static void record_events(SceneData *scene) {
    const b2SensorEvents sensors = b2World_GetSensorEvents(scene->physics_world);
    const b2ContactEvents contacts = b2World_GetContactEvents(scene->physics_world);
    int i;
    for (i = 0; i < sensors.beginCount; ++i) {
        record_contact(scene, SCENE_TRIGGER_ENTER, sensors.beginEvents[i].sensorShapeId, sensors.beginEvents[i].visitorShapeId);
    }
    for (i = 0; i < sensors.endCount; ++i) {
        record_contact(scene, SCENE_TRIGGER_EXIT, sensors.endEvents[i].sensorShapeId, sensors.endEvents[i].visitorShapeId);
    }
    for (i = 0; i < contacts.beginCount; ++i) {
        record_contact(scene, SCENE_CONTACT_BEGIN, contacts.beginEvents[i].shapeIdA, contacts.beginEvents[i].shapeIdB);
    }
    for (i = 0; i < contacts.endCount; ++i) {
        record_contact(scene, SCENE_CONTACT_END, contacts.endEvents[i].shapeIdA, contacts.endEvents[i].shapeIdB);
    }
}

void scene_physics_step(SceneData *scene, float dt) {
    static bool allocator_set = false;
    int i;
    if (B2_IS_NULL(scene->physics_world)) {
        b2WorldDef def;
        if (!allocator_set) {
            b2SetAllocator(physics_alloc, physics_free);
            allocator_set = true;
        }
        def = b2DefaultWorldDef();
        def.gravity.x = SCENE_GRAVITY_X;
        def.gravity.y = SCENE_GRAVITY_Y;
        scene->physics_world = b2CreateWorld(&def);
        scene->physics_time = 0.0f;
    }

    /* Bodies for entities that need one, and entities the game moved since the last step. */
    for (i = 0; i < scene->order_count; ++i) {
        const int id = scene->order[i];
        if (is_ui(scene, id)) {
            continue;
        }
        if (!has_body(scene, id)) {
            if (rigidbody_pool_get(&scene->rigidbodies, id) != NULL || collider_pool_get(&scene->colliders, id) != NULL) {
                create_body(scene, id);
            }
        } else {
            const ScenePose pose = physics_pose(scene, id);
            const ScenePose last = scene->body_poses[id];
            if (pose.position.x != last.position.x || pose.position.y != last.position.y || pose.rotation != last.rotation) {
                b2Body_SetTransform(scene->bodies[id], to_meters(pose.position), b2MakeRot(pose.rotation * DEG2RAD));
                scene->body_poses[id] = pose;
            }
        }
    }

    /* Fixed steps for the time that has passed (at most a quarter second's worth). */
    scene->contact_count = 0;
    scene->physics_time += dt < 0.25f ? dt : 0.25f;
    while (scene->physics_time >= SCENE_PHYSICS_TIME_STEP) {
        b2World_Step(scene->physics_world, SCENE_PHYSICS_TIME_STEP, SCENE_PHYSICS_SUB_STEPS);
        record_events(scene);
        scene->physics_time -= SCENE_PHYSICS_TIME_STEP;
    }

    /* Moving bodies back into their entities, parents before children (a child's transform
       is relative to its parent's new placement). */
    {
        int depth;
        int max_depth = 0;
        for (depth = 0; depth <= max_depth; ++depth) {
            for (i = 0; i < scene->order_count; ++i) {
                const int id = scene->order[i];
                const SceneRigidbody *rb = rigidbody_pool_get(&scene->rigidbodies, id);
                int d = 0;
                int parent;
                if (rb == NULL || rb->type == SCENE_BODY_STATIC || !has_body(scene, id)) {
                    continue;
                }
                for (parent = scene->entities[id].parent; is_live(scene, parent) && d < SCENE_MAX_DEPTH;
                     parent = scene->entities[parent].parent) {
                    ++d;
                }
                if (d > max_depth) {
                    max_depth = d;
                }
                if (d == depth) {
                    const b2BodyId body = scene->bodies[id];
                    set_physics_pose(scene, id, to_scene_units(b2Body_GetPosition(body)),
                                     b2Rot_GetAngle(b2Body_GetRotation(body)) * RAD2DEG);
                    scene->body_poses[id] = physics_pose(scene, id);
                }
            }
        }
    }
}

SceneRigidbody *scene_set_rigidbody(SceneData *scene, int id, SceneBodyType type) {
    const bool existed = is_live(scene, id) && rigidbody_pool_get(&scene->rigidbodies, id) != NULL;
    SceneRigidbody *rb = is_live(scene, id) && !is_ui(scene, id)
                             ? rigidbody_pool_emplace(memory_of(scene), &scene->rigidbodies, id)
                             : NULL;
    if (rb != NULL) {
        if (!existed) {
            rb->gravity_scale = 1.0f;
            rb->linear_damping = 0.0f;
            rb->angular_damping = 0.0f;
            rb->fixed_rotation = false;
            rb->is_bullet = false;
        }
        rb->type = type;
        physics_forget(scene, id); /* re-created with the new settings on the next step */
    }
    return rb;
}

SceneRigidbody *scene_get_rigidbody(SceneData *scene, int id) {
    return is_live(scene, id) ? rigidbody_pool_get(&scene->rigidbodies, id) : NULL;
}

void scene_remove_rigidbody(SceneData *scene, int id) {
    rigidbody_pool_remove(&scene->rigidbodies, id);
    physics_forget(scene, id);
}

void scene_physics_rebuild(SceneData *scene, int id) {
    physics_forget(scene, id);
}

b2BodyId scene_physics_body(const SceneData *scene, int id) {
    return has_body(scene, id) ? scene->bodies[id] : b2_nullBodyId;
}

Vector2 scene_get_velocity(const SceneData *scene, int id) {
    const Vector2 none = {0.0f, 0.0f};
    return has_body(scene, id) ? to_scene_units(b2Body_GetLinearVelocity(scene->bodies[id])) : none;
}

void scene_set_velocity(SceneData *scene, int id, Vector2 velocity) {
    if (has_body(scene, id)) {
        b2Body_SetLinearVelocity(scene->bodies[id], to_meters(velocity));
    }
}

void scene_apply_impulse(SceneData *scene, int id, Vector2 impulse) {
    if (has_body(scene, id)) {
        b2Body_ApplyLinearImpulseToCenter(scene->bodies[id], to_meters(impulse), true);
    }
}

void scene_apply_force(SceneData *scene, int id, Vector2 force) {
    if (has_body(scene, id)) {
        b2Body_ApplyForceToCenter(scene->bodies[id], to_meters(force), true);
    }
}
#endif /* SCENE_PHYSICS */

void scene_unload(SceneData *scene) {
    const SceneDrawHook on_draw = scene->on_draw;
    const SceneDrawUIHook on_draw_ui = scene->on_draw_ui;
    const Font *const ui_font = scene->ui_font;
    void *const user_data = scene->user_data;
    struct EM *const memory = scene->memory;
#if SCENE_PHYSICS
    if (B2_IS_NON_NULL(scene->physics_world)) {
        b2DestroyWorld(scene->physics_world); /* and every body in it */
    }
    release(scene->bodies);
    release(scene->body_poses);
    release(scene->contacts);
    rigidbody_pool_free(&scene->rigidbodies);
#endif
    release(scene->entities);
    release(scene->order);
    release(scene->poses);
    release(scene->pose_stamps);
    release(scene->ui_rects);
    release(scene->ui_stamps);
    sprite_pool_free(&scene->sprites);
    collider_pool_free(&scene->colliders);
    ui_transform_pool_free(&scene->ui_transforms);
    ui_rectangle_pool_free(&scene->ui_rectangles);
    ui_label_pool_free(&scene->ui_labels);
    memset(scene, 0, sizeof(*scene));
    scene->world = scene_transform(0.0f, 0.0f, 0.0f, 1.0f, 1.0f);
    scene->on_draw = on_draw;
    scene->on_draw_ui = on_draw_ui;
    scene->ui_font = ui_font;
    scene->user_data = user_data;
    scene->memory = memory;
}
)GEN",
                               values);

    const std::filesystem::path dir = outDir / kUtilsDir;
    return writeFile(dir / "scene_common.h", h, error) && writeFile(dir / "scene_common.c", c, error);
}

bool writeProjectMain(const std::string& projectName, const std::vector<std::string>& sceneNames,
                      const std::filesystem::path& outDir, std::string& error) {
    (void)sceneNames; // the scene list lives in scene_manager.c
    const std::string c = fill(R"CODE({{marker}}: example main() for project "{{project}}". Do not edit:
// re-export from the editor instead (copy it to your own file to change it).
//
// Shows how the exported scenes fit together in a raylib program: a SceneManager holding
// every scene, scene logic that switches scenes (example_scene_logic), and changing a
// scene at runtime. Build it with every exported .c file and raylib (see the Makefile).
// Put your own code next to it in src/. If your game has its own main(), leave this file
// out of the build (make EXCLUDE=src/project_main.c).
//
// Starts in borderless fullscreen (a borderless window covering the monitor, so Alt+Tab
// works normally). F11 or Alt+Enter toggles windowed mode.
// Controls: Left / Right    switch scenes (done by the scenes themselves, see example_scene_logic)
//           M               cycle through the exported scale modes
//           click           add an entity with a random shape at the mouse (not on the UI: the element under
//                           the mouse is shown instead)
//           Delete          remove the last entity added
//           C               show / hide colliders
//           Space           with physics: make an entity called "Player" jump
//           R               reload the scene from its exported data
//           Esc             quit
// Options:  --windowed      start in a window (design size, fitted to the monitor)
//
// Test builds (compiled with -DSCENE_TESTS, as `make test` does) also accept:
//           --frames N      smoke test: checks the runtime scene API, draws every scene in
//                           every scale mode in a window, then exits (non-zero on failure)
// Release builds (`make release`) leave all the test code out.
#include <string.h>

#ifdef SCENE_TESTS
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#endif

#include <raylib.h>

#include "scenes.h"

#ifdef SCENE_TESTS
#include "easy_memory.h"
#endif

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

/* Scene logic, run by each scene's draw (it's set as every scene's on_draw hook below).
   `*current` is this scene's SceneId; setting it to another one switches scenes. Here
   Left / Right go to the previous / next scene; a game would give each scene its own
   function, e.g. a level's logic setting *current = SCENE_ID_<NEXT_LEVEL> at the exit. */
/* UI logic, run by scene_draw_ui after the scene's UI (in window pixels). It changes a
   label's text at runtime, and draws its own text that scales with the UI: scene_ui_text
   turns a font size in canvas units into pixels for an element's rectangle. Pass your own
   Font (e.g. LoadFontEx("font.ttf", 64, NULL, 0)) instead of the default one; set
   scene->ui_font to use it for the scene's labels too. */
static void example_ui_logic(SceneData *scene) {
    const int score = scene_find_entity(scene, "Score");
    const int minimap = scene_find_entity(scene, "Minimap");
    if (score >= 0) {
        scene_set_ui_label_text(scene, score, TextFormat("Time %.1f", GetTime()));
    }
    if (minimap >= 0) {
        const SceneUIText t = scene_ui_text(scene, minimap, 24.0f, 2.0f);
        scene_draw_ui_text(GetFontDefault(), TextFormat("%d FPS", GetFPS()), t.rect, t.font_size, t.spacing,
                           SCENE_ALIGN_CENTER, SCENE_ALIGN_END, RAYWHITE);
    }
}

static void example_scene_logic(SceneData *scene, int *current) {
    (void)scene;
    if (IsKeyPressed(KEY_RIGHT)) {
        *current = (*current + 1) % SCENE_ID_COUNT;
    }
    if (IsKeyPressed(KEY_LEFT)) {
        *current = (*current + SCENE_ID_COUNT - 1) % SCENE_ID_COUNT;
    }
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

#ifdef SCENE_TESTS
/* --frames: exercises the runtime scene API on every scene. Returns the number of failures. */
static int check(int ok, const char *what, const char *scene_name) {
    if (!ok) {
        printf("FAILED: %s (scene %s)\n", what, scene_name);
    }
    return ok ? 0 : 1;
}

static int near(float a, float b) {
    return fabsf(a - b) < 0.01f;
}

static int pixel_near(float a, float b) {
    return fabsf(a - b) < 1.5f;
}

static int test_scene_api(SceneManager *manager, int id) {
    SceneData *scene = &manager->scenes[id];
    const char *const name = scene_names[id];
    const int before = scene->order_count;
    int failures = 0;
    int parent, child, spawned;
    ScenePose pose;

    /* Add, find, edit and place an entity. */
    spawned = scene_add_entity(scene, "smoke test", SCENE_WORLD);
    failures += check(spawned >= 0 && scene->order_count == before + 1, "add entity", name);
    failures += check(scene_find_entity(scene, "smoke test") == spawned, "find entity", name);
    scene_set_sprite(scene, spawned, (Vector2){10.0f, 10.0f}, RED);
    scene_place_entity(scene, spawned, (Vector2){12.0f, -34.0f});
    pose = scene_entity_pose(scene, spawned);
    failures += check(near(pose.position.x, 12.0f) && near(pose.position.y, -34.0f), "place entity", name);

    /* Children follow a rotated parent, and stay put when the parent is removed. */
    parent = scene_add_entity(scene, "smoke parent", SCENE_WORLD);
    child = scene_add_entity(scene, "smoke child", parent);
    scene_place_entity(scene, parent, (Vector2){100.0f, 0.0f});
    scene_entity(scene, parent)->transform.rotation += 90.0f;
    scene_entity(scene, child)->transform.position = (Vector2){50.0f, 0.0f};
    {
        const ScenePose before_remove = scene_entity_pose(scene, child);
        const ScenePose parent_pose = scene_entity_pose(scene, parent);
        /* Child at local (50, 0): 50 units along the parent's rotated, scaled x axis. */
        const float rad = parent_pose.rotation * (PI / 180.0f);
        const float along = 50.0f * parent_pose.scale.x;
        failures += check(near(before_remove.position.x, parent_pose.position.x + along * cosf(rad)) &&
                              near(before_remove.position.y, parent_pose.position.y + along * sinf(rad)),
                          "child follows its rotated parent", name);
        failures += check(near(before_remove.rotation, parent_pose.rotation), "child inherits rotation", name);
        failures += check(scene_remove_entity(scene, parent), "remove parent", name);
        pose = scene_entity_pose(scene, child);
        failures += check(near(pose.position.x, before_remove.position.x) && near(pose.position.y, before_remove.position.y),
                          "child keeps its place when the parent is removed", name);
        failures += check(scene_entity(scene, child)->parent == SCENE_WORLD, "child re-attached to World", name);
    }
    failures += check(!scene_set_parent(scene, child, child, false), "parenting to itself is refused", name);

    /* Component pools: get after set and remove, and the packed arrays stay consistent. */
    {
        const int sprites_before = scene->sprites.count;
        failures += check(scene_get_sprite(scene, spawned) != NULL, "get sprite after set", name);
        failures += check(scene_get_collider(scene, spawned) == NULL, "no collider until set", name);
        scene_set_collider(scene, spawned, (Vector2){0.0f, 0.0f}, (Vector2){4.0f, 4.0f}, true);
        failures += check(scene_get_collider(scene, spawned) != NULL && scene_get_collider(scene, spawned)->is_trigger,
                          "get collider after set", name);
        failures += check(scene_get_sprite(scene, spawned)->shape == SCENE_SHAPE_RECTANGLE, "a new sprite is a rectangle",
                          name);
        scene_set_sprite_shape(scene, spawned, SCENE_SHAPE_TRIANGLE);
        scene_set_sprite(scene, spawned, (Vector2){12.0f, 12.0f}, BLUE);
        failures += check(scene_get_sprite(scene, spawned)->shape == SCENE_SHAPE_TRIANGLE,
                          "replacing a sprite keeps its shape", name);
        scene_remove_sprite(scene, spawned);
        failures += check(scene_get_sprite(scene, spawned) == NULL && scene->sprites.count == sprites_before - 1,
                          "remove sprite", name);
        scene_set_sprite(scene, spawned, (Vector2){10.0f, 10.0f}, RED);
    }

    /* Pose cache: matches the direct computation for every entity, before and after edits. */
    {
        int pass, k;
        for (pass = 0; pass < 2; ++pass) {
            int mismatches = 0;
            scene_update_poses(scene);
            for (k = 0; k < scene->order_count; ++k) {
                const int id = scene->order[k];
                const ScenePose direct = scene_entity_pose(scene, id);
                const ScenePose cached = scene->poses[id];
                if (!near(direct.position.x, cached.position.x) || !near(direct.position.y, cached.position.y) ||
                    !near(direct.rotation, cached.rotation) || !near(direct.scale.x, cached.scale.x) ||
                    !near(direct.scale.y, cached.scale.y)) {
                    ++mismatches;
                }
            }
            failures += check(mismatches == 0, pass == 0 ? "cached poses" : "cached poses after moving the World",
                              name);
            scene->world.position.x += 25.0f; /* every pose changes; the next pass must see it */
            scene->world.rotation += 10.0f;
        }
        scene->world.position.x -= 50.0f;
        scene->world.rotation -= 20.0f;
    }

    /* UI: layout against the window, anchoring, stretching, nesting, hit tests, and the two
       trees kept apart. Expected rectangles are worked out here from the definitions. */
    {
        const float sw = (float)GetScreenWidth();
        const float sh = (float)GetScreenHeight();
        const float s = sw / SCENE_DESIGN_WIDTH < sh / SCENE_DESIGN_HEIGHT ? sw / SCENE_DESIGN_WIDTH : sh / SCENE_DESIGN_HEIGHT;
        const Vector2 top_right = {1.0f, 0.0f};
        const Vector2 top_left = {0.0f, 0.0f};
        const Vector2 centre = {0.5f, 0.5f};
        const int corner = scene_add_ui_element(scene, "smoke corner", SCENE_CANVAS);
        const int bar = scene_add_ui_element(scene, "smoke bar", SCENE_CANVAS);
        const int inner = scene_add_ui_element(scene, "smoke inner", corner);
        Rectangle r, before_remove;
        *scene_ui_transform(scene, corner) = scene_ui_rect_transform(top_right, top_right, top_right, (Vector2){-10.0f, 10.0f},
                                                                     (Vector2){100.0f, 50.0f});
        *scene_ui_transform(scene, bar) = scene_ui_rect_transform(top_left, top_right, (Vector2){0.5f, 0.0f},
                                                                  (Vector2){0.0f, 0.0f}, (Vector2){-20.0f, 40.0f});
        *scene_ui_transform(scene, inner) = scene_ui_rect_transform(centre, centre, centre, (Vector2){0.0f, 0.0f},
                                                                    (Vector2){20.0f, 10.0f});
        const int label = scene_add_ui_element(scene, "smoke label", bar);
        scene_set_ui_rectangle(scene, corner, BLUE);
        scene_set_ui_rectangle(scene, bar, BLUE);
        scene_set_ui_rectangle(scene, inner, RED);
        scene_set_ui_label(scene, label, "Hi", 20.0f, 2.0f, SCENE_ALIGN_CENTER, SCENE_ALIGN_CENTER, WHITE);
        BeginDrawing();
        scene_draw_ui(scene, SCENE_DEFAULT_SCALE_MODE); /* lays out, and draws the label */
        EndDrawing();
        {
            const SceneUIText t = scene_ui_text(scene, label, 20.0f, 2.0f);
            const Rectangle lr = scene_ui_rect(scene, label);
            char long_text[300];
            failures += check(pixel_near(t.font_size, 20.0f * s) && pixel_near(t.spacing, 2.0f * s) && t.rect.x == lr.x &&
                                  t.rect.width == lr.width,
                              "text sizes scale with the UI", name);
            memset(long_text, 'x', sizeof(long_text) - 1);
            long_text[sizeof(long_text) - 1] = '\0';
            scene_set_ui_label_text(scene, label, long_text);
            failures += check(strlen(scene_get_ui_label(scene, label)->text) == SCENE_TEXT_MAX - 1,
                              "label text is truncated to SCENE_TEXT_MAX", name);
            failures += check(scene_ui_element_at(scene, (Vector2){lr.x + lr.width / 2.0f, lr.y + lr.height / 2.0f}) == label,
                              "hit test finds a label", name);
        }

        r = scene_ui_rect(scene, corner);
        failures += check(pixel_near(r.x, sw - 110.0f * s) && pixel_near(r.y, 10.0f * s) && pixel_near(r.width, 100.0f * s) &&
                              pixel_near(r.height, 50.0f * s),
                          "UI element pinned to the top-right corner", name);
        r = scene_ui_rect(scene, bar);
        failures += check(pixel_near(r.x, 10.0f * s) && pixel_near(r.width, sw - 20.0f * s) && pixel_near(r.height, 40.0f * s),
                          "UI element stretched across the top", name);
        before_remove = scene_ui_rect(scene, inner);
        failures += check(pixel_near(before_remove.x + before_remove.width / 2.0f, sw - 60.0f * s) &&
                              pixel_near(before_remove.width, 20.0f * s),
                          "UI child centred in its parent", name);
        failures += check(scene_ui_element_at(scene, (Vector2){before_remove.x + 2.0f, before_remove.y + 2.0f}) == inner,
                          "hit test finds the topmost UI element", name);
        {
            /* Just below the corner element, and below the bar: none of these elements. */
            const int below = scene_ui_element_at(scene, (Vector2){sw - 60.0f * s, 63.0f * s});
            failures += check(below != corner && below != bar && below != inner, "hit test misses outside the elements", name);
        }
        failures += check(!scene_set_parent(scene, spawned, corner, false) && !scene_set_parent(scene, corner, SCENE_WORLD, false) &&
                              !scene_set_parent(scene, corner, spawned, false),
                          "scene entities and UI elements can't be parented to each other", name);
        failures += check(scene_add_entity(scene, "smoke under ui", corner) >= 0 &&
                              scene_entity(scene, scene_find_entity(scene, "smoke under ui"))->parent == SCENE_WORLD,
                          "a scene entity asked to go under the UI goes under the World", name);
        scene_remove_entity(scene, scene_find_entity(scene, "smoke under ui"));
        failures += check(scene_set_ui_rectangle(scene, spawned, RED) == NULL, "UI components only on UI elements", name);

        scene_remove_entity(scene, corner);
        scene_update_ui(scene, SCENE_DEFAULT_SCALE_MODE);
        r = scene_ui_rect(scene, inner);
        failures += check(scene_entity(scene, inner)->parent == SCENE_CANVAS && pixel_near(r.x, before_remove.x) &&
                              pixel_near(r.y, before_remove.y) && pixel_near(r.width, before_remove.width),
                          "UI child keeps its place when its parent is removed", name);
        scene_remove_entity(scene, inner);
        scene_remove_entity(scene, label);
        failures += check(scene_get_ui_label(scene, label) == NULL, "removed label is gone", name);
        scene_remove_entity(scene, bar);
        failures += check(scene_ui_transform(scene, bar) == NULL && scene_ui_transform(scene, inner) == NULL &&
                              scene_get_ui_rectangle(scene, inner) == NULL,
                          "removed UI elements are gone", name);
    }

    /* Remove everything added; the scene is back to its original entities. */
    scene_remove_entity(scene, child);
    scene_remove_entity(scene, spawned);
    failures += check(scene_entity(scene, spawned) == NULL, "removed entity is gone", name);
    failures += check(scene_get_sprite(scene, spawned) == NULL && scene_get_collider(scene, spawned) == NULL,
                      "removed entity's components are gone", name);
    failures += check(scene->order_count == before, "entity count restored", name);
    {
        /* Every packed slot points back at itself through the sparse index. */
        int k, consistent = 1;
        for (k = 0; k < scene->sprites.count; ++k) {
            consistent &= scene->sprites.sparse[scene->sprites.entity[k]] == k;
        }
        for (k = 0; k < scene->colliders.count; ++k) {
            consistent &= scene->colliders.sparse[scene->colliders.entity[k]] == k;
        }
        for (k = 0; k < scene->ui_transforms.count; ++k) {
            consistent &= scene->ui_transforms.sparse[scene->ui_transforms.entity[k]] == k;
        }
        for (k = 0; k < scene->ui_rectangles.count; ++k) {
            consistent &= scene->ui_rectangles.sparse[scene->ui_rectangles.entity[k]] == k;
        }
        for (k = 0; k < scene->ui_labels.count; ++k) {
            consistent &= scene->ui_labels.sparse[scene->ui_labels.entity[k]] == k;
        }
        failures += check(consistent, "pools consistent", name);
    }
    return failures;
}

/* Memory: a scene in its own small region runs out cleanly, and frees everything back. */
static int test_scene_memory(void) {
    static unsigned char buffer[16 * 1024];
    EM *tiny = em_create_static(buffer, sizeof(buffer));
    EM *nested = em_create_nested(scene_memory(), 256 * 1024);
    SceneData scene;
    int failures = 0;
    int added = 0;
    int i;
    memset(&scene, 0, sizeof(scene));
    failures += check(tiny != NULL && nested != NULL && scene_memory() != NULL, "memory regions created", "memory");

    scene.memory = tiny;
    while (added < 100000 && scene_add_entity(&scene, "filler", SCENE_WORLD) >= 0) {
        scene_set_sprite(&scene, added, (Vector2){1.0f, 1.0f}, RED);
        ++added;
    }
    failures += check(added > 0 && added < 100000, "a full region makes scene_add_entity fail with -1", "memory");
    scene_unload(&scene);
    failures += check(scene.memory == tiny && scene.entities == NULL, "unload keeps the region, frees the arrays", "memory");
    /* Everything went back: the same amount fits again. */
    for (i = 0; i < added && scene_add_entity(&scene, "again", SCENE_WORLD) >= 0; ++i) {
        scene_set_sprite(&scene, i, (Vector2){1.0f, 1.0f}, RED);
    }
    failures += check(i == added, "unloading returns all of a scene's memory", "memory");
    scene_unload(&scene);

    scene.memory = nested;
    for (i = 0; i < 1000; ++i) {
        if (scene_add_entity(&scene, "nested", SCENE_WORLD) < 0) {
            break;
        }
    }
    failures += check(i == 1000, "a scene in a nested region", "memory");
    scene_unload(&scene);
    em_destroy(nested);
    return failures;
}

#if SCENE_PHYSICS
/* Physics: a box falls through a trigger onto the ground and comes to rest on it. */
static int test_physics(void) {
    SceneData scene;
    const char *const name = "physics";
    int failures = 0;
    int ground, box, zone, step;
    int entered = 0;
    int landed = 0;
    float expected_y;
    ScenePose pose;
    memset(&scene, 0, sizeof(scene));
    ground = scene_add_entity(&scene, "ground", SCENE_WORLD);
    scene_entity(&scene, ground)->transform.position = (Vector2){0.0f, 300.0f};
    scene_set_collider(&scene, ground, (Vector2){0.0f, 0.0f}, (Vector2){800.0f, 40.0f}, false);
    zone = scene_add_entity(&scene, "zone", SCENE_WORLD);
    scene_entity(&scene, zone)->transform.position = (Vector2){0.0f, 150.0f};
    scene_set_collider(&scene, zone, (Vector2){0.0f, 0.0f}, (Vector2){200.0f, 40.0f}, true);
    box = scene_add_entity(&scene, "box", SCENE_WORLD);
    scene_set_collider(&scene, box, (Vector2){0.0f, 0.0f}, (Vector2){40.0f, 40.0f}, false);
    scene_set_rigidbody(&scene, box, SCENE_BODY_DYNAMIC)->fixed_rotation = true;

    for (step = 0; step < 240; ++step) {
        int k;
        scene_physics_step(&scene, 1.0f / 60.0f);
        for (k = 0; k < scene.contact_count; ++k) {
            const SceneContactEvent *c = &scene.contacts[k];
            entered += c->kind == SCENE_TRIGGER_ENTER && c->entity == zone && c->other == box;
            landed += c->kind == SCENE_CONTACT_BEGIN && ((c->entity == box && c->other == ground) || (c->entity == ground && c->other == box));
        }
    }
    pose = scene_entity_pose(&scene, box);
    expected_y = 300.0f - 20.0f - 20.0f; /* ground top minus half the box */
    failures += check(fabsf(pose.position.y - expected_y) < 2.0f, "a dynamic body falls and rests on the ground", name);
    failures += check(fabsf(scene_get_velocity(&scene, box).y) < 1.0f, "a resting body has no velocity", name);
    failures += check(entered == 1, "falling through a trigger reports one trigger enter", name);
    failures += check(landed == 1, "landing reports one contact begin", name);

    /* The game moves an entity: its body follows. */
    scene_entity(&scene, box)->transform.position = (Vector2){300.0f, 0.0f};
    scene_physics_step(&scene, 1.0f / 60.0f);
    pose = scene_entity_pose(&scene, box);
    failures += check(fabsf(pose.position.x - 300.0f) < 1.0f && pose.position.y < 10.0f, "moving an entity moves its body",
                      name);
    failures += check(B2_IS_NON_NULL(scene_physics_body(&scene, box)), "bodies are created by the step", name);
    scene_remove_entity(&scene, box);
    failures += check(B2_IS_NULL(scene_physics_body(&scene, box)), "removing an entity destroys its body", name);
    scene_unload(&scene);
    failures += check(B2_IS_NULL(scene.physics_world), "unloading destroys the physics world", name);
    return failures;
}
#endif

/* Scene switching: a scene's on_draw hook sets *current; the manager shows that scene next. */
static int switch_target;
static void switch_to_target(SceneData *scene, int *current) {
    (void)scene;
    *current = switch_target;
}

static int test_scene_manager(SceneManager *manager) {
    int failures = 0;
    int id;
    for (id = 0; id < SCENE_ID_COUNT; ++id) {
        const char *const name = scene_names[id];
        manager->current = id;
        manager->scenes[id].on_draw = switch_to_target;
        failures += check(scene_manager_current(manager) == &manager->scenes[id], "current scene", name);

        switch_target = (id + 1) % SCENE_ID_COUNT;
        BeginDrawing();
        scene_begin(SCENE_DEFAULT_SCALE_MODE);
        scene_manager_draw(manager);
        scene_end();
        EndDrawing();
        failures += check(manager->current == switch_target, "a scene switches to another from its draw", name);

        manager->current = id;
        switch_target = -7;
        scene_manager_draw(manager);
        failures += check(manager->current == id, "an invalid scene id is ignored", name);

        scene_manager_reload(manager, id);
        failures += check(manager->scenes[id].on_draw == switch_to_target, "reloading keeps the scene's hook", name);
        manager->scenes[id].on_draw = example_scene_logic;
    }
    manager->current = 0;
    return failures;
}
#endif /* SCENE_TESTS */

int main(int argc, char **argv) {
    int i;
    int fullscreen = 1;
    int show_colliders = 0;
    int last_added[SCENE_ID_COUNT];
    static SceneManager manager; /* every scene; static, so zero-initialised */
    SceneScaleMode mode = SCENE_DEFAULT_SCALE_MODE;
#ifdef SCENE_TESTS
    int frame = 0;
    int frames_left = -1; /* >= 0 in smoke-test mode */
    int failures = 0;
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

    for (i = 0; i < SCENE_ID_COUNT; ++i) {
        manager.scenes[i].on_draw = example_scene_logic;
        manager.scenes[i].on_draw_ui = example_ui_logic;
        last_added[i] = -1;
    }
    scene_manager_init(&manager, 0);
#ifdef SCENE_TESTS
    if (frames_left >= 0) {
        for (i = 0; i < SCENE_ID_COUNT; ++i) {
            failures += test_scene_api(&manager, i);
        }
        failures += test_scene_manager(&manager);
        failures += test_scene_memory();
#if SCENE_PHYSICS
        failures += test_physics();
#endif
    }
#endif

    while (!WindowShouldClose()) {
        SceneData *scene;
        int shown;
#ifdef SCENE_TESTS
        if (frames_left == 0) {
            break;
        }
        if (frames_left > 0) {
            /* Smoke test: 10 frames per scene, then the next scale mode. */
            manager.current = (frame / 10) % SCENE_ID_COUNT;
            mode = scene_scale_modes[(frame / (10 * SCENE_ID_COUNT)) % SCENE_SCALE_MODE_COUNT];
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
            scene = scene_manager_current(&manager);

            /* Changing a scene at runtime: add an entity where the mouse is (unless it's over
               the UI, laid out by last frame's scene_draw_ui)... */
            if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && scene_ui_element_at(scene, GetMousePosition()) < 0) {
                const Vector2 point = GetScreenToWorld2D(GetMousePosition(), scene_camera(mode));
                const int id = scene_add_entity(scene, "Added", SCENE_WORLD);
                scene_set_sprite(scene, id, (Vector2){40.0f, 40.0f},
                                 ColorFromHSV((float)GetRandomValue(0, 359), 0.6f, 0.95f));
                scene_set_sprite_shape(scene, id, (SceneShape)GetRandomValue(SCENE_SHAPE_RECTANGLE, SCENE_SHAPE_TRIANGLE));
                scene_place_entity(scene, id, point);
                last_added[manager.current] = id;
            }
            /* ...remove it again... */
            if (IsKeyPressed(KEY_DELETE) && scene_remove_entity(scene, last_added[manager.current])) {
                last_added[manager.current] = -1;
            }
#if SCENE_PHYSICS
            /* Physics: Space makes an entity called "Player" jump (if it has a body). */
            if (IsKeyPressed(KEY_SPACE)) {
                const int player = scene_find_entity(scene, "Player");
                const Vector2 velocity = {scene_get_velocity(scene, player).x, -6.0f * SCENE_PIXELS_PER_METER};
                scene_set_velocity(scene, player, velocity);
            }
#endif
            /* ...or rebuild the scene from its exported data. */
            if (IsKeyPressed(KEY_R)) {
                scene_manager_reload(&manager, manager.current);
                last_added[manager.current] = -1;
            }
        }

        BeginDrawing();
        ClearBackground((Color){20, 20, 23, 255});

        /* Physics of the current scene (when the export has physics). */
        scene_manager_update(&manager, GetFrameTime());

        /* The scene's draw runs its logic, which may switch to another scene: remember
           which one this frame shows. */
        shown = manager.current;
        scene = scene_manager_current(&manager);
        scene_begin(mode);
        scene_manager_draw(&manager);
        if (show_colliders) {
            scene_draw_colliders(scene, GREEN);
        }
        scene_end();
        /* The UI goes on top, in window space. */
        scene_manager_draw_ui(&manager, mode);
        {
            const int hovered = scene_ui_element_at(scene, GetMousePosition());
            if (hovered >= 0) {
                const Rectangle r = scene_ui_rect(scene, hovered);
                DrawRectangleLinesEx(r, 2.0f, RAYWHITE);
                DrawText(scene->entities[hovered].name, (int)r.x + 6, (int)r.y + 6, 20, RAYWHITE);
            }
        }

        DrawText(TextFormat("%s  (%d/%d, %d entities)   scale: %s", scene_names[shown], shown + 1, SCENE_ID_COUNT,
                            scene->order_count, scale_mode_names[mode]),
                 10, 10, 20, RAYWHITE);
        DrawText(TextFormat("Left/Right scene   M scale   click add   Delete remove   C colliders   R reload%s   F11 %s",
                            SCENE_PHYSICS ? "   Space jump" : "", fullscreen ? "windowed" : "fullscreen"),
                 10, 34, 20, GRAY);
        EndDrawing();
    }

    scene_manager_deinit(&manager);
    scene_memory_destroy(); /* every scene is unloaded: free the shared region */
    CloseWindow();
#ifdef SCENE_TESTS
    if (frames_left == 0) {
        if (failures > 0) {
            printf("smoke test FAILED: %d check(s)\n", failures);
            return 1;
        }
        printf("smoke test passed: scene API checks on %d scene(s), %d frames in %d scale mode(s)\n", SCENE_ID_COUNT,
               frame, SCENE_SCALE_MODE_COUNT);
    }
#endif
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

bool writeEasyMemory(const std::filesystem::path& outDir, const std::filesystem::path& sourceDir, std::string& error) {
    const std::filesystem::path dir = outDir / kUtilsDir;
    std::error_code ec;
    if (!std::filesystem::is_regular_file(sourceDir / "easy_memory.h", ec)) {
        error = "easy_memory.h not found in " + sourceDir.string();
        return false;
    }
    std::filesystem::create_directories(dir, ec);
    std::filesystem::copy_file(sourceDir / "easy_memory.h", dir / "easy_memory.h",
                               std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) {
        error = "can't copy easy_memory.h from " + sourceDir.string() + ": " + ec.message();
        return false;
    }
    if (std::filesystem::is_regular_file(sourceDir / "LICENSE", ec)) {
        std::filesystem::copy_file(sourceDir / "LICENSE", dir / "easy_memory_LICENSE.txt",
                                   std::filesystem::copy_options::overwrite_existing, ec);
    }
    const std::string c = std::string(kGeneratedMarker) +
                          ": the easy_memory implementation (see easy_memory.h, MIT license in\n"
                          "// easy_memory_LICENSE.txt). Do not edit.\n"
                          "#define EASY_MEMORY_IMPLEMENTATION\n"
                          "#include \"easy_memory.h\"\n";
    return writeFile(dir / "easy_memory.c", c, error);
}

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
#   generated/   the scenes, scenes.h and the SceneManager; rewritten by every export
#   box2d/       Box2D (physics), when Project Settings > Physics is on
#   utils/       scene_common: the runtime scene library every scene uses, and easy_memory
#                (the memory regions scenes allocate from; MIT, see easy_memory_LICENSE.txt)
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
#                 warnings are errors), then a smoke run that checks the runtime scene API
#                 and draws every scene in every exported scale mode
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
	$(CC) $(CPPFLAGS) $(CFLAGS) $(RELEASE_CFLAGS) $(DEPFLAGS) $(NO_WARNINGS) -c $< -o $@

run: release
	"$(CURDIR)/$(TARGET)" $(ARGS)

test: $(TEST_TARGET)
	"$(CURDIR)/$(TEST_TARGET)" --frames {{frames}}

$(TEST_TARGET): $(TEST_OBJECTS) $(RAYLIB_OBJECTS) $(BOX2D_OBJECTS)
	$(CC) $(CFLAGS) $(TEST_OBJECTS) -o $@ $(LINK)

$(BIN_DIR)/test/%.o: %.c
	@$(call MKDIR,$(@D))
	$(CC) $(CPPFLAGS) $(CFLAGS) $(TEST_CFLAGS) $(DEPFLAGS) $(NO_WARNINGS) -c $< -o $@

# Third-party code (easy_memory's implementation) is built without warnings, so strict
# flags like -Werror only judge this project's code. Its header is still included, and
# checked, by scene_common.c.
$(BIN_DIR)/release/$(UTILS_DIR)/easy_memory.o $(BIN_DIR)/test/$(UTILS_DIR)/easy_memory.o: NO_WARNINGS := -w

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
