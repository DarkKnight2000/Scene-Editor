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
- **Which modes exist:** only the modes ticked under *Exported modes* in Project Settings
  are exported. `SceneScaleMode` has only those, with fixed values (Expand 0, Fit 1,
  Fill 2), and `scene_scale_modes[0 .. SCENE_SCALE_MODE_COUNT-1]` lists them:

```c
/* An options menu entry: step through the modes this export has. */
static SceneScaleMode next_mode(SceneScaleMode mode) {
    int i;
    for (i = 0; i < SCENE_SCALE_MODE_COUNT; ++i) {
        if (scene_scale_modes[i] == mode) {
            return scene_scale_modes[(i + 1) % SCENE_SCALE_MODE_COUNT];
        }
    }
    return SCENE_DEFAULT_SCALE_MODE;
}
```

Code that names a mode that wasn't exported, such as `SCENE_SCALE_FILL` in an
Expand-only export, doesn't compile. To write code that works with any export, use
`SCENE_DEFAULT_SCALE_MODE` and `scene_scale_modes`.

## Drawing with a mode

```c
static void draw_frame(HudScene *hud, SceneScaleMode mode) {
    BeginDrawing();
    ClearBackground(BLACK);
    scene_begin(mode);     /* camera: scene origin at the window centre, scaled for `mode` */
    scene_hud_draw(hud);   /* and anything else you draw here, in scene coordinates */
    scene_end();           /* with Fit, also draws the black bars */
    scene_hud_draw_ui(hud, mode);
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
