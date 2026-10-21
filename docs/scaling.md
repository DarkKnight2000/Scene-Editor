# Screen scaling

A game is laid out for a **design resolution** (Project Settings). Players' windows have
other sizes and shapes, and the **scale mode** decides how the design area fits them:

| Mode | Whole design area visible? | Bars? | Extra screen space |
|---|---|---|---|
| `SCENE_SCALE_EXPAND` | yes | no | shows more of the scene |
| `SCENE_SCALE_FIT` | yes | yes (black) | becomes bars |
| `SCENE_SCALE_FILL` | no (cropped) | no | the window is fully covered |

- **`SCENE_DESIGN_WIDTH` / `SCENE_DESIGN_HEIGHT`:** the design resolution, fixed at
  export time.
- **`SCENE_DEFAULT_SCALE_MODE`:** the mode chosen in Project Settings, as a suggested
  default.
- **Choosing a mode:** you choose the mode at runtime, every frame if you like, e.g. from
  an options menu.

## Drawing with a mode

```c
static void draw_frame(SceneData *scene, SceneScaleMode mode) {
    int current = SCENE_ID_DEMO;
    BeginDrawing();
    ClearBackground(BLACK);
    scene_begin(mode);      /* camera: scene origin at the window centre, scaled for `mode` */
    scene_demo_draw(scene, &current);
    scene_end();            /* with Fit, also draws the black bars */
    scene_draw_ui(scene, mode);
    EndDrawing();
}
```

The scene origin is always at the centre of the window, so the visible area is centred
on it.

## Mouse and window coordinates

`scene_camera(mode)` returns the `Camera2D` that `scene_begin` uses. You can pass it to
raylib's conversions:

```c
static Vector2 mouse_in_scene(SceneScaleMode mode) {
    return GetScreenToWorld2D(GetMousePosition(), scene_camera(mode));
}

static Vector2 scene_to_window(Vector2 point, SceneScaleMode mode) {
    return GetWorldToScreen2D(point, scene_camera(mode));
}
```

UI elements don't use this camera: they're laid out in window space. See [UI](ui.md).
