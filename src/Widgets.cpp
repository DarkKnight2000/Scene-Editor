#include "Widgets.h"

#include <raygui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <vector>

// Defined in external/raygui/raygui_impl.c.
extern "C" void GuiSetTextBoxCursor(int index);

namespace ui {
namespace {

constexpr int kBufferSize = 128;
constexpr float kDragThreshold = 3.0f;

struct State {
    const void* active = nullptr; // widget being pressed / dragged / edited
    bool activeSeen = false;      // the active widget was drawn this frame
    bool editing = false;         // active widget is in text-edit mode
    bool dragging = false;
    bool selectAll = false;       // text is "selected": first keystroke replaces it
    Vector2 pressPos{};
    char buffer[kBufferSize] = {};

    const void* openColor = nullptr; // expanded color field
    bool openColorSeen = false;

    // Dropdown popup (see combo / drawPopups).
    const void* popupId = nullptr;
    bool popupSeen = false;
    Rectangle popupAnchor{};
    Rectangle popupRect{}; // last drawn rect, used to block input underneath
    std::vector<std::string> popupItems;
    int popupSelected = -1;
    float popupScroll = 0.0f;
    const void* pickedId = nullptr;
    int pickedIndex = -1;
    bool overPopup = false;

    std::vector<Rectangle> pressedControls; // controls under the mouse when the left button went down

    bool interactive = true;
    int cursor = MOUSE_CURSOR_DEFAULT;
    int appliedCursor = MOUSE_CURSOR_DEFAULT;
};

State g;
std::vector<Rectangle> g_clipStack;

void applyClip() {
    if (g_clipStack.empty()) {
        EndScissorMode();
        return;
    }
    const Rectangle& r = g_clipStack.back();
    BeginScissorMode(static_cast<int>(r.x), static_cast<int>(r.y), static_cast<int>(r.width),
                     static_cast<int>(r.height));
}

void drawFieldFrame(Rectangle r, bool hot, bool pressed) {
    const int base = pressed ? BASE_COLOR_PRESSED : hot ? BASE_COLOR_FOCUSED : BASE_COLOR_NORMAL;
    const int border = (hot || pressed) ? BORDER_COLOR_FOCUSED : BORDER_COLOR_NORMAL;
    DrawRectangleRec(r, styleColor(base));
    DrawRectangleLinesEx(r, 1.0f, styleColor(border));
}

void drawTextIn(Rectangle r, const char* text, Color color, bool center) {
    const Font font = GuiGetFont();
    const float size = static_cast<float>(GuiGetStyle(DEFAULT, TEXT_SIZE));
    const float spacing = static_cast<float>(GuiGetStyle(DEFAULT, TEXT_SPACING));
    const Vector2 extent = MeasureTextEx(font, text, size, spacing);
    const float x = center ? r.x + (r.width - extent.x) / 2.0f : r.x + 6.0f;
    const float y = r.y + (r.height - size) / 2.0f;
    pushClip(r);
    DrawTextEx(font, text, {std::floor(x), std::floor(y)}, size, spacing, color);
    popClip();
}

// Text edit session shared by dragFloat and textField. Returns +1 commit, -1 cancel, 0 continue.
bool anyKeyPressed(std::initializer_list<int> keys) {
    for (const int key : keys) {
        if (IsKeyPressed(key)) {
            return true;
        }
    }
    return false;
}

void handleSelectAll() {
    if (!g.selectAll) {
        return;
    }
    // Any key that types or deletes replaces the selection. Checked per key (not with
    // GetKeyPressed's queue) so a modifier pressed in the same frame, e.g. Shift for a
    // capital letter, doesn't hide the keystroke.
    bool typing = anyKeyPressed({KEY_BACKSPACE, KEY_DELETE});
    for (int key = KEY_SPACE; key <= KEY_GRAVE && !typing; ++key) { // printable keys
        typing = IsKeyPressed(key);
    }
    for (int key = KEY_KP_0; key <= KEY_KP_EQUAL && !typing; ++key) { // keypad
        typing = key != KEY_KP_ENTER && IsKeyPressed(key);
    }
    if (typing) {
        g.buffer[0] = '\0';
        GuiSetTextBoxCursor(0);
        g.selectAll = false;
    } else if (anyKeyPressed({KEY_LEFT, KEY_RIGHT, KEY_HOME, KEY_END}) || IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
        g.selectAll = false;
    }
}

int runTextEdit(Rectangle bounds) {
    handleSelectAll();

    // Keyboard input must keep working even when the mouse leaves the scroll view.
    const bool wasLocked = GuiIsLocked();
    GuiUnlock();
    GuiTextBox(bounds, g.buffer, kBufferSize - 1, true);
    if (wasLocked) {
        GuiLock();
    }

    if (g.selectAll && g.buffer[0] != '\0') {
        const float padding = static_cast<float>(GuiGetStyle(TEXTBOX, TEXT_PADDING) + GuiGetStyle(TEXTBOX, BORDER_WIDTH));
        const float width = static_cast<float>(GuiGetTextWidth(g.buffer));
        DrawRectangleRec({bounds.x + padding, bounds.y + 3.0f, std::min(width, bounds.width - 2.0f * padding),
                          bounds.height - 6.0f},
                         Fade(styleColor(BORDER_COLOR_FOCUSED), 0.35f));
    }

    if (IsKeyPressed(KEY_ESCAPE)) {
        return -1;
    }
    if (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER) || IsKeyPressed(KEY_TAB)) {
        return 1;
    }
    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && !CheckCollisionPointRec(GetMousePosition(), bounds)) {
        return 1;
    }
    return 0;
}

