#include "Widgets.h"

#include "Theme.h"

#include <raygui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <string>
#include <vector>

// Defined in subprojects/raylib/raygui_impl.c.
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

    // Tooltip: shown once the mouse rests on the same control for a moment.
    Rectangle tipRect{};
    double tipSince = 0.0;
    std::string tipText;
    bool tipSeen = false;
};

State g;
std::vector<Rectangle> g_clipStack;

void applyClip() {
    if (g_clipStack.empty()) {
        EndScissorMode();
        return;
    }
    const Rectangle r = theme::toPixels(g_clipStack.back());
    BeginScissorMode(static_cast<int>(std::floor(r.x)), static_cast<int>(std::floor(r.y)),
                     static_cast<int>(std::ceil(r.width)), static_cast<int>(std::ceil(r.height)));
}

// Fields: a flat fill that lightens on hover; an accent outline while in use.
void drawFieldFrame(Rectangle r, bool hot, bool pressed) {
    fill(r, hot || pressed ? theme::kFieldHover : theme::kField);
    if (pressed) {
        outline(r, theme::kAccent);
    }
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

// "#12#Save" (raygui's GuiIconText) -> icon 12, and `text` moves past the prefix.
int splitIcon(const char*& text) {
    if (text == nullptr || text[0] != '#') {
        return -1;
    }
    char* end = nullptr;
    const long icon = std::strtol(text + 1, &end, 10);
    if (end == text + 1 || *end != '#') {
        return -1;
    }
    text = end + 1;
    return static_cast<int>(icon);
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
                         Fade(theme::kAccent, 0.35f));
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

void drawTooltip() {
    if (!g.tipSeen || GetTime() - g.tipSince < 0.45) {
        return;
    }
    const float w = textWidth(theme::font(), g.tipText.c_str(), theme::kFontSize) + 2.0f * theme::kSpace2;
    const float h = 26.0f;
    Rectangle r{g.tipRect.x + (g.tipRect.width - w) / 2.0f, g.tipRect.y + g.tipRect.height + 6.0f, w, h};
    r.x = std::clamp(r.x, 4.0f, std::max(4.0f, theme::screenWidth() - w - 4.0f));
    if (r.y + h > theme::screenHeight()) {
        r.y = g.tipRect.y - h - 6.0f;
    }
    fill({r.x - 1.0f, r.y + 1.0f, r.width + 2.0f, r.height + 2.0f}, Fade(BLACK, 0.3f));
    fill(r, Color{54, 57, 65, 255});
    drawText(theme::font(), g.tipText.c_str(), {r.x + theme::kSpace2, r.y + (h - theme::kFontSize) / 2.0f},
             theme::kFontSize, theme::kText);
}

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

bool button(Rectangle bounds, const char* text, ButtonKind kind) {
    const bool disabled = GuiGetState() == STATE_DISABLED;
    const bool hot = !disabled && hovered(bounds);
    const bool pressedHere = pressStartedOn(bounds);
    const bool down = hot && pressedHere && IsMouseButtonDown(MOUSE_BUTTON_LEFT);

    Color base = theme::kField;
    Color color = theme::kText;
    switch (kind) {
    case ButtonKind::Primary:
        base = down ? theme::kAccentPressed : hot ? theme::kAccentHover : theme::kAccent;
        color = WHITE;
        break;
    case ButtonKind::Ghost:
        base = down ? theme::kField : hot ? theme::kFieldHover : Color{0, 0, 0, 0};
        break;
    case ButtonKind::Normal:
        base = down ? theme::kField : hot ? theme::kFieldHover : theme::kField;
        break;
    }
    if (disabled) {
        color = theme::kTextFaint;
        if (kind == ButtonKind::Primary) {
            base = theme::kField;
        }
    }
    if (base.a > 0) {
        fill(bounds, base);
    }

    // Icon and text, centred together.
    const int icon = splitIcon(text);
    const bool hasText = text != nullptr && text[0] != '\0';
    const float textW = hasText ? textWidth(theme::font(), text, theme::kFontSize) : 0.0f;
    const float iconW = icon >= 0 ? 16.0f : 0.0f;
    const float gap = icon >= 0 && hasText ? 6.0f : 0.0f;
    float x = std::floor(bounds.x + (bounds.width - (iconW + gap + textW)) / 2.0f);
    if (icon >= 0) {
        const Color iconColor = kind == ButtonKind::Primary || disabled ? color : hot ? theme::kText : theme::kTextDim;
        GuiDrawIcon(icon, static_cast<int>(x), static_cast<int>(bounds.y + (bounds.height - 16.0f) / 2.0f), 1, iconColor);
        x += iconW + gap;
    }
    if (hasText) {
        drawText(theme::font(), text, {x, bounds.y + (bounds.height - theme::kFontSize) / 2.0f}, theme::kFontSize, color);
    }
    return hot && pressedHere && IsMouseButtonReleased(MOUSE_BUTTON_LEFT) && !g.active;
}

bool windowBox(Rectangle bounds, const char* title) {
    // A card with a soft shadow, a title and a close button.
    for (int i = 1; i <= 4; ++i) {
        const float d = static_cast<float>(i) * 3.0f;
        fill({bounds.x - d, bounds.y - d + 4.0f, bounds.width + 2.0f * d, bounds.height + 2.0f * d}, Fade(BLACK, 0.06f),
             theme::kPanelRadius + d);
    }
    fill(bounds, theme::kPanel, theme::kPanelRadius);
    outline(bounds, theme::kBorder, theme::kPanelRadius);
    drawText(theme::bold(), title, {bounds.x + theme::kSpace4, bounds.y + 12.0f}, theme::kFontSize, theme::kText);
    const Rectangle close{bounds.x + bounds.width - 34.0f, bounds.y + 6.0f, 26.0f, 26.0f};
    return iconButton(close, ICON_CROSS_SMALL, theme::kText);
}

bool iconButton(Rectangle bounds, int icon, Color hoverColor) {
    const bool hot = hovered(bounds);
    if (hot) {
        fill(bounds, theme::kFieldHover);
    }
    GuiDrawIcon(icon, static_cast<int>(bounds.x + (bounds.width - 16.0f) / 2.0f),
                static_cast<int>(bounds.y + (bounds.height - 16.0f) / 2.0f), 1, hot ? hoverColor : theme::kTextDim);
    return hot && IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && !g.active;
}

bool toggle(Rectangle bounds, const char* text, bool* value) {
    const bool hot = hovered(bounds);
    const bool pressedHere = pressStartedOn(bounds);
    const bool changed = hot && pressedHere && IsMouseButtonReleased(MOUSE_BUTTON_LEFT) && !g.active;
    if (changed) {
        *value = !*value;
    }
    fill(bounds, *value ? Fade(theme::kAccent, hot ? 0.38f : 0.28f) : hot ? theme::kFieldHover : theme::kField);
    if (*value) {
        outline(bounds, Fade(theme::kAccent, 0.8f));
    }
    const float w = textWidth(theme::font(), text, theme::kFontSize);
    drawText(theme::font(), text,
             {bounds.x + (bounds.width - w) / 2.0f, bounds.y + (bounds.height - theme::kFontSize) / 2.0f},
             theme::kFontSize, *value ? WHITE : theme::kTextDim);
    return changed;
}

void tooltip(Rectangle r, const char* text) {
    if (!hovered(r) || IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
        return;
    }
    if (r.x != g.tipRect.x || r.y != g.tipRect.y || r.width != g.tipRect.width || r.height != g.tipRect.height ||
        g.tipText != text) {
        g.tipRect = r;
        g.tipSince = GetTime();
    }
    g.tipText = text;
    g.tipSeen = true;
}

void fill(Rectangle r, Color color, float radius) {
    const float side = std::min(r.width, r.height);
    if (radius <= 0.0f || side <= 0.0f) {
        DrawRectangleRec(r, color);
        return;
    }
    DrawRectangleRounded(r, std::min(1.0f, 2.0f * radius / side), 6, color);
}

void outline(Rectangle r, Color color, float radius, float thickness) {
    const float side = std::min(r.width, r.height);
    if (radius <= 0.0f || side <= 0.0f) {
        DrawRectangleLinesEx(r, thickness, color);
        return;
    }
    DrawRectangleRoundedLinesEx(r, std::min(1.0f, 2.0f * radius / side), 6, thickness, color);
}

void drawText(Font font, const char* text, Vector2 position, float size, Color color) {
    DrawTextEx(font, text, {std::floor(position.x), std::floor(position.y)}, size, 0.0f, color);
}

float textWidth(Font font, const char* text, float size) {
    return MeasureTextEx(font, text, size, 0.0f).x;
}

void caption(Rectangle r, const char* text) {
    drawText(theme::bold(), text, {r.x, r.y + (r.height - theme::kSmallFontSize) / 2.0f}, theme::kSmallFontSize,
             theme::kTextDim);
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
    g.tipSeen = false;
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

namespace {
// While dragging a value: at the left or right edge of the screen (the monitor the editor is
// on), the cursor jumps to the other edge, so the drag can go on as far as needed. Positions
// keep arriving outside the window because GLFW captures the mouse while a button is down.
// raylib's SetMousePosition also resets the mouse delta, so the jump itself doesn't count.
void wrapCursorAtScreenEdges() {
    constexpr float kMargin = 2.0f; // pixels from the edge that count as reaching it
    const int monitor = GetCurrentMonitor();
    const Vector2 monitorPos = GetMonitorPosition(monitor);
    const float left = monitorPos.x;
    const float right = monitorPos.x + static_cast<float>(GetMonitorWidth(monitor)) - 1.0f;
    const Vector2 window = GetWindowPosition();
    const float mouseX = GetMousePosition().x * theme::uiScale(); // window pixels
    const float mouseY = GetMousePosition().y * theme::uiScale();
    const float screenX = window.x + mouseX;
    float to = screenX;
    if (screenX <= left + kMargin) {
        to = right - kMargin - 1.0f;
    } else if (screenX >= right - kMargin) {
        to = left + kMargin + 1.0f;
    }
    if (to != screenX) {
        SetMousePosition(static_cast<int>(to - window.x), static_cast<int>(mouseY));
    }
}
} // namespace

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
                const float delta = theme::mouseDelta().x * step;
                if (delta != 0.0f) {
                    *value += delta;
                    changed = true;
                }
                wrapCursorAtScreenEdges();
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
    float textX = bounds.x;
    if (prefix != nullptr) {
        // Axis letters in the usual colours: X red, Y green.
        const Color prefixColor = std::strcmp(prefix, "X") == 0   ? theme::kAxisX
                                  : std::strcmp(prefix, "Y") == 0 ? theme::kAxisY
                                                                  : theme::kTextDim;
        drawText(theme::bold(), prefix, {bounds.x + 8.0f, bounds.y + (bounds.height - theme::kFontSize) / 2.0f},
                 theme::kFontSize, prefixColor);
        textX = bounds.x + 18.0f;
    }
    drawTextIn({textX, bounds.y, bounds.x + bounds.width - textX, bounds.height}, TextFormat(format, *value),
               pressed || hot ? theme::kText : Color{205, 207, 214, 255}, true);
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
    drawTextIn(bounds, text.c_str(), theme::kText, false);
    return false;
}

void focusTextField(std::string& text) {
    startTextEdit(&text, text.c_str(), true);
    g.activeSeen = true;
}

bool checkbox(Rectangle bounds, bool* checked) {
    const float size = std::min(bounds.height, 18.0f);
    const Rectangle box{bounds.x, std::floor(bounds.y + (bounds.height - size) / 2.0f), size, size};
    const bool hot = hovered(box);
    const bool pressedHere = pressStartedOn(box);
    const bool changed = hot && pressedHere && IsMouseButtonReleased(MOUSE_BUTTON_LEFT) && !g.active;
    if (changed) {
        *checked = !*checked;
    }
    if (*checked) {
        fill(box, hot ? theme::kAccentHover : theme::kAccent);
        // A tick.
        const Vector2 a{box.x + size * 0.24f, box.y + size * 0.52f};
        const Vector2 b{box.x + size * 0.43f, box.y + size * 0.70f};
        const Vector2 c{box.x + size * 0.77f, box.y + size * 0.32f};
        DrawLineEx(a, b, 2.0f, WHITE);
        DrawLineEx(b, c, 2.0f, WHITE);
    } else {
        fill(box, hot ? theme::kFieldHover : theme::kField);
        outline(box, theme::kBorder);
    }
    return changed;
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
    pushClip(bounds);
    DrawRectangleRec(bounds, LIGHTGRAY);
    for (float x = bounds.x; x < bounds.x + bounds.width; x += 8.0f) {
        for (float y = bounds.y; y < bounds.y + bounds.height; y += 8.0f) {
            if ((static_cast<int>((x - bounds.x) / 8.0f) + static_cast<int>((y - bounds.y) / 8.0f)) % 2 == 0) {
                DrawRectangleRec({x, y, 8.0f, 8.0f}, GRAY);
            }
        }
    }
    DrawRectangleRec(bounds, *color);
    popClip();
    outline(bounds, hot || open ? theme::kAccent : theme::kPanel, theme::kRadius, hot || open ? 1.0f : 2.0f);
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
    drawTextIn({bounds.x, bounds.y, bounds.width - 22.0f, bounds.height}, text, theme::kText, false);
    // A chevron, pointing up while open.
    const float cx = bounds.x + bounds.width - 13.0f;
    const float cy = bounds.y + bounds.height / 2.0f;
    const float dy = g.popupId == id ? -2.5f : 2.5f;
    const Color chevron = hot ? theme::kText : theme::kTextDim;
    DrawLineEx({cx - 4.0f, cy - dy}, {cx, cy + dy}, 1.5f, chevron);
    DrawLineEx({cx, cy + dy}, {cx + 4.0f, cy - dy}, 1.5f, chevron);
    return changed;
}

void drawPopups() {
    drawTooltip();
    if (!g.popupId || !g.popupSeen) {
        return;
    }
    constexpr float kItemHeight = 26.0f;
    constexpr int kMaxVisible = 12;

    const int count = static_cast<int>(g.popupItems.size());
    const float height = static_cast<float>(std::clamp(count, 1, kMaxVisible)) * kItemHeight + 8.0f;
    const Rectangle anchor = g.popupAnchor;
    Rectangle rect{anchor.x, anchor.y + anchor.height + 4.0f, anchor.width, height};
    if (rect.y + rect.height > theme::screenHeight()) {
        rect.y = anchor.y - 4.0f - height;
    }
    g.popupRect = rect;

    const Vector2 mouse = GetMousePosition();
    const bool over = CheckCollisionPointRec(mouse, rect);
    const float maxScroll = std::max(0.0f, static_cast<float>(count) * kItemHeight + 8.0f - height);
    if (over) {
        g.popupScroll = std::clamp(g.popupScroll - GetMouseWheelMove() * kItemHeight * 2.0f, 0.0f, maxScroll);
    }

    fill({rect.x - 2.0f, rect.y, rect.width + 4.0f, rect.height + 4.0f}, Fade(BLACK, 0.25f), theme::kRadius + 2.0f);
    fill(rect, theme::kRaised, theme::kPanelRadius);
    outline(rect, theme::kBorder, theme::kPanelRadius);
    pushClip({rect.x + 1.0f, rect.y + 4.0f, rect.width - 2.0f, rect.height - 8.0f});
    for (int i = 0; i < count; ++i) {
        const Rectangle item{rect.x + 4.0f, rect.y + 4.0f + static_cast<float>(i) * kItemHeight - g.popupScroll,
                             rect.width - 8.0f, kItemHeight};
        if (item.y + item.height < rect.y || item.y > rect.y + rect.height) {
            continue;
        }
        const bool hot = over && !GuiIsLocked() && CheckCollisionPointRec(mouse, item);
        if (hot) {
            fill(item, theme::kAccent);
        } else if (i == g.popupSelected) {
            fill(item, theme::kFieldHover);
        }
        drawTextIn(item, g.popupItems[static_cast<std::size_t>(i)].c_str(), hot ? WHITE : theme::kText, false);
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
    drawTextIn(labelRect, label, theme::kTextDim, false);

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
