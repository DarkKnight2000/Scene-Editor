# API reference

`scene_manager.h` includes everything: `scene_common.h` and every scene's header.

## Per scene: `generated/<name>.h`

For a scene `<name>` with struct type `<Name>Scene` (e.g. `hud` → `HudScene`):

| Member | |
|---|---|
| `SceneTransform world` | the World root |
| `SceneEntity <entity>` | one per entity, in drawing order |
| `SceneUIElement <element>` | one per UI element, parents first |
| `Rectangle ui_canvas`, `float ui_scale` | scenes with UI: the Canvas in window pixels and pixels per canvas unit, from the last `_update_ui` |
| `const Font *ui_font` | scenes with labels: the labels' font, NULL for raylib's default |
| `b2WorldId physics_world`, `float physics_time` | scenes with physics bodies |

| Function | When the scene has it |
|---|---|
| `void scene_<name>_init(<Name>Scene *s)` | always |
| `void scene_<name>_update_poses(<Name>Scene *s)` | entities |
| `void scene_<name>_draw(<Name>Scene *s)` | entities |
| `void scene_<name>_update_ui(<Name>Scene *s)` | UI elements |
| `void scene_<name>_draw_ui(<Name>Scene *s)` | UI elements |
| `void scene_<name>_step(<Name>Scene *s, float dt)` | physics bodies |
| `void scene_<name>_deinit(<Name>Scene *s)` | physics bodies |

## SceneManager: `generated/scene_manager.h`

| | |
|---|---|
| `SceneId` | `SCENE_ID_<NAME>` per scene, then `SCENE_ID_COUNT` |
| `SceneManager` | one member per scene (named after it), and `int current` |
| `SCENE_ID_START` | the scene to start in: the first scene, or after Play the one being edited |
| `scene_manager_init(m, first)` | initialises every scene, shows `first` |
| `scene_manager_update(m, dt)` | the current scene's physics |
| `scene_manager_draw(m)` | the current scene |
| `scene_manager_draw_ui(m)` | the current scene's UI |
| `scene_manager_deinit(m)` | frees what the scenes' init created |

## Screen scaling: `utils/scene_common.h`

| | |
|---|---|
| `SCENE_DESIGN_WIDTH`, `SCENE_DESIGN_HEIGHT` | the design resolution |
| `Camera2D scene_camera(void)` | the camera `scene_begin` uses, for the project's scale mode |
| `void scene_begin(void)`, `void scene_end(void)` | draw in scene coordinates in between |
| `Rectangle scene_ui_canvas(void)` | the Canvas in window pixels |
| `float scene_ui_scale(Rectangle canvas)` | window pixels per canvas unit |

## Entities

| Type | Fields |
|---|---|
| `SceneTransform` | `position`, `rotation` (degrees), `scale`, `ignore_parent_rotation`, `ignore_parent_scale` |
| `ScenePose` | `position`, `rotation`, `scale` |
| `SceneSprite` | `size` (before scaling), `tint` |
| `SceneBoxCollider` | `offset`, `size` (before scaling), `is_trigger`, `density`, `friction`, `restitution` |
| `SceneEntity` | `transform`, `pose`, `sprite`, `collider`, `visible`, and `body` and `platformer` with physics |
| `SceneBox` | `center`, `size`, `rotation` |

| | |
|---|---|
| `SCENE_POSE_IDENTITY` | position 0, rotation 0, scale 1 |
| `ScenePose scene_child_pose(ScenePose parent, SceneTransform t)` | where a child with local transform `t` ends up |
| `SceneBox scene_box(ScenePose pose, Vector2 offset, Vector2 size)` | a box at an entity, in the scene |
| `void scene_draw_rectangle(SceneSprite sprite, ScenePose pose)` | also `scene_draw_circle`, `scene_draw_triangle` |

## UI

| Type | Fields |
|---|---|
| `SceneUIRectTransform` | `anchor_min`, `anchor_max`, `pivot`, `position`, `size` |
| `SceneTextAlign` | `SCENE_ALIGN_START`, `SCENE_ALIGN_CENTER`, `SCENE_ALIGN_END` |
| `SceneUILabel` | `text`, `font_size`, `spacing`, `align_h`, `align_v`, `color` |
| `SceneUIElement` | `layout`, `rect` (window pixels), `color`, `label`, `visible` |

| | |
|---|---|
| `Rectangle scene_ui_layout(Rectangle parent, SceneUIRectTransform layout, float scale)` | the layout rule |
| `void scene_draw_ui_label(SceneUILabel label, Rectangle rect, float ui_scale, Font font)` | |
| `void scene_draw_ui_text(Font font, const char *text, Rectangle rect, float font_size, float spacing, SceneTextAlign align_h, SceneTextAlign align_v, Color color)` | aligned text, pixel sizes |

## Physics (when `SCENE_PHYSICS` is 1)

| | |
|---|---|
| `SCENE_PIXELS_PER_METER` | scene units per metre |
| `SCENE_GRAVITY_X`, `SCENE_GRAVITY_Y` | m/s², y down |
| `b2Vec2 scene_to_meters(Vector2 v)`, `Vector2 scene_to_scene_units(b2Vec2 m)` | unit conversion |
| `ScenePlatformer` | a platformer controller: `move_speed`, `acceleration`, `air_control`, `jump_height`, `coyote_time`, `jump_buffer`, `jump_cut`, `max_fall_speed`; input `manual_input`, `move`, `jump_pressed`, `jump_held`; state `grounded`, `coyote_left`, `buffer_left`, `rising` |
| `void scene_platformer_update(SceneEntity *entity, float dt)` | moves a platformer character for one frame; called by the scene's `_step` |

`scene_physics_create_world`, `scene_body_def`, `scene_add_box_collider`,
`scene_physics_advance` and `scene_body_to_entity` are used by the generated scenes.
