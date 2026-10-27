#pragma once

#include <raylib.h>

#include <vector>

#include "Scene.h"

class History;

// Shows the entity tree and lets the user add, select and drag entities to
// reorder or re-parent them.
class EntitiesPanel {
public:
    // `panel` is the whole panel including its header; the add button sits in the header.
    void draw(Rectangle panel, Rectangle content, Scene& scene, History& history, EntityId& selected);

private:
    // Where a dragged entity would land if dropped now.
    struct DropTarget {
        bool valid = false;
        EntityId origin = kNoEntity;
        EntityId before = kNoEntity; // kNoEntity = last among the siblings
        bool intoRow = false;             // highlight `rowRect` instead of drawing a line
        Rectangle rowRect{};
        Vector2 lineStart{};
        float lineEnd = 0.0f;
    };

    void drawList(Rectangle content, Scene& scene, EntityId& selected);
    // `area` is the list's on-screen area, including empty space below the last row.
    DropTarget findDropTarget(const Scene& scene, const std::vector<HierarchyRow>& rows, Rectangle area,
                              Vector2 mouse) const;
    void drawDragGhost(const Scene& scene, Vector2 mouse) const;

    Vector2 m_scroll{};

    EntityId m_pressed = kNoEntity; // row under the mouse when the button went down
    Vector2 m_pressPos{};
    bool m_dragging = false;
};
