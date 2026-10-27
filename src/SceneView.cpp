#include "SceneView.h"

#include "Components.h"
#include "Theme.h"
#include "Widgets.h"

#include <raygui.h>
#include <raymath.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <utility>
#include <vector>

namespace {
constexpr Color kBackground = theme::kBackground;
constexpr Color kGridColor{30, 31, 36, 255};      // minor lines
constexpr Color kGridMajorColor{38, 40, 46, 255}; // every 4th line
constexpr Color kAxisColor{58, 61, 70, 255};
constexpr Color kFrameColor{205, 205, 215, 255};
constexpr Color kPreviewColor{240, 160, 60, 255};
constexpr Color kSelectionColor = theme::kAccent;
constexpr Color kColliderColor{90, 220, 120, 255};
constexpr Color kTriggerColor{230, 200, 80, 255};
constexpr Color kMarkerColor{150, 150, 160, 255};
constexpr Color kCanvasColor{170, 120, 230, 255};
constexpr Color kAnchorColor{240, 200, 80, 255};
constexpr float kMinZoom = 0.05f;
constexpr float kMaxZoom = 20.0f;

struct PreviewShape {
    const char* label;
    float aspect; // width / height; 0 = preview off
};
constexpr PreviewShape kPreviews[] = {
    {"Preview: off", 0.0f},          {"16:9 (1920x1080)", 16.0f / 9.0f}, {"16:10", 16.0f / 10.0f},
    {"21:9 ultrawide", 21.0f / 9.0f}, {"4:3", 4.0f / 3.0f},               {"9:16 portrait", 9.0f / 16.0f},
    {"1:1", 1.0f},
};

// Outline of a rectangle centred on `center`, rotated by `degrees`.
void drawRotatedRectLines(Vector2 center, Vector2 halfSize, float degrees, float thickness, Color color) {
    const float rad = degrees * DEG2RAD;
    const std::array<Vector2, 4> local{{
        {-halfSize.x, -halfSize.y},
        {halfSize.x, -halfSize.y},
        {halfSize.x, halfSize.y},
        {-halfSize.x, halfSize.y},
    }};
    std::array<Vector2, 4> corners{};
    for (std::size_t i = 0; i < local.size(); ++i) {
        corners[i] = Vector2Add(center, Vector2Rotate(local[i], rad));
    }
    for (std::size_t i = 0; i < corners.size(); ++i) {
        DrawLineEx(corners[i], corners[(i + 1) % corners.size()], thickness, color);
    }
}

// Grid in scene units over the visible area; spacing adapts so lines stay 24-192 px apart.
void drawGrid(Rectangle visible, float zoom) {
    float spacing = 64.0f;
    while (spacing * zoom < 24.0f) {
        spacing *= 2.0f;
    }
    while (spacing * zoom > 192.0f && spacing > 4.0f) {
        spacing /= 2.0f;
    }
    // Every 4th line is a little brighter, so the grid reads at a glance.
    const auto lineColor = [&](float at) {
        return std::llround(at / spacing) % 4 == 0 ? kGridMajorColor : kGridColor;
    };
    for (float x = std::floor(visible.x / spacing) * spacing; x < visible.x + visible.width; x += spacing) {
        DrawLineV({x, visible.y}, {x, visible.y + visible.height}, lineColor(x));
    }
    for (float y = std::floor(visible.y / spacing) * spacing; y < visible.y + visible.height; y += spacing) {
        DrawLineV({visible.x, y}, {visible.x + visible.width, y}, lineColor(y));
    }
    DrawLineV({0.0f, visible.y}, {0.0f, visible.y + visible.height}, kAxisColor);
    DrawLineV({visible.x, 0.0f}, {visible.x + visible.width, 0.0f}, kAxisColor);
}

// Scene origin after the World's transform: X axis (red) and Y axis (green). `px` = one screen pixel.
void drawWorldGizmo(const Pose& world, bool selected, float px) {
    const float rad = world.rotation * DEG2RAD;
    const float alpha = selected ? 1.0f : 0.6f;
    const Vector2 xAxis = Vector2Rotate({48.0f * world.scale.x, 0.0f}, rad);
    const Vector2 yAxis = Vector2Rotate({0.0f, 48.0f * world.scale.y}, rad);
    DrawLineEx(world.position, Vector2Add(world.position, xAxis), 2.0f * px, Fade(Color{230, 80, 80, 255}, alpha));
    DrawLineEx(world.position, Vector2Add(world.position, yAxis), 2.0f * px, Fade(Color{90, 220, 120, 255}, alpha));
    DrawCircleV(world.position, 4.0f * px, Fade(kSelectionColor, alpha));
}

void drawMarker(Vector2 p, Color color, float px) {
    DrawLineEx({p.x - 8.0f * px, p.y}, {p.x + 8.0f * px, p.y}, 2.0f * px, color);
    DrawLineEx({p.x, p.y - 8.0f * px}, {p.x, p.y + 8.0f * px}, 2.0f * px, color);
}

// Selection handles: small squares at the corners of a (rotated) rectangle. `px` = one screen pixel.
void drawHandles(Vector2 center, Vector2 halfSize, float degrees, float px) {
    const float rad = degrees * DEG2RAD;
    const float h = 3.5f * px;
    for (const Vector2 corner : {Vector2{-halfSize.x, -halfSize.y}, Vector2{halfSize.x, -halfSize.y},
                                 Vector2{halfSize.x, halfSize.y}, Vector2{-halfSize.x, halfSize.y}}) {
        const Vector2 p = Vector2Add(center, Vector2Rotate(corner, rad));
        DrawRectangleRec({p.x - h - px, p.y - h - px, 2.0f * (h + px), 2.0f * (h + px)}, kSelectionColor);
        DrawRectangleRec({p.x - h + px, p.y - h + px, 2.0f * (h - px), 2.0f * (h - px)}, kBackground);
    }
}

// A small label on a translucent pill, in screen space.
void pill(Vector2 at, const char* text, Color color) {
    constexpr float kSize = theme::kSmallFontSize + 1.0f;
    const Rectangle r{at.x, at.y, ui::textWidth(theme::font(), text, kSize) + 14.0f, 20.0f};
    ui::fill(r, Fade(theme::kRaised, 0.92f), 10.0f);
    ui::drawText(theme::font(), text, {r.x + 7.0f, r.y + (r.height - kSize) / 2.0f}, kSize, color);
}

float pillWidth(const char* text) {
    return ui::textWidth(theme::font(), text, theme::kSmallFontSize + 1.0f) + 14.0f;
}

// Darkens `visible` outside `keep` (both in scene units).
void shadeOutside(Rectangle visible, Rectangle keep, Color color) {
    const float left = keep.x;
    const float right = keep.x + keep.width;
    const float top = keep.y;
    const float bottom = keep.y + keep.height;
    const float vr = visible.x + visible.width;
    const float vb = visible.y + visible.height;
    if (top > visible.y) {
        DrawRectangleRec({visible.x, visible.y, visible.width, top - visible.y}, color);
    }
    if (bottom < vb) {
        DrawRectangleRec({visible.x, bottom, visible.width, vb - bottom}, color);
    }
    const float y0 = std::max(top, visible.y);
    const float y1 = std::min(bottom, vb);
    if (left > visible.x && y1 > y0) {
        DrawRectangleRec({visible.x, y0, left - visible.x, y1 - y0}, color);
    }
    if (right < vr && y1 > y0) {
        DrawRectangleRec({right, y0, vr - right, y1 - y0}, color);
    }
}

Rectangle centred(Vector2 size) {
    return {-size.x / 2.0f, -size.y / 2.0f, size.x, size.y};
}
} // namespace

