#pragma once

#include <raylib.h>

#include <string>
#include <vector>

// Editor widgets built on top of raygui. Widgets are identified by the address
// of the value they edit, so that address must stay stable while it's being edited.
namespace ui {

constexpr float kRowHeight = 28.0f;
constexpr float kFieldHeight = 24.0f;

// Call once per frame before any widget / after all widgets.
void beginFrame();
void endFrame();

// Mouse cursor wanted this frame; the last request before endFrame() wins.
void requestCursor(int cursor);

// Nested clipping: each push intersects with the current clip rectangle.
void pushClip(Rectangle r);
void popClip();

// Widgets ignore the mouse while not interactive (e.g. scrolled out of view).
void setInteractive(bool interactive);
bool isInteractive();

// True while a text field has keyboard focus.
bool isTextEditing();

// Number box: drag horizontally to scrub (Shift = fine, Ctrl = coarse), click to type.
// `prefix` is drawn before the value, e.g. "X". Returns true when the value changed.
bool dragFloat(Rectangle bounds, float* value, float speed = 0.5f, const char* prefix = nullptr,
               const char* format = "%.2f");

// Single-line text box; commits on Enter or click outside, Esc cancels.
bool textField(Rectangle bounds, std::string& text);

// Gives keyboard focus to the textField editing `text`, with its contents selected.
void focusTextField(std::string& text);

bool checkbox(Rectangle bounds, bool* checked);

// raygui buttons fire when the mouse is released over them, even if the press started
// elsewhere, e.g. on a tab's close button after which the "+" button slid under the
// cursor. These wrappers only count clicks whose press started on the same control at
// the same place.
bool button(Rectangle bounds, const char* text);

// raygui window box; returns true when its close button is clicked.
bool windowBox(Rectangle bounds, const char* title);

// Call every frame for a clickable control. True if the current/last left-button press
// started on this control while it was at exactly `r`.
bool pressStartedOn(Rectangle r);

// Flat icon button; `hoverColor` tints the icon while hovered.
bool iconButton(Rectangle bounds, int icon, Color hoverColor);

// Mouse is over `r` and widgets are interactive.
bool hovered(Rectangle r);
// Left click on `r` while no other widget is active.
bool clicked(Rectangle r);

// Draws `text` vertically centered in `r`, clipped to it.
void label(Rectangle r, const char* text, Color color, bool center = false);

// Dropdown field. The list opens in a popup drawn above everything by drawPopups().
// `id` must be stable across frames. Returns true when the user picked a different item.
bool combo(Rectangle bounds, const void* id, const std::vector<std::string>& items, int* selected);

// Draws open popups; call after all panels, before endFrame().
void drawPopups();

// Mouse is over an open popup, so widgets underneath should ignore it.
bool mouseOverPopup();

// Color swatch that expands into a picker when clicked. Returns the height used,
// which is larger than `bounds.height` while expanded.
float colorField(Rectangle bounds, Color* color);

// Lays out labelled property rows: label on the left, value control on the right.
class PropertyGrid {
public:
    PropertyGrid(float x, float y, float width);

    // Draws `label` and returns the rectangle for its value control.
    Rectangle row(const char* label, float height = kFieldHeight);
    void addSpace(float height) { m_y += height; }
    // Extends the last row, e.g. when an expanded color picker needs more room.
    void growLastRow(float extra) { m_y += extra; }

    float y() const { return m_y; }

private:
    float m_x;
    float m_y;
    float m_width;
};

// Splits `r` into two equal columns with a small gap.
void splitHalves(Rectangle r, Rectangle& left, Rectangle& right);

Color styleColor(int property);

} // namespace ui
