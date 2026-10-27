#pragma once

#include <raylib.h>

// The editor's look: palette, fonts, spacing, and the UI scale (100% / 125% / 150%...).
//
// UI scale: the editor lays everything out in logical pixels. beginFrame() scales all
// drawing by uiScale(), raylib's mouse scale maps the mouse back to logical pixels, and
// fonts are loaded at the scaled size so text stays sharp. Use screenWidth() /
// screenHeight() and mouseDelta() instead of raylib's, and beginCamera() / endCamera()
// instead of BeginMode2D / EndMode2D.
namespace theme {

// Surfaces, from the back to the front.
inline constexpr Color kBackground{20, 21, 24, 255}; // window, gaps between panels, scene viewport
inline constexpr Color kPanel{29, 30, 34, 255};      // panels
inline constexpr Color kRaised{37, 39, 44, 255};     // headers, toolbar groups, popups
inline constexpr Color kField{43, 45, 51, 255};      // inputs, buttons
inline constexpr Color kFieldHover{52, 55, 62, 255};
inline constexpr Color kBorder{50, 52, 59, 255};

// Text.
inline constexpr Color kText{226, 228, 233, 255};
inline constexpr Color kTextDim{150, 153, 162, 255};
inline constexpr Color kTextFaint{100, 103, 112, 255};

// One accent colour, used sparingly: selection, focus, toggles that are on, Play.
inline constexpr Color kAccent{77, 140, 255, 255};
inline constexpr Color kAccentHover{100, 157, 255, 255};
inline constexpr Color kAccentPressed{60, 120, 226, 255};
inline constexpr Color kSelection{77, 140, 255, 60}; // selected rows

inline constexpr Color kWarning{226, 182, 70, 255};
inline constexpr Color kError{232, 92, 84, 255};
inline constexpr Color kAxisX{232, 102, 94, 255};
inline constexpr Color kAxisY{104, 194, 124, 255};

// Spacing scale and shapes.
inline constexpr float kSpace1 = 4.0f;
inline constexpr float kSpace2 = 8.0f;
inline constexpr float kSpace3 = 12.0f;
inline constexpr float kSpace4 = 16.0f;
inline constexpr float kRadius = 4.0f;   // controls
inline constexpr float kPanelRadius = 6.0f;

// Text sizes (logical pixels).
inline constexpr float kFontSize = 15.0f;
inline constexpr float kSmallFontSize = 12.0f;
inline constexpr float kTitleFontSize = 30.0f;

// Loads the fonts (the first installed of a list: Inter, Segoe UI, Noto Sans, DejaVu Sans...;
// raylib's default font if none is found) and sets raygui's style. After InitWindow.
void load(float uiScale);
void unload();

float uiScale();
// Changes the UI scale (clamped to 0.75..2) and reloads the fonts at the new size.
void setUiScale(float scale);

Font font();     // UI text
Font bold();     // headings and captions
Font mono();     // numbers and logs
Font title();    // big headings (kTitleFontSize)
float fontSize(); // kFontSize

// Wrap each frame's drawing (after BeginDrawing, before EndDrawing).
void beginFrame();
void endFrame();

// The window in logical pixels.
float screenWidth();
float screenHeight();
// Mouse movement since the last frame, in logical pixels.
Vector2 mouseDelta();
// A logical rectangle in window pixels, e.g. for scissor rectangles.
Rectangle toPixels(Rectangle r);

// 2D camera mode for a camera set up in logical pixels.
void beginCamera(Camera2D camera);
void endCamera();

} // namespace theme