namespace {
float canvasScale(const UICanvasFrame& canvas) {
    return canvas.frame.width / canvas.size.x;
}

// A rectangle in canvas units, placed in the scene view.
Rectangle canvasToScene(const UICanvasFrame& canvas, Rectangle r) {
    const float k = canvasScale(canvas);
    return {canvas.frame.x + r.x * k, canvas.frame.y + r.y * k, r.width * k, r.height * k};
}

// Draws `text` inside `rect`, aligned per axis, line by line (the same layout as the
// exported scene_draw_ui_text). Sizes are in the same units as `rect`.
void drawTextInRect(Font font, const std::string& text, Rectangle rect, float fontSize, float spacing, TextAlign alignH,
                    TextAlign alignV, Color color) {
    const auto fraction = [](TextAlign a) { return a == TextAlign::Start ? 0.0f : a == TextAlign::Center ? 0.5f : 1.0f; };
    std::vector<std::string> lines;
    std::size_t start = 0;
    for (std::size_t end; (end = text.find('\n', start)) != std::string::npos; start = end + 1) {
        lines.push_back(text.substr(start, end - start));
    }
    lines.push_back(text.substr(start));
    const float lineHeight = fontSize * 1.2f;
    const float blockHeight = lineHeight * static_cast<float>(lines.size() - 1) + fontSize;
    float y = rect.y + (rect.height - blockHeight) * fraction(alignV);
    for (const std::string& line : lines) {
        const float width = MeasureTextEx(font, line.c_str(), fontSize, spacing).x;
        DrawTextEx(font, line.c_str(), {rect.x + (rect.width - width) * fraction(alignH), y}, fontSize, spacing, color);
        y += lineHeight;
    }
}

// A point in sprite units (before scaling, relative to the centre) placed in the scene.
Vector2 spriteToScene(const Pose& pose, Vector2 local) {
    return Vector2Add(pose.position, Vector2Rotate(Vector2Multiply(local, pose.scale), pose.rotation * DEG2RAD));
}

// raylib only draws triangles whose corners run counter-clockwise on screen.
void drawTriangleAnyOrder(Vector2 a, Vector2 b, Vector2 c, Color color) {
    if ((b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x) > 0.0f) {
        std::swap(b, c);
    }
    DrawTriangle(a, b, c, color);
}

// The same shapes the exported scene_draw draws.
void drawSprite(const Pose& pose, const SpriteComponent& sprite) {
    const float hw = sprite.size.x / 2.0f;
    const float hh = sprite.size.y / 2.0f;
    switch (sprite.shape) {
    case SpriteShape::Circle: {
        constexpr int kSegments = 36;
        Vector2 previous = spriteToScene(pose, {hw, 0.0f});
        for (int i = 1; i <= kSegments; ++i) {
            const float angle = 2.0f * PI * static_cast<float>(i) / kSegments;
            const Vector2 next = spriteToScene(pose, {hw * std::cos(angle), hh * std::sin(angle)});
            drawTriangleAnyOrder(pose.position, previous, next, sprite.tint);
            previous = next;
        }
        break;
    }
    case SpriteShape::Triangle:
        drawTriangleAnyOrder(spriteToScene(pose, {0.0f, -hh}), spriteToScene(pose, {-hw, hh}),
                             spriteToScene(pose, {hw, hh}), sprite.tint);
        break;
    case SpriteShape::Rectangle: {
        const float w = sprite.size.x * std::fabs(pose.scale.x);
        const float h = sprite.size.y * std::fabs(pose.scale.y);
        DrawRectanglePro({pose.position.x, pose.position.y, w, h}, {w / 2.0f, h / 2.0f}, pose.rotation, sprite.tint);
        break;
    }
    }
}

bool spriteContains(const Pose& pose, const SpriteComponent& sprite, Vector2 point) {
    if (pose.scale.x == 0.0f || pose.scale.y == 0.0f || sprite.size.x <= 0.0f || sprite.size.y <= 0.0f) {
        return false;
    }
    // Back to sprite units: undo position, rotation and scale.
    const Vector2 rotated = Vector2Rotate(Vector2Subtract(point, pose.position), -pose.rotation * DEG2RAD);
    const Vector2 local{rotated.x / pose.scale.x, rotated.y / pose.scale.y};
    const float hw = sprite.size.x / 2.0f;
    const float hh = sprite.size.y / 2.0f;
    switch (sprite.shape) {
    case SpriteShape::Circle:
        return (local.x / hw) * (local.x / hw) + (local.y / hh) * (local.y / hh) <= 1.0f;
    case SpriteShape::Triangle:
        // Apex at the top centre: the half-width grows from 0 there to hw at the bottom.
        return local.y >= -hh && local.y <= hh && std::fabs(local.x) <= hw * (local.y + hh) / (2.0f * hh);
    case SpriteShape::Rectangle:
        break;
    }
    return std::fabs(local.x) <= hw && std::fabs(local.y) <= hh;
}
} // namespace

