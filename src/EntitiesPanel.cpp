#include "EntitiesPanel.h"

#include <raygui.h>
#include <raymath.h>

#include "ComponentRegistry.h"
#include "Components.h"
#include "EditorLayout.h"
#include "History.h"
#include "Theme.h"
#include "Widgets.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace {
constexpr float kRowHeight = 28.0f;
constexpr float kAddButtonSize = 24.0f;
constexpr float kHeaderHeight = EditorLayout::kHeaderHeight;
constexpr float kIndent = 18.0f;

// Flat button with a pixel-crisp "+" glyph.
bool plusButton(Rectangle bounds, const char* tooltip) {
    const bool hot = ui::hovered(bounds);
    const bool clicked = ui::button(bounds, "", ui::ButtonKind::Ghost);
    const Color color = hot ? theme::kText : theme::kTextDim;
    const float cx = std::floor(bounds.x + bounds.width / 2.0f);
    const float cy = std::floor(bounds.y + bounds.height / 2.0f);
    const float arm = 6.0f;
    DrawRectangleRec({cx - arm, cy - 1.0f, arm * 2.0f, 2.0f}, color);
    DrawRectangleRec({cx - 1.0f, cy - arm, 2.0f, arm * 2.0f}, color);
    ui::tooltip(bounds, tooltip);
    return clicked;
}
} // namespace

void EntitiesPanel::draw(Rectangle panel, Rectangle content, Scene& scene, History& history, entt::entity& selected) {
    drawList(content, scene, selected);

    // Add button, right-aligned in the panel header. Drawn last so its tooltip stays on top.
    // With the Canvas or a UI element selected it adds a UI element inside the selection;
    // otherwise an entity under the World.
    const float pad = (kHeaderHeight - kAddButtonSize) / 2.0f;
    const Rectangle addBounds{panel.x + panel.width - kAddButtonSize - 6.0f, panel.y + pad, kAddButtonSize, kAddButtonSize};
    const bool addUI = selected != entt::null && scene.registry().valid(selected) && scene.isUI(selected);
    if (plusButton(addBounds, addUI ? "Add UI element inside the selection" : "Add entity")) {
        selected = addUI ? history.createUIElement(scene, selected) : history.createEntity(scene);
        // Scroll just enough to show the new row.
        const std::vector<HierarchyRow> rows = scene.hierarchy();
        const auto it = std::find_if(rows.begin(), rows.end(), [&](const HierarchyRow& r) { return r.entity == selected; });
        const float rowTop = kRowHeight * static_cast<float>(it - rows.begin());
        m_scroll.y = std::clamp(m_scroll.y, std::min(-rowTop, content.height - kRowHeight - rowTop), -rowTop);
    }
}

