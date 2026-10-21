#include "DetailsPanel.h"

#include "ComponentRegistry.h"
#include "Components.h"
#include "History.h"
#include "Widgets.h"

#include <raygui.h>

#include <algorithm>

namespace {
constexpr float kPadding = 8.0f;
constexpr float kHeaderHeight = 28.0f;
constexpr float kMenuItemHeight = 28.0f;
constexpr Color kRemoveColor{230, 90, 90, 255};
} // namespace

void DetailsPanel::draw(Rectangle content, Scene& scene, History& history, entt::entity& selected) {
    entt::registry& registry = scene.registry();

    if (selected != m_lastEntity) {
        m_lastEntity = selected;
        m_addMenuOpen = false;
        m_scroll = {};
    }

    if (selected == entt::null || !registry.valid(selected)) {
        ui::label({content.x + 10.0f, content.y + 6.0f, content.width - 20.0f, ui::kRowHeight},
                  "No entity selected.", ui::styleColor(TEXT_COLOR_DISABLED));
        return;
    }

    const auto types = componentTypes();
    m_collapsed.resize(types.size(), false);

    // Scroll view; reserve room for the scrollbar only when it's needed.
    const bool needsScrollbar = m_contentHeight > content.height - 2.0f;
    const float innerWidth =
        content.width - 2.0f - (needsScrollbar ? static_cast<float>(GuiGetStyle(LISTVIEW, SCROLLBAR_WIDTH)) : 0.0f);
    Rectangle view{};
    const bool wasLocked = GuiIsLocked();
    if (ui::mouseOverPopup()) {
        GuiLock(); // the mouse wheel belongs to the popup
    }
    GuiScrollPanel(content, nullptr, {0.0f, 0.0f, innerWidth, m_contentHeight}, &m_scroll, &view);
    if (!wasLocked) {
        GuiUnlock();
    }

    const Vector2 mouse = GetMousePosition();
    const bool overMenu = m_addMenuOpen && CheckCollisionPointRec(mouse, m_addMenuRect);
    ui::setInteractive(CheckCollisionPointRec(mouse, view) && !overMenu);
    ui::pushClip(view);

    const float x = view.x;
    const float w = view.width;
    const float top = view.y + m_scroll.y;
    float y = top + kPadding;

    // Name row. The roots can't be renamed or deleted; the World only has a Transform and
    // the Canvas no components at all.
    const bool isRoot = scene.isRoot(selected);
    const ComponentScope scope = scene.isUI(selected) ? ComponentScope::UI : ComponentScope::World;
    bool deleteRequested = false;
    auto& name = registry.get<NameComponent>(selected).name;
    GuiDrawIcon(entityIcon(scene, selected), static_cast<int>(x + 10.0f), static_cast<int>(y + 4.0f), 1,
                ui::styleColor(TEXT_COLOR_NORMAL));
    if (isRoot) {
        ui::label({x + 34.0f, y, w - 44.0f, ui::kFieldHeight}, name.c_str(), ui::styleColor(TEXT_COLOR_NORMAL));
        y += ui::kFieldHeight + kPadding;
        const char* about = scene.isWorld(selected) ? "Root of the scene." : "Root of the UI, laid out on the screen.";
        ui::label({x + 10.0f, y, w - 20.0f, ui::kFieldHeight}, about, ui::styleColor(TEXT_COLOR_DISABLED));
        y += ui::kFieldHeight;
    } else {
        ui::textField({x + 34.0f, y, w - 74.0f, ui::kFieldHeight}, name);
        const Rectangle deleteButton{x + w - 34.0f, y, ui::kFieldHeight, ui::kFieldHeight};
        if (ui::iconButton(deleteButton, ICON_BIN, kRemoveColor)) {
            deleteRequested = true;
        }
    }
    y += ui::kFieldHeight + kPadding * 1.5f;

    // Components
    int removeIndex = -1;
    for (std::size_t i = 0; i < types.size(); ++i) {
        const ComponentType& type = types[i];
        if ((isRoot && type.removable) || !type.has(registry, selected)) {
            continue;
        }

        const Rectangle header{x + 4.0f, y, w - 8.0f, kHeaderHeight};
        const Rectangle removeButton{header.x + header.width - 26.0f, header.y + 3.0f, 22.0f, 22.0f};
        const bool overRemove = type.removable && CheckCollisionPointRec(mouse, removeButton);

        DrawRectangleRec(header, ui::styleColor(ui::hovered(header) && !overRemove ? BASE_COLOR_FOCUSED : BASE_COLOR_NORMAL));
        DrawRectangleLinesEx(header, 1.0f, ui::styleColor(BORDER_COLOR_NORMAL));
        if (!overRemove && ui::clicked(header)) {
            m_collapsed[i] = !m_collapsed[i];
        }

        const Color text = ui::styleColor(TEXT_COLOR_NORMAL);
        const int iconY = static_cast<int>(header.y + (kHeaderHeight - 16.0f) / 2.0f);
        GuiDrawIcon(m_collapsed[i] ? ICON_ARROW_RIGHT_FILL : ICON_ARROW_DOWN_FILL, static_cast<int>(header.x + 4.0f),
                    iconY, 1, text);
        GuiDrawIcon(type.icon, static_cast<int>(header.x + 24.0f), iconY, 1, text);
        ui::label({header.x + 44.0f, header.y, header.width - 76.0f, header.height}, type.name, text);

        if (type.removable && ui::iconButton(removeButton, ICON_CROSS_SMALL, kRemoveColor)) {
            removeIndex = static_cast<int>(i);
        }
        y += kHeaderHeight + 4.0f;

        if (!m_collapsed[i]) {
            ui::PropertyGrid grid(x + 4.0f, y, w - 8.0f);
            type.drawFields(scene, selected, grid);
            y = grid.y() + 4.0f;
        }
        y += kPadding;
    }

    // Add Component
    const float buttonWidth = std::min(220.0f, w - 20.0f);
    const Rectangle addButton{x + (w - buttonWidth) / 2.0f, y + 4.0f, buttonWidth, ui::kRowHeight};
    if (!isRoot) {
        if (ui::button(addButton, GuiIconText(ICON_FILE_ADD, "Add Component"))) {
            m_addMenuOpen = !m_addMenuOpen;
        }
        y += 4.0f + ui::kRowHeight + kPadding * 2.0f;
    }

    ui::popClip();
    ui::setInteractive(true);
    m_contentHeight = y - top;

    // Apply structural changes after drawing so we never touch a removed component.
    if (deleteRequested) {
        history.destroyEntity(scene, selected);
        selected = entt::null;
        m_addMenuOpen = false;
        return;
    }
    if (removeIndex >= 0) {
        types[static_cast<std::size_t>(removeIndex)].remove(registry, selected);
    }

    if (m_addMenuOpen) {
        const int addIndex = drawAddMenu(addButton, content, registry, selected, scope);
        if (addIndex >= 0) {
            types[static_cast<std::size_t>(addIndex)].add(registry, selected);
            m_collapsed[static_cast<std::size_t>(addIndex)] = false;
            m_addMenuOpen = false;
        }
    }
}

