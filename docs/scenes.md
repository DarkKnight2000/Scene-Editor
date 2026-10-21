# Scenes and the SceneManager

## A scene is a SceneData

Each exported scene `<name>` (its C identifier: `my level` becomes `my_level`) gets a
header `generated/<name>.h` with two functions:

```c
void scene_demo_load(SceneData *scene);                /* build the scene into `scene` */
void scene_demo_draw(SceneData *scene, int *current);  /* draw it and run its logic hook */
```

- **`SceneData` holds everything** about one scene: its entities, components, caches and
  hooks.
- **A zero-initialised `SceneData` is valid and empty**, so a `static` one needs no setup.
- **`_load` replaces whatever the `SceneData` held** with the scene as exported, and
  keeps your hooks.
- **You can load the same scene into several `SceneData`s**, for example a pristine copy
  and a working copy.
- **`scene_unload` frees a scene's memory.** The scene is empty but still usable
  afterwards.

## Drawing a scene

Scene drawing happens between `scene_begin(mode)` and `scene_end()`, which set up the
camera that [scales the scene](scaling.md) to the window. The UI is drawn afterwards in
window space:

```c
static void draw_one_scene(SceneData *scene, SceneScaleMode mode) {
    int current = SCENE_ID_DEMO;
    scene_begin(mode);
    scene_demo_draw(scene, &current); /* sprites in drawing order, then scene->on_draw */
    scene_end();
    scene_draw_ui(scene, mode);       /* UI on top, then scene->on_draw_ui */
}
```

## The SceneManager

`generated/scene_manager.h` lists every scene of the project. It defines:

- **`SceneId`:** `SCENE_ID_<NAME>` for each scene, plus `SCENE_ID_COUNT`.
- **`scene_names`:** the scene names as they appear in the editor.
- **`SceneManager`:** holds every scene's `SceneData` and the id of the scene being shown.

```c
static SceneManager manager; /* static: zero-initialised */

static void run_game(void) {
    const SceneScaleMode mode = SCENE_DEFAULT_SCALE_MODE;
    scene_manager_init(&manager, SCENE_ID_DEMO); /* loads every scene; after InitWindow */
    while (!WindowShouldClose()) {
        scene_manager_update(&manager, GetFrameTime()); /* physics, when exported */
        BeginDrawing();
        ClearBackground(BLACK);
        scene_begin(mode);
        scene_manager_draw(&manager);        /* draws manager.current */
        scene_end();
        scene_manager_draw_ui(&manager, mode); /* the UI of the scene just drawn */
        EndDrawing();
    }
    scene_manager_deinit(&manager);
    scene_memory_destroy();
}
```

| Function | |
|---|---|
| `scene_manager_init(m, first)` | loads every scene (keeping hooks already set) and shows `first` |
| `scene_manager_update(m, dt)` | advances the current scene by `dt` seconds: its [physics](physics.md), when exported. Call once per frame, before drawing. |
| `scene_manager_current(m)` | the `SceneData` being shown |
| `scene_manager_reload(m, id)` | rebuilds scene `id` from its exported data (hooks kept) |
| `scene_manager_draw(m)` | draws the current scene; its logic may switch to another |
| `scene_manager_draw_ui(m, mode)` | draws the UI of the scene `scene_manager_draw` drew |
| `scene_manager_deinit(m)` | unloads every scene |

Scenes keep their state while another one is shown. To start a scene fresh when you
switch to it, call `scene_manager_reload`.

## Game logic: on_draw and on_draw_ui

Exported files are rewritten on every export, so your game code doesn't go in them.
Instead, give a scene **hooks**: function pointers in its `SceneData` that the scene
calls at the right time.

| Hook | Called by | Space | Use it for |
|---|---|---|---|
| `void (*on_draw)(SceneData *scene, int *current)` | the scene's draw function, after its sprites | scene coordinates (inside the camera) | game logic, drawing extra things in the scene, switching scenes |
| `void (*on_draw_ui)(SceneData *scene)` | `scene_draw_ui`, after the UI | window pixels | HUD logic, drawing text or anything on top |

Both hooks, `ui_font` and `user_data` (a `void *` for your own state) survive
`scene_unload` and reloads, so you can set them before or after loading.

```c
typedef struct PlayerState {
    int coins;
} PlayerState;

static PlayerState player_state;

static void level_logic(SceneData *scene, int *current) {
    PlayerState *state = (PlayerState *)scene->user_data;
    const int player = scene_find_entity(scene, "Player");
    SceneEntity *e = scene_entity(scene, player);
    if (e != NULL && IsKeyDown(KEY_RIGHT)) {
        e->transform.position.x += 200.0f * GetFrameTime();
    }
    if (e != NULL && e->transform.position.x > 600.0f) {
        state->coins += 1;
        *current = SCENE_ID_HUD; /* switch scenes: shown from the next frame on */
    }
}

static void setup_hooks(void) {
    manager.scenes[SCENE_ID_DEMO].on_draw = level_logic;
    manager.scenes[SCENE_ID_DEMO].user_data = &player_state;
}
```

## Switching scenes

There are two ways to switch:

- **From a scene's own logic:** set `*current` in its `on_draw` hook, as above.
  `*current` starts as the scene's own `SceneId`.
- **From anywhere else:** set `manager.current`.

Either way, the new scene is drawn from the next `scene_manager_draw` on. A value that
isn't a valid `SceneId` is ignored.

`scene_manager_draw_ui` draws the UI of the scene that was actually drawn this frame,
even if that scene just switched to another. So a frame never shows one scene's world
with another scene's UI.