Vector2 visibleSceneArea(const DisplaySettings& display, float aspect) {
    const float w = static_cast<float>(display.width);
    const float h = static_cast<float>(display.height);
    const float designAspect = w / h;
    switch (display.scaleMode) {
    case ScaleMode::Fit:
        return {w, h};
    case ScaleMode::Fill:
        return aspect >= designAspect ? Vector2{w, w / aspect} : Vector2{h * aspect, h};
    case ScaleMode::Expand:
        break;
    }
    return aspect >= designAspect ? Vector2{h * aspect, h} : Vector2{w, w / aspect};
}

UICanvasFrame uiCanvasFrame(const DisplaySettings& display, float aspect) {
    const float w = static_cast<float>(display.width);
    const float h = static_cast<float>(display.height);
    const float designAspect = w / h;
    if (aspect <= 0.0f) {
        aspect = designAspect;
    }
    if (display.scaleMode == ScaleMode::Fit) {
        return {centred({w, h}), {w, h}};
    }
    // The screen, scaled to fit the design area inside it: canvas units match the design
    // resolution, and the screen's extra length on one axis becomes extra canvas.
    const Vector2 size = aspect >= designAspect ? Vector2{h * aspect, h} : Vector2{w, w / aspect};
    return {centred(visibleSceneArea(display, aspect)), size};
}

