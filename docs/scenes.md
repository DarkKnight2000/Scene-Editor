# Scenes and the SceneManager

## A scene is a struct

Each scene `<name>` exports `generated/<name>.h` and `.c`. The header declares the
scene's struct, with one field per entity and UI element. For a scene called `hud`:

```c
typedef struct HudScene {
    SceneTransform world; /* the World root: moves, rotates and zooms the whole scene */
    const Font *ui_font;  /* labels' font (a label's own font wins); NULL: raylib's default */
    Rectangle ui_canvas;  /* from the last scene_hud_update_ui: the Canvas in window pixels */
    float ui_scale;       /* ...and window pixels per canvas unit */

    /* Entities, in drawing order */
    SceneEntity ground; /* "Ground": rectangle */

    /* UI elements, parents before children (drawn in this order) */
    SceneUIElement health_bar; /* "Health Bar": rectangle */
    SceneUIElement minimap;    /* "Minimap": rectangle */
    SceneUIElement score;      /* "Score": label */
} HudScene;
```

- **Entities** (`SceneEntity`) are placed in the scene: see [Entities](entities.md).
- **UI elements** (`SceneUIElement`) are laid out on the screen: see [UI](ui.md).
- **Scenes with physics bodies** also have `physics_world` and `physics_time`: see
  [Physics](physics.md).

The comment after each field gives its name in the editor and what it has.

## The scene's functions

| Function | What it does |
|---|---|
| `scene_<name>_init(s)` | fills in every designed value (and creates the physics world and bodies), then the poses |
| `scene_<name>_deinit(s)` | frees what init created: the physics world, if any |
| `scene_<name>_update_poses(s)` | recomputes every entity's `pose` from the transforms |
| `scene_<name>_draw(s)` | updates the poses and draws the sprites, between `scene_begin` and `scene_end` |
| `scene_<name>_draw_colliders(s, color)` | debug outlines of the box colliders, between `scene_begin` and `scene_end` |
| `scene_<name>_update_ui(s, mode)` | lays out every UI element's `rect` for the window |
| `scene_<name>_draw_ui(s, mode)` | updates the UI layout and draws the UI rectangles and labels, after `scene_end` |
| `scene_<name>_step(s, dt)` | scenes with physics bodies: advances the physics |

- **Order:** `init` overwrites the whole struct, so call it first and again to start the
  scene over. Call `deinit` before re-initialising a scene with physics.
- **Generated code:** `init`, `update_poses`, `draw` and `update_ui` are straight-line
  code, one statement per entity, with parents always before their children.

## The SceneManager

`generated/scene_manager.h` has every scene in one place:

```c
typedef enum SceneId {
    SCENE_ID_DEMO, /* "demo" */
    SCENE_ID_HUD,  /* "hud" */
    SCENE_ID_COUNT
} SceneId;

typedef struct SceneManager {
    DemoScene demo;
    HudScene hud;
    int current;
} SceneManager;

extern const char *const scene_names[SCENE_ID_COUNT];
```

| Function | What it does |
|---|---|
| `scene_manager_init(m, first)` | initialises every scene, and shows `first` |
| `scene_manager_update(m, dt)` | steps the current scene's physics (scenes with bodies) |
| `scene_manager_draw(m)` | draws the current scene, between `scene_begin` and `scene_end` |
| `scene_manager_draw_colliders(m, color)` | outlines the current scene's box colliders |
| `scene_manager_draw_ui(m, mode)` | lays out and draws the current scene's UI, after `scene_end` |
| `scene_manager_reload(m, id)` | re-initialises a scene from its exported data (its `ui_font` is kept) |
| `scene_manager_deinit(m)` | frees what every scene's init created |

**Switching scenes:** set `current` to another `SceneId`. Each scene keeps its state while
it isn't shown.

**Reaching a scene's fields:** use its member, e.g. `manager.hud.score.rect`.

```c
#include <raylib.h>

#include "scenes.h"

int main(void) {
    static SceneManager manager;
    SceneScaleMode mode = SCENE_DEFAULT_SCALE_MODE;

    InitWindow(1280, 720, "My game");
    SetTargetFPS(60);
    scene_manager_init(&manager, SCENE_ID_DEMO);

    while (!WindowShouldClose()) {
        if (IsKeyPressed(KEY_RIGHT)) {
            manager.current = (manager.current + 1) % SCENE_ID_COUNT;
        }
        scene_manager_update(&manager, GetFrameTime());

        BeginDrawing();
        ClearBackground(BLACK);
        scene_begin(mode);
        scene_manager_draw(&manager);
        scene_end();
        scene_manager_draw_ui(&manager, mode);
        EndDrawing();
    }

    scene_manager_deinit(&manager);
    CloseWindow();
    return 0;
}
```

**Game logic:** your own logic can switch on `manager.current` and use the scene's
fields directly:

```c
static void level_logic(SceneManager *m) {
    if (m->current == SCENE_ID_LEVEL &&
        CheckCollisionPointRec(m->level.player.pose.position, (Rectangle){600.0f, 0.0f, 80.0f, 200.0f})) {
        m->current = SCENE_ID_LEVEL_2; /* reached the exit */
    }
}
```
