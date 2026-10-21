# API reference

Everything is declared in `utils/scene_common.h` (the runtime) and
`generated/scene_manager.h` / `generated/<scene>.h` (per project). `#include "scenes.h"`
includes all of them.

Functions that take a `SceneData *` and an id do nothing, and return NULL, -1 or false,
when the id isn't a live entity of the right kind.

## Macros

| Macro | Meaning |
|---|---|
| `SCENE_DESIGN_WIDTH`, `SCENE_DESIGN_HEIGHT` | the design resolution (float), fixed at export |
| `SCENE_DEFAULT_SCALE_MODE` | the scale mode chosen in Project Settings |
| `SCENE_SCALE_MODE_COUNT` | how many scale modes this export has (Project Settings > Export) |
| `SCENE_WORLD` (-1) | parent id: the World root |
| `SCENE_CANVAS` (-2) | parent id: the Canvas, the root of the UI |
| `SCENE_NAME_MAX` (64) | entity name buffer size |
| `SCENE_TEXT_MAX` (128) | label text buffer size |
| `SCENE_MEMORY_SIZE` | bytes in the shared memory region (default 8 MiB; define it to change it) |
| `SCENE_ID_<NAME>`, `SCENE_ID_COUNT` | scene ids (`SceneId` enum, scene_manager.h) |

## Types