Camera2D SceneView::camera(Rectangle viewport) const {
    Camera2D cam{};
    cam.offset = {std::floor(viewport.x + viewport.width / 2.0f), std::floor(viewport.y + viewport.height / 2.0f)};
    cam.target = m_target;
    cam.zoom = m_zoom;
    return cam;
}

void SceneView::frameDesign(Rectangle viewport, const DisplaySettings& display) {
    const float fit = std::min(viewport.width / static_cast<float>(display.width),
                               viewport.height / static_cast<float>(display.height));
    m_zoom = std::clamp(fit * 0.9f, kMinZoom, kMaxZoom);
    m_target = {0.0f, 0.0f};
}

EntityId SceneView::pick(const Scene& scene, Vector2 point, EntityId selected, const UICanvasFrame& canvas) const {
    const float px = 1.0f / m_zoom;
    const auto hits = [&](const Entity& e) {
        if (scene.isRoot(e.id)) {
            return false;
        }
        if (e.rectTransform) {
            return CheckCollisionPointRec(point, canvasToScene(canvas, scene.uiRect(e.id, canvas.size)));
        }
        const Pose pose = scene.finalPose(e.id);
        if (e.sprite) {
            return spriteContains(pose, *e.sprite, point);
        }
        return Vector2Distance(point, pose.position) <= 8.0f * px; // marker
    };

    if (const Entity* s = scene.find(selected); s != nullptr && hits(*s)) {
        return selected;
    }
    // UI is drawn on top: last drawn first. Elements without a UI Rectangle or Label are invisible,
    // so they're only picked while selected (above) or from the Entities panel.
    const std::vector<HierarchyRow> rows = scene.hierarchy();
    for (auto it = rows.rbegin(); it != rows.rend(); ++it) {
        const Entity& e = scene.get(it->entity);
        if (e.rectTransform && (e.uiRectangle || e.uiLabel) && hits(e)) {
            return e.id;
        }
    }
    const auto& order = scene.entities();
    for (auto it = order.rbegin(); it != order.rend(); ++it) {
        if (!scene.isUI(it->id) && hits(*it)) {
            return it->id;
        }
    }
    return kNoEntity;
}

void SceneView::dragTo(Scene& scene, Vector2 delta, const UICanvasFrame& canvas) const {
    if (Entity& e = scene.get(m_pressed); e.rectTransform) {
        // The position moves the rectangle one to one, in canvas units.
        e.rectTransform->position = Vector2Add(m_dragStart, Vector2Scale(delta, 1.0f / canvasScale(canvas)));
    } else {
        scene.placeEntity(m_pressed, Vector2Add(m_dragStart, delta));
    }
}

