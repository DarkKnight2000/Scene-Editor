# API reference

Everything below comes from `scenes.h`, which includes `scene_common.h`, `scene_manager.h`
and every scene's header.

## Per scene: `generated/<name>.h`

For a scene `<name>` with struct type `<Name>Scene` (e.g. `hud` → `HudScene`):

```c
typedef struct HudScene {
    SceneTransform world;  /* the World root */
    const Font *ui_font;   /* labels' default font; NULL: raylib's */
    Rectangle ui_canvas;   /* the Canvas in window pixels, from the last _update_ui */
    float ui_scale;        /* window pixels per canvas unit, from the last _update_ui */
    SceneEntity <entity>;  /* one per entity, in drawing order */
    SceneUIElement <element>; /* one per UI element, parents first */
    b2WorldId physics_world; float physics_time; /* scenes with physics bodies */
} HudScene;
```

| Function | |
|---|---|
| `void scene_<name>_init(<Name>Scene *s)` | the designed values, the physics world and bodies, then the poses |
| `void scene_<name>_deinit(<Name>Scene *s)` | destroys the physics world, if any |
| `void scene_<name>_update_poses(<Name>Scene *s)` | every entity's `pose` from the transforms, parents first |
| `void scene_<name>_draw(<Name>Scene *s)` | `update_poses`, then the sprites (between `scene_begin` and `scene_end`) |
| `void scene_<name>_draw_colliders(<Name>Scene *s, Color color)` | outlines of the box colliders |
| `void scene_<name>_update_ui(<Name>Scene *s, SceneScaleMode mode)` | every element's `rect`, and `ui_canvas` / `ui_scale` |
| `void scene_<name>_draw_ui(<Name>Scene *s, SceneScaleMode mode)` | `update_ui`, then UI rectangles and labels (after `scene_end`) |
| `void scene_<name>_step(<Name>Scene *s, float dt)` | scenes with physics bodies: fixed steps, then bodies back into transforms |

## SceneManager: `generated/scene_manager.h`

| | |
|---|---|
| `SceneId` | `SCENE_ID_<NAME>` per scene, then `SCENE_ID_COUNT` |
| `const char *const scene_names[]` | scene names, by `SceneId` |
| `SceneManager` | one member per scene (named after it), and `int current` |
| `scene_manager_init(m, first)` | initialises every scene, shows `first` |
| `scene_manager_update(m, dt)` | the current scene's physics |
| `scene_manager_draw(m)` | the current scene |
| `scene_manager_draw_colliders(m, color)` | the current scene's colliders |
| `scene_manager_draw_ui(m, mode)` | the current scene's UI |
| `scene_manager_reload(m, id)` | re-initialises a scene, keeping its `ui_font` |
| `scene_manager_deinit(m)` | deinitialises every scene |

## Screen scaling

| | |
|---|---|
| `SCENE_DESIGN_WIDTH`, `SCENE_DESIGN_HEIGHT` | the design resolution |
| `SceneScaleMode` | the exported ones of `SCENE_SCALE_EXPAND` (0), `SCENE_SCALE_FIT` (1), `SCENE_SCALE_FILL` (2) |
| `SCENE_SCALE_MODE_COUNT`, `scene_scale_modes[]` | the exported modes |
| `SCENE_DEFAULT_SCALE_MODE` | the mode chosen in Project Settings |
| `Camera2D scene_camera(SceneScaleMode mode)` | the camera `scene_begin` uses, for coordinate conversions |
| `void scene_begin(SceneScaleMode mode)` / `void scene_end(void)` | wrap drawing in scene coordinates |
| `Rectangle scene_ui_canvas(SceneScaleMode mode)` | the Canvas in window pixels |
| `float scene_ui_scale(Rectangle canvas)` | window pixels per canvas unit |

## Entities

| Type | Fields |
|---|---|
| `SceneTransform` | `position`, `rotation` (degrees), `scale`, `inherit_rotation`, `inherit_scale` |
| `ScenePose` | `position`, `rotation`, `scale`: the placement in the scene |
| `SceneSprite` | `size` (before scaling), `tint` |
| `SceneBoxCollider` | `offset`, `size` (before scaling), `is_trigger`, `density`, `friction`, `restitution` |
| `SceneEntity` | `transform`, `pose`, `sprite`, `collider`, `visible`, and `body` with physics |
| `SceneBox` | `center`, `size`, `rotation`: a placed box |
| `SceneRot` | `c`, `s`: cosine and sine of a rotation |

