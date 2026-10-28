#pragma once

#include <raylib.h>

#include "Project.h"
#include "Scene.h"

// Visible scene area (width, height), centred on the scene origin, for a screen whose
// aspect ratio is `aspect` (width / height) under the project's scale mode. The exported
// scene_begin() camera shows exactly this area.
Vector2 visibleSceneArea(const DisplaySettings& display, float aspect);

// Where the UI canvas sits for a screen of shape `aspect`: `frame` is the screen area it
// covers, in scene units (the whole screen, or with Fit the design area inside the bars),
// and `size` is the canvas size in canvas units (the design resolution, its longer side
// extended to the frame's shape). The exported scene_update_ui() uses the same rule.
struct UICanvasFrame {
    Rectangle frame;
    Vector2 size;
};
UICanvasFrame uiCanvasFrame(const DisplaySettings& display, float aspect);

// The Scene panel: draws the scene through an editor-only camera (not saved), with the
// project's design-resolution frame and an optional screen-shape preview. The UI is drawn
// over the screen area, laid out for the previewed shape (the design shape when off).
//   Mouse wheel: zoom around the cursor   Right drag: pan   F: frame the design area
//   Click: select (UI elements first, then sprites and markers; empty space deselects)
//   Left drag: move the entity or UI element under the mouse   Esc while dragging: cancel
//   Drag a corner handle of the selection: scale a sprite's entity about its centre (Shift:
//     keep its proportions), or resize a UI element with the opposite corner fixed
class SceneView {
public:
    // Input: zoom, pan, selecting and dragging. Call before drawing, after the history has
    // started watching this frame (so a drag is recorded as an edit); `allowInput` is false
    // while a dialog is open.
    void update(Scene& scene, EntityId& selected, Rectangle viewport, const DisplaySettings& display,
                bool allowInput);

    void draw(const Scene& scene, Rectangle viewport, EntityId selected, const DisplaySettings& display);

    // Zoom readout, Frame button and Preview dropdown, right-aligned in the panel header.
    void drawHeader(Rectangle header, Rectangle viewport, const DisplaySettings& display);

private:
    Camera2D camera(Rectangle viewport) const;
    void frameDesign(Rectangle viewport, const DisplaySettings& display);
    // The entity or UI element drawn topmost at `point` (scene units), or kNoEntity. The current
    // selection wins where it is under the point, so it can be dragged out from under others.
    EntityId pick(const Scene& scene, Vector2 point, EntityId selected, const UICanvasFrame& canvas) const;
    // Moves the dragged entity by `delta` (scene units) from where the drag started.
    void dragTo(Scene& scene, Vector2 delta, const UICanvasFrame& canvas) const;
    // The selection's corner handle at `point` (0..3: top-left, top-right, bottom-right,
    // bottom-left, before rotation), or -1.
    int handleAt(const Scene& scene, EntityId selected, Vector2 point, const UICanvasFrame& canvas) const;
    // Scales (entities) or resizes (UI elements) the pressed one for a handle dragged to `point`.
    void resizeTo(Scene& scene, Vector2 point, const UICanvasFrame& canvas, bool keepProportions) const;
    // Puts back the scale or size it had when the handle drag started.
    void cancelResize(Scene& scene, const UICanvasFrame& canvas) const;

    float m_zoom = 1.0f;
    Vector2 m_target{0.0f, 0.0f}; // scene point at the centre of the view
    bool m_framed = false;        // frame the design area on the first frame
    bool m_panning = false;
    int m_preview = 0; // index into the preview list; 0 = off

    // Left button: the entity pressed on, and whether it has become a drag.
    EntityId m_pressed = kNoEntity;
    bool m_dragging = false;
    Vector2 m_pressMouse{};  // window pixels
    Vector2 m_pressPoint{};  // scene units
    Vector2 m_dragStart{};   // final position (entities) or UIRectTransform position (UI elements)

    // A corner handle pressed instead (-1: none), and what the drag started from.
    int m_handle = -1;
    Vector2 m_scaleStart{};   // entities: the transform's scale
    Rectangle m_rectStart{};  // UI elements: the rectangle, in canvas units
};
