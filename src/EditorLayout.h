#pragma once

#include <raylib.h>

// Editor layout: three columns (entities | scene + logs | details) separated by
// splitters that can be dragged with the mouse.
//
//   +-----------+---------------------------+-----------+
//   |           |                           |           |
//   | Entities  |          Scene            |  Details  |
//   |           |                           |           |
//   |           +---------------------------+           |
//   |           |     Logs (when open)      |           |
//   +-----------+---------------------------+-----------+
class EditorLayout {
public:
    // Height of each panel's header row (caption and the panel's own controls).
    static constexpr float kHeaderHeight = 34.0f;

    // Recomputes panel rectangles to fill `area` and, if `allowInput`, handles splitter dragging.
    void update(Rectangle area, bool allowInput = true);

    // Draws the panel frames and splitter highlights.
    void draw() const;

    // Panel sizes, for saving and restoring the layout. Out-of-range values are clamped
    // on the next update().
    float entitiesWidth() const { return m_leftWidth; }
    float detailsWidth() const { return m_rightWidth; }
    float logsHeight() const { return m_logsHeight; }
    void setSizes(float entitiesWidth, float detailsWidth, float logsHeight) {
        m_leftWidth = entitiesWidth;
        m_rightWidth = detailsWidth;
        m_logsHeight = logsHeight;
    }

    // The Logs panel can be closed; the Scene panel then takes its place.
    bool logsOpen() const { return m_logsOpen; }
    void setLogsOpen(bool open) { m_logsOpen = open; }

    // Whole panel rectangles, header included.
    Rectangle entitiesBounds() const { return m_entities; }
    Rectangle sceneBounds() const { return m_scene; }
    Rectangle logsBounds() const { return m_logs; }

    // Content areas below each panel's header.
    Rectangle entitiesContent() const { return contentOf(m_entities); }
    Rectangle detailsContent() const { return contentOf(m_details); }
    Rectangle sceneContent() const { return contentOf(m_scene); }
    Rectangle logsContent() const { return contentOf(m_logs); }

    // True while a splitter is hovered or dragged; the editor should ignore
    // viewport input in that case.
    bool isInteracting() const { return m_hovered != Splitter::None || m_active != Splitter::None; }

private:
    enum class Splitter { None, Left, Right, Logs };

    static Rectangle contentOf(Rectangle panel);
    Rectangle splitterRect(Splitter s) const;
    Splitter splitterAt(Vector2 point) const;
    void clampSizes();
    void computeRects();
    void updateCursor();

    // Panel sizes in pixels; the center column takes the remaining width.
    float m_leftWidth = 300.0f;
    float m_rightWidth = 340.0f;
    float m_logsHeight = 220.0f;
    bool m_logsOpen = false;

    Rectangle m_area{};
    Rectangle m_entities{};
    Rectangle m_details{};
    Rectangle m_scene{};
    Rectangle m_logs{};

    Splitter m_hovered = Splitter::None;
    Splitter m_active = Splitter::None;
    float m_grabOffset = 0.0f; // mouse offset inside the splitter when the drag started
};