void startTextEdit(const void* id, const char* text, bool selectAll) {
    g.active = id;
    g.editing = true;
    g.dragging = false;
    g.selectAll = selectAll;
    std::snprintf(g.buffer, sizeof(g.buffer), "%s", text);
    GuiSetTextBoxCursor(static_cast<int>(std::strlen(g.buffer)));
}

void clearActive() {
    g.active = nullptr;
    g.editing = false;
    g.dragging = false;
}

// Saves and restores raygui's lock so raygui controls respect `interactive`.
class InteractionGuard {
public:
    InteractionGuard() : m_wasLocked(GuiIsLocked()) {
        if (!g.interactive || g.overPopup) {
            GuiLock();
        }
    }
    ~InteractionGuard() {
        if (!m_wasLocked) {
            GuiUnlock();
        }
    }

private:
    bool m_wasLocked;
};

} // namespace

void pushClip(Rectangle r) {
    if (!g_clipStack.empty()) {
        const Rectangle& top = g_clipStack.back();
        const float x0 = std::max(r.x, top.x);
        const float y0 = std::max(r.y, top.y);
        const float x1 = std::min(r.x + r.width, top.x + top.width);
        const float y1 = std::min(r.y + r.height, top.y + top.height);
        r = {x0, y0, std::max(0.0f, x1 - x0), std::max(0.0f, y1 - y0)};
    }
    g_clipStack.push_back(r);
    applyClip();
}

void popClip() {
    if (!g_clipStack.empty()) {
        g_clipStack.pop_back();
    }
    applyClip();
}

bool hovered(Rectangle r) {
    return g.interactive && !g.overPopup && !GuiIsLocked() && CheckCollisionPointRec(GetMousePosition(), r);
}

bool clicked(Rectangle r) {
    return hovered(r) && IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && !g.active;
}

void label(Rectangle r, const char* text, Color color, bool center) {
    drawTextIn(r, text, color, center);
}

bool pressStartedOn(Rectangle r) {
    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && hovered(r)) { // hovered: unlocked, not under a popup
        g.pressedControls.push_back(r);
    }
    // Exact comparison on purpose: a control that moved since the press is a different place.
    return std::any_of(g.pressedControls.begin(), g.pressedControls.end(), [&](const Rectangle& p) {
        return p.x == r.x && p.y == r.y && p.width == r.width && p.height == r.height;
    });
}

bool button(Rectangle bounds, const char* text) {
    InteractionGuard guard;
    const bool pressedHere = pressStartedOn(bounds);
    return GuiButton(bounds, text) != 0 && pressedHere && !g.active;
}

bool windowBox(Rectangle bounds, const char* title) {
    const bool pressedHere = pressStartedOn(bounds);
    return GuiWindowBox(bounds, title) != 0 && pressedHere;
}

bool iconButton(Rectangle bounds, int icon, Color hoverColor) {
    const bool hot = hovered(bounds);
    if (hot) {
        DrawRectangleRec(bounds, styleColor(BASE_COLOR_FOCUSED));
    }
    const Color color = hot ? hoverColor : styleColor(TEXT_COLOR_NORMAL);
    GuiDrawIcon(icon, static_cast<int>(bounds.x + (bounds.width - 16.0f) / 2.0f),
                static_cast<int>(bounds.y + (bounds.height - 16.0f) / 2.0f), 1, color);
    return hot && IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && !g.active;
}

