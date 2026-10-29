# UI

A scene has two trees:

- **The World tree** holds scene entities, placed in the scene and seen through the camera.
- **The Canvas tree** holds UI elements, laid out on the screen. The camera, the World
  transform and screen shake never move them.

The main use of UI elements is **rectangles that fit any screen**: design a HUD or a menu
in the editor, then draw your own widgets into the elements' rects. UI rectangles and labels
added in the editor are drawn for you.

## Coordinates

- **The Canvas** is the window. With the Fit [scale mode](scaling.md) it is the design area
  inside the black bars instead.
- **Canvas units:** layouts are in canvas units, the design resolution's units. On a window
  of a different shape, the extra length on one axis becomes extra canvas.
- **Rects:** an element's `rect` is in window pixels, y down, (0, 0) at the top-left.

## UI elements

```c
typedef struct SceneUIElement {
    SceneUIRectTransform layout; /* relative to its parent element or the Canvas */
    Rectangle rect;     /* window pixels, from the last _update_ui or _draw_ui */
    Color color;        /* its UI rectangle, if it has one */
    SceneUILabel label; /* if it has one */
    bool visible;       /* false: its rectangle and label aren't drawn */
} SceneUIElement;
```

| Function | What it does |
|---|---|
| `scene_<name>_update_ui(s)` | lays out every element's `rect`, and sets `s->ui_canvas` and `s->ui_scale` |
| `scene_<name>_draw_ui(s)` | the same, then draws the rectangles and labels, parents first |

Call `draw_ui` after `scene_end()`.

## Your own widgets

Draw into an element's `rect` after `draw_ui`. Sizes designed in canvas units become pixels
when multiplied by `s->ui_scale`, so they scale with the rest of the UI:

```c
scene_hud_draw_ui(&hud);

/* A health bar filling the "Health Bar" element, with a 4-unit margin. */
{
    const float pad = 4.0f * hud.ui_scale;
    Rectangle r = hud.health_bar.rect;
    r = (Rectangle){r.x + pad, r.y + pad, (r.width - 2.0f * pad) * health, r.height - 2.0f * pad};
    DrawRectangleRec(r, GREEN);
}

/* Text at the bottom of the "Minimap" element, 24 canvas units tall. */
scene_draw_ui_text(GetFontDefault(), TextFormat("%d FPS", GetFPS()), hud.minimap.rect, 24.0f * hud.ui_scale,
                   2.0f * hud.ui_scale, SCENE_ALIGN_CENTER, SCENE_ALIGN_END, RAYWHITE);
```

**Clicks:** call `update_ui` first (or use last frame's rects), then test the rect:

```c
scene_menu_update_ui(&menu);
if (CheckCollisionPointRec(GetMousePosition(), menu.play.rect) && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
    scenes.current = SCENE_ID_LEVEL;
}
```

## Rect transforms

```c
typedef struct SceneUIRectTransform {
    Vector2 anchor_min; /* points on the parent rectangle, as fractions (0..1) */
    Vector2 anchor_max;
    Vector2 pivot;      /* the point of the element (fractions of its size) that is placed */
    Vector2 position;   /* where the pivot goes, from the anchor point */
    Vector2 size;       /* on a stretched axis, added to the distance between the anchors */
} SceneUIRectTransform;
```

- **Pinned:** if `anchor_min` and `anchor_max` are equal on an axis, the element is pinned
  to that point of its parent (a corner, an edge or the centre), and `size` is its size.
- **Stretched:** if they differ, the element stretches with its parent along that axis, and
  `size` is added to the distance between the anchors: 0 fills the gap, -20 leaves 10 units
  on each side.

| Anchors (min → max) | Result |
|---|---|
| (0, 0) → (0, 0) | pinned top-left |
| (1, 0) → (1, 0) | pinned top-right |
| (0.5, 0.5) → (0.5, 0.5) | pinned to the centre |
| (0, 1) → (1, 1) | stretched along the bottom edge |
| (0, 0) → (1, 1) | fills the parent |

You can change `layout` at runtime, e.g. to slide a panel in; the next `update_ui` uses it.

## Linear layouts

Add a **UI Linear Layout** to an element to line up its children, in order, along one axis:
a toolbar of buttons, a menu, a list. Each child keeps its own size.

| Setting | What it does |
|---|---|
| Direction | **Horizontal** (left to right) or **Vertical** (top to bottom) |
| Spacing | the gap between children, in canvas units |
| Padding | the gap before the first child and after the last, in canvas units |
| Align | where the row goes when it's shorter than the element: start, centre or end |

- **Along the axis**, the layout places the children; their offset along it is ignored.
- **Across the axis**, each child is placed by its own anchors, pivot and offset. Anchor it
  to the middle to centre it, or stretch it (e.g. anchors (0, 0) → (1, 0) in a vertical list)
  to fill the element's width.
- **Children of children** follow their own parent as usual, e.g. a badge on a button.

In the exported code, `_update_ui` lays the children out on their parent, then calls:

```c
void scene_ui_linear_layout(Rectangle parent, SceneUILinearLayout layout, float scale,
                            SceneUIElement *const children[], int count);
```

- **Hiding children:** a child with `visible = false` takes no room, and the others close
  the gap. Use it to show and hide menu entries at runtime.
- **The settings** are fixed at export.

## Labels

```c
typedef struct SceneUILabel {
    const char *text;
    float font_size; /* canvas units */
    float spacing;
    SceneTextAlign align_h; /* SCENE_ALIGN_START (left), _CENTER, _END (right) */
    SceneTextAlign align_v; /* SCENE_ALIGN_START (top), _CENTER, _END (bottom) */
    Color color;
} SceneUILabel;
```

- **Changing the text:** point `text` at your own string. It isn't copied, so it must stay
  valid while the label is drawn:

  ```c
  static char score_text[32];
  snprintf(score_text, sizeof score_text, "Score %d", score);
  hud.score.label.text = score_text;
  ```

- **Fonts:** labels use the scene's `ui_font`, or raylib's default font when it's NULL. The
  default font is a small bitmap font that looks blocky when scaled up. For clean text, load
  a TTF at about the largest size you'll show and filter it:

  ```c
  static Font font;
  font = LoadFontEx("assets/font.ttf", 96, NULL, 0); /* after InitWindow */
  SetTextureFilter(font.texture, TEXTURE_FILTER_BILINEAR);
  hud.ui_font = &font;
  ```

`scene_draw_ui_text(font, text, rect, font_size, spacing, align_h, align_v, color)` draws any
text aligned inside a rectangle, with lines split at `'\n'`. Sizes are in pixels. It
doesn't clip or wrap.
