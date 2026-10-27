# Scenes and the SceneManager

## A scene is a struct

Each scene `<name>` exports `generated/<name>.h` and `.c`. For a scene called `hud`:

```c
typedef struct HudScene {
    SceneTransform world;      /* the World root: moves, rotates and zooms the whole scene */
    SceneEntity ground;        /* "Ground" */
    SceneUIElement health_bar; /* "Health Bar" */
    SceneUIElement minimap;    /* "Minimap" */
    SceneUIElement score;      /* "Score" */
    Rectangle ui_canvas;       /* the Canvas in window pixels, from the last _update_ui */
    float ui_scale;            /* window pixels per canvas unit, from the last _update_ui */
    const Font *ui_font;       /* the labels' font; NULL: raylib's default */
} HudScene;
```

- **Entities** (`SceneEntity`) are placed in the scene: see [Entities](entities.md).
- **UI elements** (`SceneUIElement`) are laid out on the screen: see [UI](ui.md).
- **Scenes with physics bodies** also have `physics_world` and `physics_time`: see
  [Physics](physics.md).

## Its functions

A scene has the functions it needs:

| Function | The scene has it when | What it does |
|---|---|---|
| `scene_<name>_init(s)` | always | fills in every designed value (and creates the physics bodies), then the poses |
| `scene_<name>_update_poses(s)` | it has entities | recomputes every entity's `pose`, parents first |
| `scene_<name>_draw(s)` | it has entities | updates the poses and draws the sprites, between `scene_begin` and `scene_end` |
| `scene_<name>_update_ui(s)` | it has UI elements | lays out every element's `rect` for the window |
| `scene_<name>_draw_ui(s)` | it has UI elements | updates the layout and draws the UI rectangles and labels, after `scene_end` |
| `scene_<name>_step(s, dt)` | it has physics bodies | advances the physics |
| `scene_<name>_deinit(s)` | it has physics bodies | destroys the physics world |

`init` overwrites the whole struct: call it again to start the scene over (after `deinit`,
for a scene with physics).

## The SceneManager

`generated/scene_manager.h` includes every scene and holds them all:

```c
typedef enum SceneId {
    SCENE_ID_DEMO,
    SCENE_ID_HUD,
    SCENE_ID_COUNT
} SceneId;

typedef struct SceneManager {
    DemoScene demo;
    HudScene hud;
    int current;
} SceneManager;
```

| Function | What it does |
|---|---|
| `scene_manager_init(m, first)` | initialises every scene, and shows `first` |
| `scene_manager_update(m, dt)` | steps the current scene's physics |
| `scene_manager_draw(m)` | draws the current scene, between `scene_begin` and `scene_end` |
| `scene_manager_draw_ui(m)` | lays out and draws the current scene's UI, after `scene_end` |
| `scene_manager_deinit(m)` | frees what the scenes' init created |

- **The first scene:** `SCENE_ID_START` is the scene the game starts in: the first scene
  after Export, or the scene you were editing after Play. The example `main()` passes it to
  `scene_manager_init`; pass any `SceneId` instead to choose yourself.
- **Switching scenes:** set `current` to another `SceneId`. Each scene keeps its state while
  it isn't shown.
- **Reaching a scene's fields:** use its member, e.g. `scenes.hud.score.rect`.
- **Without the manager:** call a scene's functions yourself; the manager is only a switch
  over them.

```c
static void level_logic(SceneManager *m) {
    const Rectangle exit_area = {600.0f, 0.0f, 80.0f, 200.0f};
    if (m->current == SCENE_ID_LEVEL && CheckCollisionPointRec(m->level.player.pose.position, exit_area)) {
        m->current = SCENE_ID_LEVEL_2;
    }
}
```