Color styleColor(int property) {
    return GetColor(static_cast<unsigned int>(GuiGetStyle(DEFAULT, property)));
}

void beginFrame() {
    g.activeSeen = false;
    g.openColorSeen = false;
    g.popupSeen = false;
    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
        g.pressedControls.clear();
    }
    g.overPopup = g.popupId && CheckCollisionPointRec(GetMousePosition(), g.popupRect);
    g.interactive = true;
    g.cursor = MOUSE_CURSOR_DEFAULT;
}

void endFrame() {
    // The active widget disappeared (selection changed, component removed...): drop it.
    if (g.active && !g.activeSeen) {
        clearActive();
    }
    if (!g.openColorSeen) {
        g.openColor = nullptr;
    }
    if (g.popupId && !g.popupSeen) {
        g.popupId = nullptr;
    }
    if (g.cursor != g.appliedCursor) {
        SetMouseCursor(g.cursor);
        g.appliedCursor = g.cursor;
    }
}

void requestCursor(int cursor) {
    g.cursor = cursor;
}

void setInteractive(bool interactive) {
    g.interactive = interactive;
}

bool isInteractive() {
    return g.interactive;
}

bool isTextEditing() {
    return g.active && g.editing;
}

bool dragFloat(Rectangle bounds, float* value, float speed, const char* prefix, const char* format) {
    const void* id = value;
    const bool isActive = g.active == id;
    if (isActive) {
        g.activeSeen = true;
    }

    if (isActive && g.editing) {
        const int result = runTextEdit(bounds);
        if (result != 0) {
            const float old = *value;
            if (result > 0) {
                char* end = nullptr;
                const float parsed = std::strtof(g.buffer, &end);
                if (end != g.buffer && std::isfinite(parsed)) {
                    *value = parsed;
                }
            }
            clearActive();
            return *value != old;
        }
        return false;
    }

    bool changed = false;
    const bool hot = hovered(bounds);

    if (isActive) {
        const Vector2 mouse = GetMousePosition();
        if (IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
            if (!g.dragging && std::fabs(mouse.x - g.pressPos.x) > kDragThreshold) {
                g.dragging = true;
            }
            if (g.dragging) {
                float step = speed;
                if (IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT)) {
                    step *= 0.1f;
                }
                if (IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL)) {
                    step *= 10.0f;
                }
                const float delta = GetMouseDelta().x * step;
                if (delta != 0.0f) {
                    *value += delta;
                    changed = true;
                }
            }
        } else if (!g.dragging) {
            startTextEdit(id, TextFormat("%g", *value), true); // plain number, no units
            return false;
        } else {
            clearActive();
        }
    } else if (hot && IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && !g.active) {
        g.active = id;
        g.activeSeen = true;
        g.editing = false;
        g.dragging = false;
        g.pressPos = GetMousePosition();
    }

    if (hot || (g.active == id && g.dragging)) {
        requestCursor(MOUSE_CURSOR_RESIZE_EW);
    }

    const bool pressed = g.active == id;
    drawFieldFrame(bounds, hot, pressed);
    const char* text = prefix ? TextFormat("%s  %s", prefix, TextFormat(format, *value)) : TextFormat(format, *value);
    drawTextIn(bounds, text, styleColor(pressed ? TEXT_COLOR_PRESSED : hot ? TEXT_COLOR_FOCUSED : TEXT_COLOR_NORMAL),
               true);
    return changed;
}

bool textField(Rectangle bounds, std::string& text) {
    const void* id = &text;
    if (g.active == id) {
        g.activeSeen = true;
        if (g.editing) {
            const int result = runTextEdit(bounds);
            if (result != 0) {
                const bool changed = result > 0 && text != g.buffer;
                if (changed) {
                    text = g.buffer;
                }
                clearActive();
                return changed;
            }
            return false;
        }
    }

    const bool hot = hovered(bounds);
    if (hot) {
        requestCursor(MOUSE_CURSOR_IBEAM);
        if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && !g.active) {
            startTextEdit(id, text.c_str(), false);
            g.activeSeen = true;
        }
    }

    drawFieldFrame(bounds, hot, false);
    drawTextIn(bounds, text.c_str(), styleColor(hot ? TEXT_COLOR_FOCUSED : TEXT_COLOR_NORMAL), false);
    return false;
}

void focusTextField(std::string& text) {
    startTextEdit(&text, text.c_str(), true);
    g.activeSeen = true;
}

