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
class SceneView {
public:
    // Input: zoom, pan, selecting and dragging. Call before drawing, after the history has
    // started watching this frame (so a drag is recorded as an edit); `allowInput` is false
    // while a dialog is open.
    void update(Scene& scene, entt::entity& selected, Rectangle viewport, const DisplaySettings& display,
                bool allowInput);

    void draw(const Scene& scene, Rectangle viewport, entt::entity selected, const DisplaySettings& display);

    // Zoom readout, Frame button and Preview dropdown, right-aligned in the panel header.
    void drawHeader(Rectangle header, Rectangle viewport, const DisplaySettings& display);

private:
    Camera2D camera(Rectangle viewport) const;
    void frameDesign(Rectangle viewport, const DisplaySettings& display);
    // The entity or UI element drawn topmost at `point` (scene units), or null. The current
    // selection wins where it is under the point, so it can be dragged out from under others.
    entt::entity pick(const Scene& scene, Vector2 point, entt::entity selected, const UICanvasFrame& canvas) const;
    // Moves the dragged entity by `delta` (scene units) from where the drag started.
    void dragTo(Scene& scene, Vector2 delta, const UICanvasFrame& canvas) const;

    float m_zoom = 1.0f;
    Vector2 m_target{0.0f, 0.0f}; // scene point at the centre of the view
    bool m_framed = false;        // frame the design area on the first frame
    bool m_panning = false;
    int m_preview = 0; // index into the preview list; 0 = off

    // Left button: the entity pressed on, and whether it has become a drag.
    entt::entity m_pressed = entt::null;
    bool m_dragging = false;
    Vector2 m_pressMouse{};  // window pixels
    Vector2 m_pressPoint{};  // scene units
    Vector2 m_dragStart{};   // final position (entities) or UIRectTransform position (UI elements)
};
