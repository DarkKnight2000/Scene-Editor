# UI

A scene has two trees:

- **The World tree** holds scene entities, which are placed in the scene and seen through
  the camera.
- **The Canvas tree** holds UI elements, which are laid out on the screen. The camera,
  the World transform and screen shake never move them.

UI elements are entities too, with ids, names and parents, but they're placed by a UI
rect transform instead of a transform.

## Coordinates

- **The Canvas** is the window. With `SCENE_SCALE_FIT` it is the design area inside the
  black bars instead.
- **Canvas units:** the Canvas is measured in canvas units, which are the design
  resolution's units. On a window of a different shape, the extra length on one axis
  becomes extra canvas.
- **Scaling:** UI keeps its size relative to the design and stays pinned to its anchors
  on any window.
- **Axes:** y points down and (0, 0) is the top-left.

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

```c
static void build_hud(SceneData *scene) {
    const Vector2 top_right = {1.0f, 0.0f};
    const Vector2 zero = {0.0f, 0.0f};

    /* 240 x 240, its top-right corner 24 units in from the window's top-right corner. */
    const int minimap = scene_add_ui_element(scene, "Minimap", SCENE_CANVAS);
    *scene_ui_transform(scene, minimap) =
        scene_ui_rect_transform(top_right, top_right, top_right, (Vector2){-24.0f, 24.0f}, (Vector2){240.0f, 240.0f});
    scene_set_ui_rectangle(scene, minimap, (Color){30, 40, 70, 220});

    /* A bar along the bottom, 80 units tall, the full width of the window. */
    const int bar = scene_add_ui_element(scene, "Bottom Bar", SCENE_CANVAS);
    *scene_ui_transform(scene, bar) = scene_ui_rect_transform((Vector2){0.0f, 1.0f}, (Vector2){1.0f, 1.0f},
                                                              (Vector2){0.5f, 1.0f}, zero, (Vector2){0.0f, 80.0f});
    scene_set_ui_rectangle(scene, bar, (Color){20, 20, 26, 230});
}
```

Some details:

- **New elements:** `scene_add_ui_element(scene, name, parent)` adds a 200 × 100 element
  centred on its parent. The parent is `SCENE_CANVAS` or another UI element.
- **Getting the transform:** `scene_ui_transform(scene, id)` returns it, or NULL if `id`
  isn't a UI element.
- **Keeping the two trees apart:**
  - `scene_add_entity` never puts a scene entity under the UI.
  - `scene_set_parent` refuses to parent across the two trees.
  - Sprites and colliders can't be added to UI elements, and UI components can't be
    added to scene entities.
- **Re-parenting with `keep_placement`:** for UI elements, the rectangle they had on a
  canvas of the design resolution is kept.

## UI rectangles and labels

| Component | Functions | Draws |
|---|---|---|
| `SceneUIRectangle { color }` | `scene_set_ui_rectangle / _get_ / _remove_` | a filled rectangle covering the element |
| `SceneUILabel` | `scene_set_ui_label / _get_ / _remove_`, `scene_set_ui_label_text` | text inside the element |

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
static void update_score(SceneData *scene, int score) {
    const int id = scene_find_entity(scene, "Score");
    scene_set_ui_label_text(scene, id, TextFormat("Score %d", score));
}

static void add_title(SceneData *scene, int panel) {
    const int title = scene_add_ui_element(scene, "Title", panel);
    scene_set_ui_label(scene, title, "PAUSED", 48.0f, 4.0f, SCENE_ALIGN_CENTER, SCENE_ALIGN_CENTER, WHITE);
}
```

Within each element the rectangle is drawn before the label. Elements are drawn in
drawing order, parents before children in exported scenes.

## How text scales

Font sizes are in canvas units, so text is multiplied by the same UI scale as everything
else:

- **Normal case:** a 48-unit label is 48 pixels tall on a window at the design
  resolution.
- **Half-size window:** it's 24 pixels tall.

The UI scale for the last layout is `scene->ui_scale`, in window pixels per canvas unit.

raylib's default font is a small bitmap font, so it looks blocky when scaled up. For
clean text:

1. Load a TTF at about the largest size you'll show it.
2. Turn on bilinear filtering.
3. Use the font for the scene's labels, or for a single label.

```c
static Font game_font;

static void load_fonts(SceneData *scene) {
    game_font = LoadFontEx("assets/font.ttf", 96, NULL, 0); /* after InitWindow */
    SetTextureFilter(game_font.texture, TEXTURE_FILTER_BILINEAR);
    scene->ui_font = &game_font; /* every label in this scene without its own font */
}

static void use_font_for(SceneData *scene, int label_id) {
    SceneUILabel *label = scene_get_ui_label(scene, label_id);
    if (label != NULL) {
        label->font = &game_font; /* just this label */
    }
}
```

## Drawing your own text and graphics on the UI

The `on_draw_ui` hook runs after the scene's UI is drawn, in window pixels, so any raylib
drawing works there. To draw text that scales with the UI inside an element,
`scene_ui_text` gives you everything `DrawTextEx` needs:

- `rect`: the element's rectangle, in window pixels;
- `font_size` and `spacing`: the canvas-unit sizes you pass, converted to pixels.

```c
static void hud_ui(SceneData *scene) {
    const int minimap = scene_find_entity(scene, "Minimap");
    const SceneUIText t = scene_ui_text(scene, minimap, 24.0f, 2.0f);

    /* Anywhere inside the rectangle, with DrawTextEx... */
    DrawTextEx(game_font, "N", (Vector2){t.rect.x + 8.0f, t.rect.y + 4.0f}, t.font_size, t.spacing, RAYWHITE);

    /* ...or aligned: lines split at '\n', each aligned on its own. */
    scene_draw_ui_text(game_font, TextFormat("%d FPS", GetFPS()), t.rect, t.font_size, t.spacing,
                       SCENE_ALIGN_CENTER, SCENE_ALIGN_END, RAYWHITE);
}

static void setup_hud(SceneData *scene) {
    scene->on_draw_ui = hud_ui;
}
```

`scene_draw_ui_text` doesn't clip or wrap, and works with any `Font`, including in your
own code outside the hook.

## Layout and clicks

| Function | |
|---|---|
| `scene_update_ui(scene, mode)` | lays out every element for the current window into `scene->ui_rects` (pixels) |
| `scene_ui_rect(scene, id)` | an element's rectangle in window pixels, from the last layout |
| `scene_ui_element_at(scene, point)` | the topmost element with a rectangle or label under `point` (window pixels), or -1 |
| `scene_draw_ui(scene, mode)` | lays out, draws rectangles and labels, then runs `on_draw_ui` |

`scene_draw_ui` lays the UI out every frame. When you need rectangles before drawing,
call `scene_update_ui` yourself; or use the previous frame's layout, as below.

```c
/* A button: an element named "Play" with a rectangle. Uses last frame's layout. */
static void play_button_logic(SceneData *scene, int *current) {
    const int button = scene_find_entity(scene, "Play");
    const int hovered = scene_ui_element_at(scene, GetMousePosition());
    SceneUIRectangle *rect = scene_get_ui_rectangle(scene, button);
    if (rect != NULL) {
        rect->color = hovered == button ? SKYBLUE : BLUE;
    }
    if (hovered == button && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
        *current = SCENE_ID_DEMO;
    }
}

/* Clicks on the UI shouldn't reach the game. */
static bool mouse_over_ui(const SceneData *scene) {
    return scene_ui_element_at(scene, GetMousePosition()) >= 0;
}
```
