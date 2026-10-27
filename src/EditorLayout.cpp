#include "EditorLayout.h"

#include <raygui.h>

#include "Theme.h"
#include "Widgets.h"

#include <algorithm>

namespace {
constexpr float kSplitterSize = 4.0f; // visible gap between panels
constexpr float kSplitterGrab = 4.0f; // extra hit area on each side of the gap
constexpr float kHeaderHeight = EditorLayout::kHeaderHeight;

// A panel: a rounded card with its caption in the header row.
void drawPanel(Rectangle r, const char* caption) {
    if (r.width <= 0.0f || r.height <= 0.0f) {
        return;
    }
    ui::fill(r, theme::kPanel, theme::kPanelRadius);
    DrawLineV({r.x + 1.0f, r.y + kHeaderHeight - 0.5f}, {r.x + r.width - 1.0f, r.y + kHeaderHeight - 0.5f},
              theme::kBorder);
    ui::caption({r.x + theme::kSpace3, r.y, r.width, kHeaderHeight}, caption);
}

constexpr float kMinSideWidth = 160.0f;
constexpr float kMinCenterWidth = 320.0f;
constexpr float kMinLogsHeight = 80.0f;
constexpr float kMinSceneHeight = 160.0f;

Rectangle expanded(Rectangle r, float dx, float dy) {
    return {r.x - dx, r.y - dy, r.width + 2.0f * dx, r.height + 2.0f * dy};
}
} // namespace

void EditorLayout::update(Rectangle area, bool allowInput) {
    m_area = area;
    clampSizes();
    computeRects();

    if (!allowInput) {
        m_hovered = Splitter::None;
        m_active = Splitter::None;
        return;
    }

    const Vector2 mouse = GetMousePosition();

    if (m_active == Splitter::None) {
        m_hovered = splitterAt(mouse);
        if (m_hovered != Splitter::None && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
            m_active = m_hovered;
            const Rectangle r = splitterRect(m_active);
            m_grabOffset = (m_active == Splitter::Logs) ? mouse.y - r.y : mouse.x - r.x;
        }
    }

    if (m_active != Splitter::None) {
        switch (m_active) {
        case Splitter::Left:
            m_leftWidth = mouse.x - m_grabOffset - m_area.x;
            break;
        case Splitter::Right:
            m_rightWidth = (m_area.x + m_area.width) - (mouse.x - m_grabOffset) - kSplitterSize;
            break;
        case Splitter::Logs:
            m_logsHeight = (m_area.y + m_area.height) - (mouse.y - m_grabOffset) - kSplitterSize;
            break;
        case Splitter::None:
            break;
        }
        clampSizes();
        computeRects();

        if (!IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
            m_active = Splitter::None;
        }
    }

    // Keep raygui controls from reacting while a splitter is being dragged.
    if (m_active != Splitter::None) {
        GuiLock();
    } else {
        GuiUnlock();
    }

    updateCursor();
}

void EditorLayout::draw() const {
    drawPanel(m_entities, "ENTITIES");
    drawPanel(m_scene, "SCENE");
    if (m_logsOpen) {
        drawPanel(m_logs, "LOGS");
    }
    drawPanel(m_details, "DETAILS");

    const Splitter highlight = (m_active != Splitter::None) ? m_active : m_hovered;
    if (highlight != Splitter::None) {
        const Rectangle r = splitterRect(highlight);
        const Rectangle line = highlight == Splitter::Logs ? Rectangle{r.x, r.y + r.height / 2.0f - 1.0f, r.width, 2.0f}
                                                           : Rectangle{r.x + r.width / 2.0f - 1.0f, r.y, 2.0f, r.height};
        DrawRectangleRec(line, Fade(theme::kAccent, m_active != Splitter::None ? 1.0f : 0.6f));
    }
}

Rectangle EditorLayout::contentOf(Rectangle panel) {
    // Inset at the bottom so content doesn't cover the panel's rounded corners.
    return {panel.x + 1.0f, panel.y + kHeaderHeight, std::max(0.0f, panel.width - 2.0f),
            std::max(0.0f, panel.height - kHeaderHeight - theme::kPanelRadius)};
}

Rectangle EditorLayout::splitterRect(Splitter s) const {
    switch (s) {
    case Splitter::Left:
        return {m_entities.x + m_entities.width, m_area.y, kSplitterSize, m_area.height};
    case Splitter::Right:
        return {m_details.x - kSplitterSize, m_area.y, kSplitterSize, m_area.height};
    case Splitter::Logs:
        return {m_scene.x, m_scene.y + m_scene.height, m_scene.width, kSplitterSize};
    case Splitter::None:
        break;
    }
    return {};
}

EditorLayout::Splitter EditorLayout::splitterAt(Vector2 point) const {
    if (m_logsOpen && CheckCollisionPointRec(point, expanded(splitterRect(Splitter::Logs), 0.0f, kSplitterGrab))) {
        return Splitter::Logs;
    }
    if (CheckCollisionPointRec(point, expanded(splitterRect(Splitter::Left), kSplitterGrab, 0.0f))) {
        return Splitter::Left;
    }
    if (CheckCollisionPointRec(point, expanded(splitterRect(Splitter::Right), kSplitterGrab, 0.0f))) {
        return Splitter::Right;
    }
    return Splitter::None;
}

void EditorLayout::clampSizes() {
    const float sideSpace = m_area.width - kMinCenterWidth - 2.0f * kSplitterSize;

    // The panel being dragged takes priority; otherwise the right panel gives way first.
    if (m_active == Splitter::Right) {
        m_rightWidth = std::clamp(m_rightWidth, kMinSideWidth, std::max(kMinSideWidth, sideSpace - m_leftWidth));
        m_leftWidth = std::clamp(m_leftWidth, kMinSideWidth, std::max(kMinSideWidth, sideSpace - m_rightWidth));
    } else {
        m_leftWidth = std::clamp(m_leftWidth, kMinSideWidth, std::max(kMinSideWidth, sideSpace - kMinSideWidth));
        m_rightWidth = std::clamp(m_rightWidth, kMinSideWidth, std::max(kMinSideWidth, sideSpace - m_leftWidth));
    }

    const float maxLogs = m_area.height - kMinSceneHeight - kSplitterSize;
    m_logsHeight = std::clamp(m_logsHeight, kMinLogsHeight, std::max(kMinLogsHeight, maxLogs));
}

void EditorLayout::computeRects() {
    const float x = m_area.x;
    const float y = m_area.y;
    const float w = m_area.width;
    const float h = m_area.height;

    m_entities = {x, y, m_leftWidth, h};
    m_details = {x + w - m_rightWidth, y, m_rightWidth, h};

    const float centerX = m_entities.x + m_entities.width + kSplitterSize;
    const float centerW = std::max(0.0f, m_details.x - kSplitterSize - centerX);
    const float sceneH = m_logsOpen ? std::max(0.0f, h - m_logsHeight - kSplitterSize) : h;

    m_scene = {centerX, y, centerW, sceneH};
    m_logs = m_logsOpen ? Rectangle{centerX, y + sceneH + kSplitterSize, centerW, m_logsHeight} : Rectangle{};
}

void EditorLayout::updateCursor() {
    const Splitter s = (m_active != Splitter::None) ? m_active : m_hovered;
    if (s == Splitter::Logs) {
        ui::requestCursor(MOUSE_CURSOR_RESIZE_NS);
    } else if (s != Splitter::None) {
        ui::requestCursor(MOUSE_CURSOR_RESIZE_EW);
    }
}