int DetailsPanel::drawAddMenu(Rectangle anchor, Rectangle content, const entt::registry& registry,
                              entt::entity entity, ComponentScope scope) {
    const auto types = componentTypes();
    std::vector<int> available;
    for (std::size_t i = 0; i < types.size(); ++i) {
        if (types[i].removable && types[i].scope == scope && !types[i].has(registry, entity)) {
            available.push_back(static_cast<int>(i));
        }
    }

    const float itemCount = static_cast<float>(std::max<std::size_t>(available.size(), 1));
    const float height = itemCount * kMenuItemHeight + 4.0f;

    // Below the button if it fits inside the panel, otherwise above it.
    Rectangle menu{anchor.x, anchor.y + anchor.height + 2.0f, anchor.width, height};
    if (menu.y + menu.height > content.y + content.height) {
        menu.y = anchor.y - 2.0f - height;
    }
    m_addMenuRect = menu;

    DrawRectangleRec(menu, ui::styleColor(BACKGROUND_COLOR));
    DrawRectangleLinesEx(menu, 1.0f, ui::styleColor(BORDER_COLOR_FOCUSED));

    int chosen = -1;
    if (available.empty()) {
        ui::label({menu.x + 10.0f, menu.y + 2.0f, menu.width - 20.0f, kMenuItemHeight}, "All components added",
                  ui::styleColor(TEXT_COLOR_DISABLED));
    }
    for (std::size_t i = 0; i < available.size(); ++i) {
        const ComponentType& type = types[static_cast<std::size_t>(available[i])];
        const Rectangle item{menu.x + 2.0f, menu.y + 2.0f + kMenuItemHeight * static_cast<float>(i), menu.width - 4.0f,
                             kMenuItemHeight};
        const bool hot = ui::hovered(item);
        if (hot) {
            DrawRectangleRec(item, ui::styleColor(BASE_COLOR_PRESSED));
        }
        const Color text = ui::styleColor(hot ? TEXT_COLOR_PRESSED : TEXT_COLOR_NORMAL);
        GuiDrawIcon(type.icon, static_cast<int>(item.x + 8.0f), static_cast<int>(item.y + (kMenuItemHeight - 16.0f) / 2.0f),
                    1, text);
        ui::label({item.x + 32.0f, item.y, item.width - 40.0f, item.height}, type.name, text);
        if (ui::clicked(item)) {
            chosen = available[i];
        }
    }

    // Clicking anywhere else closes the menu (the Add button toggles it itself).
    if (chosen < 0 && IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && !CheckCollisionPointRec(GetMousePosition(), menu) &&
        !CheckCollisionPointRec(GetMousePosition(), anchor)) {
        m_addMenuOpen = false;
    }
    return chosen;
}