bool checkbox(Rectangle bounds, bool* checked) {
    InteractionGuard guard;
    const float size = std::min(bounds.height, 20.0f);
    const Rectangle box{bounds.x, bounds.y + (bounds.height - size) / 2.0f, size, size};
    const bool before = *checked;
    const bool pressedHere = pressStartedOn(box);
    GuiCheckBox(box, nullptr, checked);
    if (*checked != before && !pressedHere) {
        *checked = before; // released here, but pressed somewhere else
    }
    return *checked != before;
}

float colorField(Rectangle bounds, Color* color) {
    const void* id = color;
    const bool open = g.openColor == id;
    const bool hot = hovered(bounds);

    // Expanded layout: saturation/value square + hue bar, then an alpha bar.
    const float gap = 6.0f;
    const float hueSpace = static_cast<float>(GuiGetStyle(COLORPICKER, HUEBAR_WIDTH) +
                                              GuiGetStyle(COLORPICKER, HUEBAR_PADDING));
    const float pickerSize = std::max(40.0f, std::min(160.0f, bounds.width - hueSpace - 4.0f));
    const Rectangle picker{bounds.x, bounds.y + bounds.height + gap, pickerSize, pickerSize};
    const Rectangle alphaBar{bounds.x, picker.y + picker.height + gap, pickerSize + hueSpace, 16.0f};
    const Rectangle expanded{bounds.x, bounds.y, std::max(bounds.width, alphaBar.width),
                             alphaBar.y + alphaBar.height - bounds.y};

    if (hot && IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && !g.active) {
        g.openColor = open ? nullptr : id;
    } else if (open && ((IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && !CheckCollisionPointRec(GetMousePosition(), expanded)) ||
                        (IsKeyPressed(KEY_ESCAPE) && !isTextEditing()))) {
        // Click anywhere else (or Esc) closes the picker.
        g.openColor = nullptr;
    }

    // Swatch: checkerboard behind the color so alpha is visible.
    DrawRectangleRec(bounds, LIGHTGRAY);
    for (float x = bounds.x; x < bounds.x + bounds.width; x += 8.0f) {
        for (float y = bounds.y; y < bounds.y + bounds.height; y += 8.0f) {
            if ((static_cast<int>((x - bounds.x) / 8.0f) + static_cast<int>((y - bounds.y) / 8.0f)) % 2 == 0) {
                DrawRectangleRec({x, y, std::min(8.0f, bounds.x + bounds.width - x),
                                  std::min(8.0f, bounds.y + bounds.height - y)},
                                 GRAY);
            }
        }
    }
    DrawRectangleRec(bounds, *color);
    DrawRectangleLinesEx(bounds, 1.0f, styleColor(hot || open ? BORDER_COLOR_FOCUSED : BORDER_COLOR_NORMAL));
    const Color label = (color->r * 299 + color->g * 587 + color->b * 114) / 1000 > 140 || color->a < 100 ? BLACK : WHITE;
    drawTextIn(bounds, TextFormat("#%02X%02X%02X%02X", color->r, color->g, color->b, color->a), label, true);

    if (g.openColor != id) {
        return bounds.height;
    }
    g.openColorSeen = true;

    InteractionGuard guard;
    GuiColorPicker(picker, nullptr, color);

    float alpha = static_cast<float>(color->a) / 255.0f;
    GuiColorBarAlpha(alphaBar, nullptr, &alpha);
    color->a = static_cast<unsigned char>(std::lround(alpha * 255.0f));

    return (alphaBar.y + alphaBar.height) - bounds.y;
}

bool mouseOverPopup() {
    return g.overPopup;
}

bool combo(Rectangle bounds, const void* id, const std::vector<std::string>& items, int* selected) {
    bool changed = false;
    if (g.pickedId == id) {
        changed = g.pickedIndex != *selected;
        *selected = g.pickedIndex;
        g.pickedId = nullptr;
        g.pickedIndex = -1;
    }

    const bool open = g.popupId == id;
    const bool hot = hovered(bounds);
    if (clicked(bounds)) {
        if (open) {
            g.popupId = nullptr;
        } else {
            g.popupId = id;
            g.popupScroll = 0.0f;
        }
    }
    if (g.popupId == id) {
        g.popupSeen = true;
        g.popupAnchor = bounds;
        g.popupItems = items;
        g.popupSelected = *selected;
    }

    drawFieldFrame(bounds, hot, g.popupId == id);
    const char* text = (*selected >= 0 && *selected < static_cast<int>(items.size())) ? items[*selected].c_str() : "";
    const Color color = styleColor(hot ? TEXT_COLOR_FOCUSED : TEXT_COLOR_NORMAL);
    drawTextIn({bounds.x, bounds.y, bounds.width - 22.0f, bounds.height}, text, color, false);
    GuiDrawIcon(g.popupId == id ? ICON_ARROW_UP_FILL : ICON_ARROW_DOWN_FILL, static_cast<int>(bounds.x + bounds.width - 20.0f),
                static_cast<int>(bounds.y + (bounds.height - 16.0f) / 2.0f), 1, color);
    return changed;
}