void SceneView::update(Scene& scene, EntityId& selected, Rectangle viewport, const DisplaySettings& display,
                       bool allowInput) {
    if (!m_framed && viewport.width > 0.0f && viewport.height > 0.0f) {
        frameDesign(viewport, display);
        m_framed = true;
    }
    if (!allowInput) {
        m_panning = false;
        m_pressed = kNoEntity;
        m_dragging = false;
        return;
    }

    const Vector2 mouse = GetMousePosition();
    const bool over = CheckCollisionPointRec(mouse, viewport) && !ui::mouseOverPopup() && !GuiIsLocked();

    // Zoom around the cursor: the scene point under it stays put.
    const float wheel = GetMouseWheelMove();
    if (over && wheel != 0.0f) {
        const Vector2 before = GetScreenToWorld2D(mouse, camera(viewport));
        m_zoom = std::clamp(m_zoom * std::pow(1.15f, wheel), kMinZoom, kMaxZoom);
        const Vector2 after = GetScreenToWorld2D(mouse, camera(viewport));
        m_target = Vector2Add(m_target, Vector2Subtract(before, after));
    }

    if (over && IsMouseButtonPressed(MOUSE_BUTTON_RIGHT)) {
        m_panning = true;
    }
    if (m_panning) {
        if (IsMouseButtonDown(MOUSE_BUTTON_RIGHT)) {
            m_target = Vector2Subtract(m_target, Vector2Scale(theme::mouseDelta(), 1.0f / m_zoom));
            ui::requestCursor(MOUSE_CURSOR_RESIZE_ALL);
        } else {
            m_panning = false;
        }
    }

    if (over && IsKeyPressed(KEY_F) && !ui::isTextEditing()) {
        frameDesign(viewport, display);
    }

    // Select on press; moving a few pixels with the button held drags the pressed entity.
    const UICanvasFrame canvas = uiCanvasFrame(display, kPreviews[m_preview].aspect);
    const Vector2 point = GetScreenToWorld2D(mouse, camera(viewport));
    if (over && !m_panning && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
        selected = pick(scene, point, selected, canvas);
        m_pressed = selected;
        m_dragging = false;
        m_pressMouse = mouse;
        m_pressPoint = point;
        if (const Entity* e = scene.find(m_pressed)) {
            m_dragStart = e->rectTransform ? e->rectTransform->position : scene.finalPose(m_pressed).position;
        }
    }
    if (m_pressed == kNoEntity) {
        return;
    }
    if (!scene.contains(m_pressed) || !IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
        m_pressed = kNoEntity; // released (the history records the move as one edit)
        m_dragging = false;
        return;
    }
    if (!m_dragging && Vector2Distance(mouse, m_pressMouse) > 3.0f) {
        m_dragging = true;
    }
    if (m_dragging) {
        if (IsKeyPressed(KEY_ESCAPE)) {
            dragTo(scene, {0.0f, 0.0f}, canvas); // back where it started
            m_pressed = kNoEntity;
            m_dragging = false;
            return;
        }
        dragTo(scene, Vector2Subtract(point, m_pressPoint), canvas);
        ui::requestCursor(MOUSE_CURSOR_RESIZE_ALL);
    }
}