| Type | |
|---|---|
| `SceneData` | one scene: entities, components, caches and your hooks. Zero-initialised = empty. |
| `SceneEntity` | `name`, `parent`, `transform`, `alive` |
| `SceneTransform` | `position`, `rotation` (degrees), `scale`, `inherit_rotation`, `inherit_scale` |
| `ScenePose` | final `position`, `rotation`, `scale` after parents and the World |
| `SceneSprite` | `size`, `tint` |
| `SceneBoxCollider` | `offset`, `size`, `is_trigger` |
| `SceneUIRectTransform` | `anchor_min`, `anchor_max`, `pivot`, `position`, `size` |
| `SceneUIRectangle` | `color` |
| `SceneUILabel` | `text`, `font_size`, `spacing`, `align_h`, `align_v`, `color`, `font` |
| `SceneTextAlign` | `SCENE_ALIGN_START` (left/top), `SCENE_ALIGN_CENTER`, `SCENE_ALIGN_END` (right/bottom) |
| `SceneUIText` | `rect` (pixels), `font_size`, `spacing` (pixels) |
| `SceneScaleMode` | `SCENE_SCALE_EXPAND` (0), `SCENE_SCALE_FIT` (1), `SCENE_SCALE_FILL` (2): only the exported ones |
| `SceneDrawHook` | `void (*)(SceneData *scene, int *current)` |
| `SceneDrawUIHook` | `void (*)(SceneData *scene)` |
| `SceneManager` | `scenes[SCENE_ID_COUNT]`, `current`, `drawn` |
| `Scene<T>Pool` | `data`, `entity`, `count`, `capacity`, `sparse` (see [Components](components.md#looping-over-a-component-type)) |

### SceneData fields you use

| Field | |
|---|---|
| `world` | the World transform: moves, rotates and zooms the whole scene |
| `entities[id]`, `entity_count` | all entities by id, including removed ones (`alive` false) |
| `order[0..order_count-1]` | ids of live entities, in drawing order |
| `sprites`, `colliders`, `ui_transforms`, `ui_rectangles`, `ui_labels` | component pools |
| `poses[id]` | final poses, after `scene_update_poses` |
| `ui_rects[id]`, `ui_canvas`, `ui_scale` | UI layout in window pixels, after `scene_update_ui` |
| `on_draw`, `on_draw_ui`, `ui_font`, `user_data`, `memory` | yours: kept by `scene_unload` |

## Per scene (generated/&lt;name&gt;.h)

| Function | |
|---|---|
| `void scene_<name>_load(SceneData *scene)` | builds the scene into `scene`, replacing its contents (hooks kept) |
| `void scene_<name>_draw(SceneData *scene, int *current)` | `scene_draw`, then `on_draw`; between `scene_begin` and `scene_end` |

## SceneManager (generated/scene_manager.h)

| Function | |
|---|---|
| `extern const char *const scene_names[SCENE_ID_COUNT]` | scene names as in the editor |
| `void scene_manager_init(SceneManager *m, int first)` | loads every scene, shows `first` |
| `SceneData *scene_manager_current(SceneManager *m)` | the scene shown |
| `void scene_manager_reload(SceneManager *m, int id)` | rebuilds one scene from its exported data |
| `void scene_manager_draw(SceneManager *m)` | draws the current scene; it may switch `current` |
| `void scene_manager_draw_ui(SceneManager *m, SceneScaleMode mode)` | the UI of the scene just drawn; after `scene_end` |
| `void scene_manager_deinit(SceneManager *m)` | unloads every scene |

## Screen scaling

| Function | |
|---|---|
| `extern const SceneScaleMode scene_scale_modes[SCENE_SCALE_MODE_COUNT]` | the exported scale modes |
| `Camera2D scene_camera(SceneScaleMode mode)` | the camera for the current window: scene origin at the centre |
| `void scene_begin(SceneScaleMode mode)` | starts drawing the scene (after `BeginDrawing`) |
| `void scene_end(void)` | ends it; with Fit, draws the bars |

## Entities

| Function | |
|---|---|
| `SceneTransform scene_transform(float x, float y, float rotation, float scale_x, float scale_y)` | a transform that inherits rotation and scale |
| `int scene_add_entity(SceneData *scene, const char *name, int parent)` | new entity at the parent's position, drawn on top; -1 if out of memory |
| `bool scene_remove_entity(SceneData *scene, int id)` | removes it and its components; children move to its parent without moving |
| `SceneEntity *scene_entity(SceneData *scene, int id)` | the entity (valid until entities are added) |
| `int scene_find_entity(const SceneData *scene, const char *name)` | first live entity with this name, in drawing order |
| `bool scene_set_parent(SceneData *scene, int id, int parent, bool keep_placement)` | re-parents within the same tree; refuses loops |
| `void scene_update_poses(SceneData *scene)` | fills `scene->poses` |
| `ScenePose scene_entity_pose(const SceneData *scene, int id)` | one entity's final pose, always current |
| `void scene_place_entity(SceneData *scene, int id, Vector2 point)` | moves it so its final position is `point` |
| `void scene_unload(SceneData *scene)` | frees the scene's memory; keeps your fields |

## Components

| Function | |
|---|---|
| `SceneSprite *scene_set_sprite(SceneData *scene, int id, Vector2 size, Color tint)` | add or replace |
| `SceneSprite *scene_get_sprite(SceneData *scene, int id)` | or NULL |
| `void scene_remove_sprite(SceneData *scene, int id)` | |
| `SceneBoxCollider *scene_set_collider(SceneData *scene, int id, Vector2 offset, Vector2 size, bool is_trigger)` | add or replace |
| `SceneBoxCollider *scene_get_collider(SceneData *scene, int id)` | or NULL |
| `void scene_remove_collider(SceneData *scene, int id)` | |
| `void scene_draw(SceneData *scene)` | every sprite in drawing order |
| `void scene_draw_colliders(SceneData *scene, Color color)` | debug outlines |

Component pointers stay valid until a component of the same type is added or removed.

## UI

| Function | |
|---|---|
| `int scene_add_ui_element(SceneData *scene, const char *name, int parent)` | 200 × 100, centred on `parent` (`SCENE_CANVAS` or a UI element) |
| `SceneUIRectTransform scene_ui_rect_transform(Vector2 anchor_min, Vector2 anchor_max, Vector2 pivot, Vector2 position, Vector2 size)` | builds one |
| `SceneUIRectTransform *scene_ui_transform(SceneData *scene, int id)` | the element's rect transform, or NULL |
| `SceneUIRectangle *scene_set_ui_rectangle(SceneData *scene, int id, Color color)` | add or replace |
| `SceneUIRectangle *scene_get_ui_rectangle(SceneData *scene, int id)` | or NULL |
| `void scene_remove_ui_rectangle(SceneData *scene, int id)` | |
| `SceneUILabel *scene_set_ui_label(SceneData *scene, int id, const char *text, float font_size, float spacing, SceneTextAlign align_h, SceneTextAlign align_v, Color color)` | add or replace |
| `SceneUILabel *scene_get_ui_label(SceneData *scene, int id)` | or NULL |
| `void scene_remove_ui_label(SceneData *scene, int id)` | |
| `void scene_set_ui_label_text(SceneData *scene, int id, const char *text)` | copies, truncating |
| `void scene_update_ui(SceneData *scene, SceneScaleMode mode)` | lays out every element into `scene->ui_rects` |
| `Rectangle scene_ui_rect(const SceneData *scene, int id)` | from the last layout, window pixels |
| `int scene_ui_element_at(const SceneData *scene, Vector2 point)` | topmost element with a rectangle or label under `point`, or -1 |
| `SceneUIText scene_ui_text(const SceneData *scene, int id, float font_size, float spacing)` | the element's rectangle and the sizes in pixels |
| `void scene_draw_ui_text(Font font, const char *text, Rectangle rect, float font_size, float spacing, SceneTextAlign align_h, SceneTextAlign align_v, Color color)` | aligned text in a rectangle, any font |
| `void scene_draw_ui(SceneData *scene, SceneScaleMode mode)` | lays out and draws the UI, then `on_draw_ui`; after `scene_end` |

## Memory

| Function | |
|---|---|
| `struct EM *scene_memory(void)` | the shared region (created on first use) |
| `void scene_memory_destroy(void)` | frees it, once no scene uses it |

The easy_memory functions themselves (`em_create_nested`, `em_create_static`,
`em_destroy`, …) are documented in `utils/easy_memory.h`.
