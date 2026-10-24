# UI

A scene has two trees:

- **The World tree** holds scene entities, which are placed in the scene and seen through
  the camera.
- **The Canvas tree** holds UI elements, which are laid out on the screen. The camera,
  the World transform and screen shake never move them.

The main use of UI elements is to get **rectangles that fit any screen**: design the HUD
or a menu in the editor, then draw your own widgets into the elements' rects. UI
rectangles and labels are drawn for you if you add them.

## Coordinates

- **The Canvas** is the window. With `SCENE_SCALE_FIT` it is the design area inside the
  black bars instead.
- **Canvas units:** the Canvas is measured in canvas units, which are the design
  resolution's units. On a window of a different shape, the extra length on one axis
  becomes extra canvas.
- **Scaling:** UI keeps its size relative to the design and stays pinned to its anchors
  on any window.
- **Axes:** y points down and (0, 0) is the top-left.

## UI elements

Every UI element is a `SceneUIElement` field of the scene struct:

```c
typedef struct SceneUIElement {
    SceneUIRectTransform layout; /* relative to its parent element or the Canvas */
    Rectangle rect;              /* window pixels, from the last _update_ui / _draw_ui */
    bool visible;                /* false: neither its rectangle nor its label is drawn */
    Color color;                 /* fill of its UI rectangle, if it has one */
    SceneUILabel label;          /* its text, if it has a label */
} SceneUIElement;
```

| Function | What it does |
|---|---|
| `scene_<name>_update_ui(s, mode)` | lays out every element for the current window into its `rect`, and sets `s->ui_canvas` and `s->ui_scale` |
| `scene_<name>_draw_ui(s, mode)` | the same, then draws the rectangles and labels (element by element, parents first) |

Pass the same mode as `scene_begin`. Call `draw_ui` after `scene_end()`, then draw your
own widgets on top.

## Your own widgets in an element's rect

`rect` is in window pixels, so any raylib drawing works with it. Sizes you design in
canvas units become pixels when multiplied by `s->ui_scale`, so they scale with the rest
of the UI.

```c
scene_hud_draw_ui(&hud, mode);

/* A health bar filling the "Health Bar" element. */
{
    Rectangle r = hud.health_bar.rect;
    const float pad = 4.0f * hud.ui_scale; /* 4 canvas units */
    DrawRectangleRec(r, DARKGRAY);
    r.x += pad;
    r.y += pad;
    r.width = (r.width - 2.0f * pad) * health;
    r.height -= 2.0f * pad;
    DrawRectangleRec(r, GREEN);
}

/* Text inside the "Minimap" element, 24 canvas units tall, at its bottom centre. */
scene_draw_ui_text(GetFontDefault(), TextFormat("%d FPS", GetFPS()), hud.minimap.rect, 24.0f * hud.ui_scale,
                   2.0f * hud.ui_scale, SCENE_ALIGN_CENTER, SCENE_ALIGN_END, RAYWHITE);
```

**Rects before drawing:** `draw_ui` updates the layout itself. To read rects earlier in
the frame (for clicks, say), call `scene_<name>_update_ui` first, or use last frame's
rects.

**Clicks:**

```c
/* A button: the "Play" element. */
scene_menu_update_ui(&menu, mode);
const bool over = CheckCollisionPointRec(GetMousePosition(), menu.play.rect);
menu.play.color = over ? SKYBLUE : BLUE;
if (over && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
    manager.current = SCENE_ID_LEVEL;
}
```

## Rect transforms

```c
typedef struct SceneUIRectTransform {
    Vector2 anchor_min;  /* points on the parent rectangle, as fractions (0..1) */
    Vector2 anchor_max;
    Vector2 pivot;       /* the point of the element (fractions of its size) that is placed */
    Vector2 position;    /* where the pivot goes, in canvas units, from the anchor point */
    Vector2 size;        /* canvas units; on a stretched axis, added to the anchors' distance */
} SceneUIRectTransform;
```

- **Pinned:** if `anchor_min` and `anchor_max` are equal on an axis, the element is
  pinned to that point of its parent: a corner, an edge or the centre. `size` is its size.
- **Stretched:** if they differ, the element stretches with its parent along that axis,
  and `size` is added to the distance between the anchors. So 0 fills the gap exactly,
  and -20 leaves 10 units on each side.

| Anchors (min → max) | Result |
|---|---|
| (0, 0) → (0, 0) | pinned top-left |
| (1, 0) → (1, 0) | pinned top-right |
| (0.5, 0.5) → (0.5, 0.5) | pinned to the centre |
| (0, 1) → (1, 1) | stretched along the bottom edge |
| (0, 0) → (1, 1) | fills the parent |

You can change `layout` at runtime, e.g. to slide a panel in; the next `update_ui` uses
it. `scene_ui_layout(parent, &layout, scale)` is the layout rule itself, if you need a
rectangle for a layout of your own.

## Labels

```c
typedef struct SceneUILabel {
    char text[SCENE_TEXT_MAX];  /* truncated to 127 bytes */
    float font_size;            /* canvas units: scales with the UI */
    float spacing;              /* between characters, canvas units */
    SceneTextAlign align_h;     /* SCENE_ALIGN_START (left), _CENTER, _END (right) */
    SceneTextAlign align_v;     /* SCENE_ALIGN_START (top), _CENTER, _END (bottom) */
    Color color;
    const Font *font;           /* NULL: the scene's ui_font, or raylib's default font */
} SceneUILabel;
```

```c
scene_set_ui_label_text(&hud.score.label, TextFormat("Score %d", score));
hud.pause_panel.visible = paused; /* hide the panel's rectangle */
hud.pause_title.visible = paused; /* ...and its title (children aren't hidden with it) */
```

## How text scales

Font sizes are in canvas units, so text is multiplied by the same UI scale as everything
else:

- **Normal case:** a 48-unit label is 48 pixels tall on a window at the design
  resolution.
- **Half-size window:** it's 24 pixels tall.

raylib's default font is a small bitmap font, so it looks blocky when scaled up. For
clean text:

1. Load a TTF at about the largest size you'll show it.
2. Turn on bilinear filtering.
3. Use the font for the scene's labels, or for a single label.

```c
static Font game_font;

static void load_fonts(HudScene *hud) {
    game_font = LoadFontEx("assets/font.ttf", 96, NULL, 0); /* after InitWindow */
    SetTextureFilter(game_font.texture, TEXTURE_FILTER_BILINEAR);
    hud->ui_font = &game_font;           /* every label in this scene without its own font */
    hud->score.label.font = &game_font;  /* or just one label */
}
```

`scene_draw_ui_text(font, text, rect, font_size, spacing, align_h, align_v, color)` draws
any text aligned inside a rectangle, with lines split at `'\n'`. Sizes are in pixels, as
`DrawTextEx` takes them. It doesn't clip or wrap.