void SceneView::draw(const Scene& scene, Rectangle viewport, EntityId selected, const DisplaySettings& display) {
    const Camera2D cam = camera(viewport);
    const float px = 1.0f / m_zoom; // one screen pixel in scene units
    const Vector2 topLeft = GetScreenToWorld2D({viewport.x, viewport.y}, cam);
    const Vector2 bottomRight = GetScreenToWorld2D({viewport.x + viewport.width, viewport.y + viewport.height}, cam);
    const Rectangle visible{topLeft.x, topLeft.y, bottomRight.x - topLeft.x, bottomRight.y - topLeft.y};

    ui::pushClip(viewport);
    DrawRectangleRec(viewport, kBackground);
    theme::beginCamera(cam);
    drawGrid(visible, m_zoom);

    for (const Entity& e : scene.entities()) {
        if (scene.isWorld(e.id) || !e.transform) {
            continue; // the World is drawn as a gizmo below; the Canvas and UI after the scene
        }
        const Pose pose = scene.finalPose(e.id);
        if (e.sprite) {
            drawSprite(pose, *e.sprite);
        } else {
            drawMarker(pose.position, kMarkerColor, px);
        }
        if (const auto& collider = e.boxCollider) {
            const Vector2 offset = Vector2Rotate(Vector2Multiply(collider->offset, pose.scale), pose.rotation * DEG2RAD);
            const Vector2 half{collider->size.x * std::fabs(pose.scale.x) / 2.0f,
                               collider->size.y * std::fabs(pose.scale.y) / 2.0f};
            drawRotatedRectLines(Vector2Add(pose.position, offset), half, pose.rotation, 1.5f * px,
                                 collider->isTrigger ? kTriggerColor : kColliderColor);
        }
    }

    // What a player with the previewed screen shape would see; the rest is dimmed.
    const Rectangle design = centred({static_cast<float>(display.width), static_cast<float>(display.height)});
    Rectangle preview{};
    const float aspect = kPreviews[m_preview].aspect;
    if (aspect > 0.0f) {
        preview = centred(visibleSceneArea(display, aspect));
        shadeOutside(visible, preview, Fade(BLACK, 0.55f));
        DrawRectangleLinesEx(preview, 1.5f * px, kPreviewColor);
    }
    DrawRectangleLinesEx(design, 2.0f * px, Fade(kFrameColor, 0.8f));

    // UI on top of the scene, laid out on the canvas for this screen shape. Parents are
    // drawn before their children (tree order).
    const UICanvasFrame canvas = uiCanvasFrame(display, aspect);
    const auto toFrame = [&](Rectangle r) { return canvasToScene(canvas, r); };
    for (const HierarchyRow& row : scene.hierarchy()) {
        const Entity& e = scene.get(row.entity);
        const auto& rect = e.uiRectangle;
        const auto& label = e.uiLabel;
        if (!rect && !label) {
            continue;
        }
        const Rectangle r = toFrame(scene.uiRect(row.entity, canvas.size));
        if (rect) {
            DrawRectangleRec(r, rect->color);
        }
        if (label) {
            const float k = canvasScale(canvas);
            drawTextInRect(GetFontDefault(), label->text, r, label->fontSize * k, label->spacing * k, label->alignH,
                           label->alignV, label->color);
        }
    }

    const Pose worldPose = scene.finalPose(scene.world());
    const bool worldMoved = worldPose.position.x != 0.0f || worldPose.position.y != 0.0f || worldPose.rotation != 0.0f ||
                            worldPose.scale.x != 1.0f || worldPose.scale.y != 1.0f;
    if (worldMoved || scene.isWorld(selected)) {
        drawWorldGizmo(worldPose, scene.isWorld(selected), px);
    }

    // Selection outline on top of everything.
    const Entity* sel = scene.find(selected);
    if (sel != nullptr && scene.isUI(selected)) {
        // The canvas, and the selected element's rectangle and pivot.
        DrawRectangleLinesEx(canvas.frame, 1.5f * px, Fade(kCanvasColor, 0.8f));
        if (!scene.isCanvas(selected)) {
            const Rectangle r = toFrame(scene.uiRect(selected, canvas.size));
            DrawRectangleLinesEx({r.x - px, r.y - px, r.width + 2.0f * px, r.height + 2.0f * px}, 1.5f * px,
                                 kSelectionColor);
            drawHandles({r.x + r.width / 2.0f, r.y + r.height / 2.0f}, {r.width / 2.0f + px, r.height / 2.0f + px}, 0.0f,
                        px);
            const UIRectTransformComponent& t = *sel->rectTransform;

            // Anchors: a plus at each corner of the anchor rectangle on the parent (one plus
            // when pinned to a point, two when stretched along one axis). Drawn before the
            // pivot so the pivot stays visible where they coincide.
            const Rectangle parent = toFrame(scene.uiRect(scene.parentOf(selected), canvas.size));
            const float xs[2] = {parent.x + t.anchorMin.x * parent.width, parent.x + t.anchorMax.x * parent.width};
            const float ys[2] = {parent.y + t.anchorMin.y * parent.height, parent.y + t.anchorMax.y * parent.height};
            for (int i = 0; i < (xs[0] == xs[1] ? 1 : 2); ++i) {
                for (int j = 0; j < (ys[0] == ys[1] ? 1 : 2); ++j) {
                    drawMarker({xs[i], ys[j]}, kAnchorColor, px);
                }
            }
            DrawCircleV({r.x + t.pivot.x * r.width, r.y + t.pivot.y * r.height}, 4.0f * px, kSelectionColor);
        }
    } else if (sel != nullptr && !scene.isWorld(selected)) {
        const Pose pose = scene.finalPose(selected);
        if (scene.contains(sel->parent) && !scene.isWorld(sel->parent)) {
            // Link to the entity being followed.
            const Vector2 target = scene.finalPose(sel->parent).position;
            DrawLineEx(target, pose.position, 1.5f * px, Fade(kSelectionColor, 0.6f));
            DrawCircleV(target, 4.0f * px, kSelectionColor);
        }
        if (const auto& sprite = sel->sprite) {
            const Vector2 half{sprite->size.x * std::fabs(pose.scale.x) / 2.0f + px,
                               sprite->size.y * std::fabs(pose.scale.y) / 2.0f + px};
            drawRotatedRectLines(pose.position, half, pose.rotation, 1.5f * px, kSelectionColor);
            drawHandles(pose.position, half, pose.rotation, px);
        } else {
            drawMarker(pose.position, kSelectionColor, px);
        }
    }
    theme::endCamera();

    // Labels in screen space, just above each frame.
    const Vector2 designCorner = GetWorldToScreen2D({design.x, design.y}, cam);
    const std::string designLabel = std::to_string(display.width) + " x " + std::to_string(display.height) + "   " +
                                    scaleModeName(display.scaleMode);
    pill({designCorner.x, designCorner.y - 26.0f}, designLabel.c_str(), theme::kTextDim);
    if (aspect > 0.0f) {
        const Vector2 previewCorner = GetWorldToScreen2D({preview.x, preview.y + preview.height}, cam);
        pill({previewCorner.x, previewCorner.y + 6.0f}, kPreviews[m_preview].label, kPreviewColor);
    }
    if (sel != nullptr && scene.isUI(selected)) {
        const Vector2 canvasCorner = GetWorldToScreen2D({canvas.frame.x + canvas.frame.width, canvas.frame.y}, cam);
        const std::string canvasLabel = "Canvas " + std::to_string(static_cast<int>(std::lround(canvas.size.x))) + "x" +
                                        std::to_string(static_cast<int>(std::lround(canvas.size.y)));
        pill({canvasCorner.x - pillWidth(canvasLabel.c_str()), canvasCorner.y - 26.0f}, canvasLabel.c_str(),
             Fade(kCanvasColor, 0.95f));
    }

    // Zoom, in the bottom-right corner.
    const std::string zoom = std::to_string(static_cast<int>(std::lround(m_zoom * 100.0f))) + "%";
    pill({viewport.x + viewport.width - pillWidth(zoom.c_str()) - 10.0f, viewport.y + viewport.height - 30.0f},
         zoom.c_str(), theme::kTextDim);
    ui::popClip();
}

void SceneView::drawHeader(Rectangle header, Rectangle viewport, const DisplaySettings& display) {
    std::vector<std::string> labels;
    for (const PreviewShape& shape : kPreviews) {
        labels.push_back(shape.label);
    }
    constexpr float kControlHeight = 26.0f;
    const float y = header.y + (header.height - kControlHeight) / 2.0f;
    const Rectangle combo{header.x + header.width - 190.0f, y, 184.0f, kControlHeight};
    ui::combo(combo, &m_preview, labels, &m_preview);

    const Rectangle frameButton{combo.x - kControlHeight - 4.0f, y, kControlHeight, kControlHeight};
    if (ui::iconButton(frameButton, ICON_ZOOM_CENTER, theme::kText)) {
        frameDesign(viewport, display);
    }
    ui::tooltip(frameButton, "Frame the design area");
}