void drawPopups() {
    if (!g.popupId || !g.popupSeen) {
        return;
    }
    constexpr float kItemHeight = 26.0f;
    constexpr int kMaxVisible = 12;

    const int count = static_cast<int>(g.popupItems.size());
    const float height = static_cast<float>(std::clamp(count, 1, kMaxVisible)) * kItemHeight + 4.0f;
    const Rectangle anchor = g.popupAnchor;
    Rectangle rect{anchor.x, anchor.y + anchor.height + 2.0f, anchor.width, height};
    if (rect.y + rect.height > static_cast<float>(GetScreenHeight())) {
        rect.y = anchor.y - 2.0f - height;
    }
    g.popupRect = rect;

    const Vector2 mouse = GetMousePosition();
    const bool over = CheckCollisionPointRec(mouse, rect);
    const float maxScroll = std::max(0.0f, static_cast<float>(count) * kItemHeight + 4.0f - height);
    if (over) {
        g.popupScroll = std::clamp(g.popupScroll - GetMouseWheelMove() * kItemHeight * 2.0f, 0.0f, maxScroll);
    }

    DrawRectangleRec(rect, styleColor(BACKGROUND_COLOR));
    DrawRectangleLinesEx(rect, 1.0f, styleColor(BORDER_COLOR_FOCUSED));
    pushClip({rect.x + 1.0f, rect.y + 1.0f, rect.width - 2.0f, rect.height - 2.0f});
    for (int i = 0; i < count; ++i) {
        const Rectangle item{rect.x + 2.0f, rect.y + 2.0f + static_cast<float>(i) * kItemHeight - g.popupScroll,
                             rect.width - 4.0f, kItemHeight};
        if (item.y + item.height < rect.y || item.y > rect.y + rect.height) {
            continue;
        }
        const bool hot = over && !GuiIsLocked() && CheckCollisionPointRec(mouse, item);
        if (hot) {
            DrawRectangleRec(item, styleColor(BASE_COLOR_PRESSED));
        } else if (i == g.popupSelected) {
            DrawRectangleRec(item, styleColor(BASE_COLOR_FOCUSED));
        }
        drawTextIn(item, g.popupItems[static_cast<std::size_t>(i)].c_str(),
                   styleColor(hot ? TEXT_COLOR_PRESSED : TEXT_COLOR_NORMAL), false);
        if (hot && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
            g.pickedId = g.popupId;
            g.pickedIndex = i;
        }
    }
    popClip();

    const bool picked = g.pickedId != nullptr;
    const bool clickedAway = IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && !over && !CheckCollisionPointRec(mouse, anchor);
    if (picked || clickedAway || IsKeyPressed(KEY_ESCAPE)) {
        g.popupId = nullptr;
    }
}

PropertyGrid::PropertyGrid(float x, float y, float width) : m_x(x), m_y(y), m_width(width) {}

Rectangle PropertyGrid::row(const char* label, float height) {
    constexpr float kPadding = 10.0f;
    const float labelWidth = std::clamp(m_width * 0.38f, 70.0f, 140.0f);
    const float rowHeight = std::max(height, kFieldHeight) + (kRowHeight - kFieldHeight);

    const Rectangle labelRect{m_x + kPadding, m_y, labelWidth, rowHeight};
    drawTextIn(labelRect, label, styleColor(TEXT_COLOR_NORMAL), false);

    const float valueX = m_x + kPadding + labelWidth;
    const Rectangle value{valueX, m_y + (rowHeight - height) / 2.0f, std::max(20.0f, m_x + m_width - kPadding - valueX),
                          height};
    m_y += rowHeight;
    return value;
}

void splitHalves(Rectangle r, Rectangle& left, Rectangle& right) {
    constexpr float kGap = 4.0f;
    const float w = (r.width - kGap) / 2.0f;
    left = {r.x, r.y, w, r.height};
    right = {r.x + w + kGap, r.y, w, r.height};
}

} // namespace ui