void EntitiesPanel::drawList(Rectangle content, Scene& scene, entt::entity& selected) {
    const std::vector<HierarchyRow> rows = scene.hierarchy();
    const float listHeight = kRowHeight * static_cast<float>(rows.size());

    Rectangle view{};
    GuiScrollPanel(content, nullptr, {0, 0, content.width - 16.0f, listHeight}, &m_scroll, &view);

    const Vector2 mouse = GetMousePosition();
    const bool canInteract = !GuiIsLocked() && CheckCollisionPointRec(mouse, view);
    const float listTop = view.y + m_scroll.y;
    // raygui shrinks `view` to the content height; drops may also land in the empty space below.
    const Rectangle dropArea{view.x, view.y, view.width, content.y + content.height - 1.0f - view.y};

    // Press selects; moving a few pixels with the button held starts a drag.
    if (canInteract && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
        const float rel = mouse.y - listTop;
        const int i = rel < 0.0f ? -1 : static_cast<int>(rel / kRowHeight);
        if (i >= 0 && i < static_cast<int>(rows.size())) {
            selected = rows[static_cast<std::size_t>(i)].entity;
            m_pressed = scene.isRoot(selected) ? entt::null : selected; // the roots can't move
            m_pressPos = mouse;
        }
    }

    DropTarget drop;
    if (m_pressed != entt::null) {
        if (!scene.registry().valid(m_pressed) || IsKeyPressed(KEY_ESCAPE)) {
            m_pressed = entt::null;
            m_dragging = false;
        } else if (IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
            if (!m_dragging && Vector2Distance(mouse, m_pressPos) > 4.0f) {
                m_dragging = true;
            }
        } else {
            if (m_dragging) {
                const DropTarget target = findDropTarget(scene, rows, dropArea, mouse);
                if (target.valid) {
                    scene.moveEntity(m_pressed, target.origin, target.before);
                }
            }
            m_pressed = entt::null;
            m_dragging = false;
        }
    }

    if (m_dragging) {
        // Auto-scroll near the top/bottom edges.
        constexpr float kEdge = 24.0f;
        constexpr float kSpeed = 8.0f;
        if (mouse.y < dropArea.y + kEdge) {
            m_scroll.y += kSpeed;
        } else if (mouse.y > dropArea.y + dropArea.height - kEdge) {
            m_scroll.y -= kSpeed;
        }
        m_scroll.y = std::clamp(m_scroll.y, std::min(0.0f, view.height - listHeight), 0.0f);

        drop = findDropTarget(scene, rows, dropArea, mouse);
        const bool blocked = !drop.valid && CheckCollisionPointRec(mouse, dropArea);
        ui::requestCursor(blocked ? MOUSE_CURSOR_NOT_ALLOWED : MOUSE_CURSOR_DEFAULT);
    }

    ui::pushClip(dropArea);
    std::vector<float> parentBottom; // bottom edge of the most recent row at each depth
    for (std::size_t i = 0; i < rows.size(); ++i) {
        const entt::entity entity = rows[i].entity;
        const float indent = kIndent * static_cast<float>(rows[i].depth);
        const Rectangle row{view.x, view.y + m_scroll.y + kRowHeight * static_cast<float>(i), view.width,
                            kRowHeight};
        const auto depth = static_cast<std::size_t>(rows[i].depth);
        parentBottom.resize(depth + 1);
        parentBottom[depth] = row.y + kRowHeight;
        if (row.y + row.height < view.y || row.y > view.y + view.height) {
            continue;
        }

        const bool hovered = !m_dragging && canInteract && CheckCollisionPointRec(mouse, row);
        const bool isSelected = entity == selected;
        const Rectangle highlight{row.x + 4.0f, row.y + 1.0f, row.width - 8.0f, row.height - 2.0f};
        if (isSelected) {
            ui::fill(highlight, theme::kSelection);
            ui::fill({highlight.x, highlight.y + 5.0f, 2.0f, highlight.height - 10.0f}, theme::kAccent, 1.0f);
        } else if (hovered) {
            ui::fill(highlight, Color{255, 255, 255, 10});
        }

        const bool isDragged = m_dragging && entity == m_pressed;
        const Color textColor = isDragged ? theme::kTextFaint : isSelected ? WHITE : theme::kText;
        const Color iconColor = isDragged ? theme::kTextFaint : isSelected ? theme::kAccentHover : theme::kTextDim;
        // Guide line from the parent's icon down to this row.
        if (depth > 0) {
            const float guideX = row.x + indent - kIndent + 15.0f;
            DrawLineV({guideX, parentBottom[depth - 1] - 4.0f}, {guideX, row.y + kRowHeight / 2.0f}, theme::kBorder);
            DrawLineV({guideX, row.y + kRowHeight / 2.0f}, {guideX + 8.0f, row.y + kRowHeight / 2.0f}, theme::kBorder);
        }
        GuiDrawIcon(entityIcon(scene, entity), static_cast<int>(row.x + indent) + 8,
                    static_cast<int>(row.y + (kRowHeight - 16.0f) / 2.0f), 1, iconColor);
        const std::string& name = scene.registry().get<NameComponent>(entity).name;
        ui::drawText(theme::font(), name.c_str(), {row.x + indent + 32.0f, row.y + (kRowHeight - theme::kFontSize) / 2.0f},
                     theme::kFontSize, textColor);
    }

    // Drop indicator: a line between rows, or an outline around the new parent.
    if (drop.valid) {
        const Color accent = theme::kAccent;
        if (drop.intoRow) {
            DrawRectangleLinesEx(drop.rowRect, 2.0f, accent);
        } else {
            DrawLineEx(drop.lineStart, {drop.lineEnd, drop.lineStart.y}, 2.0f, accent);
            DrawCircleV(drop.lineStart, 4.0f, accent);
        }
    }
    ui::popClip();

    if (m_dragging) {
        drawDragGhost(scene, mouse);
    }
}

