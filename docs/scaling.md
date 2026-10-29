# Screen scaling

A game is laid out for a **design resolution** (Project Settings). Players' windows have
other sizes and shapes, and the project's **scale mode** (also in Project Settings) decides
how the design area fits them:

| Mode | Whole design area visible? | Bars? | Extra screen space |
|---|---|---|---|
| Expand | yes | no | shows more of the scene |
| Fit | yes | yes (black) | becomes bars |
| Fill | no (cropped) | no | the window is fully covered |

- **`SCENE_DESIGN_WIDTH` / `SCENE_DESIGN_HEIGHT`:** the design resolution, fixed at
  export time.
- **One mode:** the export has only the project's mode, built into `scene_camera`,
  `scene_begin`, `scene_end` and `scene_ui_canvas`; nothing in the game chooses it. To
  change it, change Project Settings and export again.

## Drawing

```c
BeginDrawing();
ClearBackground(BLACK);
scene_begin();          /* camera: scene origin at the window centre, scaled to the window */
scene_hud_draw(&hud);   /* and anything else you draw here, in scene coordinates */
scene_end();            /* with Fit, also draws the black bars */
scene_hud_draw_ui(&hud);
EndDrawing();
```

The scene origin is always at the centre of the window, so the visible area is centred
on it.

## Mouse and window coordinates

`scene_camera()` returns the `Camera2D` that `scene_begin` uses. You can pass it to
raylib's conversions:

```c
Vector2 mouse_in_scene = GetScreenToWorld2D(GetMousePosition(), scene_camera());
Vector2 in_window = GetWorldToScreen2D(point, scene_camera());
```

UI elements don't use this camera: they're laid out in window space. See [UI](ui.md).