| Function | |
|---|---|
| `SceneTransform scene_transform(x, y, rotation, scale_x, scale_y)` | inherits rotation and scale |
| `SceneBoxCollider scene_box_collider(offset, size, is_trigger, density, friction, restitution)` | |
| `ScenePose scene_pose_of(SceneTransform t)` | |
| `ScenePose scene_pose_identity(void)` | |
| `SceneRot scene_rot(float degrees)` | |
| `ScenePose scene_compose(ScenePose parent, SceneRot rot, SceneTransform t)` | child pose; `rot = scene_rot(parent.rotation)` |
| `ScenePose scene_child_pose(ScenePose parent, SceneTransform t)` | the same, working out `rot` |
| `SceneBox scene_box(ScenePose pose, Vector2 offset, Vector2 size)` | a box at an entity, in the scene |
| `void scene_box_corners(SceneBox box, Vector2 corners[4])` | rotated corners |
| `void scene_draw_box_lines(SceneBox box, float thick, Color color)` | outline |
| `void scene_draw_rectangle / scene_draw_circle / scene_draw_triangle(const SceneSprite *sprite, ScenePose pose)` | a sprite at a pose |

## UI

| Type | Fields |
|---|---|
| `SceneUIRectTransform` | `anchor_min`, `anchor_max`, `pivot`, `position`, `size` |
| `SceneTextAlign` | `SCENE_ALIGN_START`, `SCENE_ALIGN_CENTER`, `SCENE_ALIGN_END` |
| `SceneUILabel` | `text[SCENE_TEXT_MAX]`, `font_size`, `spacing`, `align_h`, `align_v`, `color`, `font` |
| `SceneUIElement` | `layout`, `rect` (window pixels), `visible`, `color`, `label` |

| Function | |
|---|---|
| `SceneUIRectTransform scene_ui_rect_transform(anchor_min, anchor_max, pivot, position, size)` | |
| `Rectangle scene_ui_layout(Rectangle parent, const SceneUIRectTransform *t, float scale)` | the layout rule |
| `void scene_set_ui_label(label, text, font_size, spacing, align_h, align_v, color)` | every field |
| `void scene_set_ui_label_text(SceneUILabel *label, const char *text)` | copies, truncates |
| `void scene_draw_ui_label(const SceneUILabel *label, Rectangle rect, float ui_scale, Font font)` | |
| `void scene_draw_ui_text(Font font, const char *text, Rectangle rect, float font_size, float spacing, SceneTextAlign align_h, SceneTextAlign align_v, Color color)` | aligned text, pixel sizes |

## Physics (when `SCENE_PHYSICS` is 1)

| | |
|---|---|
| `SCENE_PIXELS_PER_METER` | scene units per metre |
| `SCENE_GRAVITY_X`, `SCENE_GRAVITY_Y` | m/s², y down |
| `SCENE_PHYSICS_TIME_STEP`, `SCENE_PHYSICS_SUB_STEPS` | 1/60 s, 4 |
| `b2Vec2 scene_to_meters(Vector2 v)` / `Vector2 scene_to_scene_units(b2Vec2 m)` | unit conversion |
| `b2WorldId scene_physics_create_world(void)` | a world with the project's gravity |
| `b2BodyDef scene_body_def(ScenePose pose, b2BodyType type, SceneEntity *entity)` | a body definition at a pose |
| `void scene_add_box_collider(b2BodyId body, Vector2 scale, const SceneBoxCollider *collider, SceneEntity *entity)` | a box shape |
| `void scene_physics_advance(b2WorldId world, float *time, float dt)` | fixed steps |
| `void scene_body_to_entity(SceneEntity *entity, ScenePose base)` | a body's place into its transform |
| `Vector2 scene_get_velocity(const SceneEntity *e)` | scene units per second |
| `void scene_set_velocity(SceneEntity *e, Vector2 velocity)` | |
| `void scene_apply_impulse(SceneEntity *e, Vector2 impulse)` | kg · scene units / s |
| `void scene_apply_force(SceneEntity *e, Vector2 force)` | kg · scene units / s², for one step |