EntitiesPanel::DropTarget EntitiesPanel::findDropTarget(const Scene& scene, const std::vector<HierarchyRow>& rows,
                                                        Rectangle area, Vector2 mouse) const {
    DropTarget target;
    if (!CheckCollisionPointRec(mouse, area)) {
        return target;
    }

    const auto originOf = [&](entt::entity e) { return scene.parentOf(e); };
    const auto lineAt = [&](float y, int depth) {
        target.lineStart = {area.x + kIndent * static_cast<float>(depth) + 8.0f, y};
        target.lineEnd = area.x + area.width - 4.0f;
    };

    const float listTop = area.y + m_scroll.y;
    const float rel = mouse.y - listTop;
    const int index = static_cast<int>(std::floor(rel / kRowHeight));
    if (index < 0) {
        return target;
    }

    if (index >= static_cast<int>(rows.size())) {
        // Empty space below the list: last child of the dragged entity's root.
        target.origin = scene.isUI(m_pressed) ? scene.canvas() : scene.world();
        lineAt(listTop + kRowHeight * static_cast<float>(rows.size()), 1);
    } else {
        const HierarchyRow& row = rows[static_cast<std::size_t>(index)];
        const float rowTop = listTop + kRowHeight * static_cast<float>(index);
        const float frac = (rel - kRowHeight * static_cast<float>(index)) / kRowHeight;
        const bool isWorld = scene.isRoot(row.entity); // a root: nothing goes beside it
        const bool hasChildren =
            index + 1 < static_cast<int>(rows.size()) && rows[static_cast<std::size_t>(index) + 1].depth > row.depth;

        if (frac < 0.25f && !isWorld) {
            // Before this row, as its sibling.
            target.origin = originOf(row.entity);
            target.before = row.entity;
            lineAt(rowTop, row.depth);
        } else if (frac > 0.75f && hasChildren) {
            // Below a parent: becomes its first child.
            target.origin = row.entity;
            target.before = rows[static_cast<std::size_t>(index) + 1].entity;
            lineAt(rowTop + kRowHeight, row.depth + 1);
        } else if (frac > 0.75f && !isWorld) {
            // After this row, as its sibling: just before its next sibling, or last.
            target.origin = originOf(row.entity);
            const auto& order = scene.entities();
            auto it = std::find(order.begin(), order.end(), row.entity);
            for (++it; it != order.end(); ++it) {
                if (originOf(*it) == target.origin) {
                    target.before = *it;
                    break;
                }
            }
            lineAt(rowTop + kRowHeight, row.depth);
        } else {
            // Onto this row: becomes its last child.
            target.origin = row.entity;
            target.intoRow = true;
            target.rowRect = {area.x + 1.0f, rowTop + 1.0f, area.width - 2.0f, kRowHeight - 2.0f};
        }
    }

    // Dropping right next to itself is a no-op; canSetOrigin rejects itself and its followers.
    target.valid = target.before != m_pressed && scene.canSetOrigin(m_pressed, target.origin);
    return target;
}

void EntitiesPanel::drawDragGhost(const Scene& scene, Vector2 mouse) const {
    const char* name = scene.registry().get<NameComponent>(m_pressed).name.c_str();
    const float width = ui::textWidth(theme::font(), name, theme::kFontSize);
    const Rectangle box{mouse.x + 14.0f, mouse.y + 8.0f, width + 42.0f, kRowHeight};

    ui::fill({box.x - 1.0f, box.y + 1.0f, box.width + 2.0f, box.height + 2.0f}, Fade(BLACK, 0.3f));
    ui::fill(box, theme::kAccent);
    GuiDrawIcon(entityIcon(scene, m_pressed), static_cast<int>(box.x + 8.0f), static_cast<int>(box.y + (kRowHeight - 16.0f) / 2.0f), 1,
                WHITE);
    ui::drawText(theme::font(), name, {box.x + 30.0f, box.y + (kRowHeight - theme::kFontSize) / 2.0f}, theme::kFontSize,
                 WHITE);
}
