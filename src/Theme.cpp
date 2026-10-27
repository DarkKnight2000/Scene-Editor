#include "Theme.h"

#include <raygui.h>
#include <rlgl.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <initializer_list>
#include <string>
#include <unordered_map>
#include <vector>

namespace theme {
namespace {

float g_scale = 1.0f;
Font g_font{};
Font g_bold{};
Font g_mono{};
Font g_title{};
bool g_loaded = false;

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

// Every font file in the system's font folders, by lower-case file name.
const std::unordered_map<std::string, std::filesystem::path>& installedFonts() {
    static std::unordered_map<std::string, std::filesystem::path> fonts;
    static bool scanned = false;
    if (scanned) {
        return fonts;
    }
    scanned = true;
    std::vector<std::filesystem::path> dirs;
    const auto env = [](const char* name) {
        const char* value = std::getenv(name);
        return value != nullptr ? std::filesystem::path(value) : std::filesystem::path();
    };
#ifdef _WIN32
    dirs.push_back(env("WINDIR") / "Fonts");
    dirs.push_back(env("LOCALAPPDATA") / "Microsoft" / "Windows" / "Fonts");
#elif defined(__APPLE__)
    dirs = {"/System/Library/Fonts", "/Library/Fonts", env("HOME") / "Library" / "Fonts"};
#else
    dirs = {"/usr/share/fonts", "/usr/local/share/fonts", env("HOME") / ".local" / "share" / "fonts",
            env("HOME") / ".fonts"};
#endif
    for (const auto& dir : dirs) {
        std::error_code ec;
        if (dir.empty() || !std::filesystem::is_directory(dir, ec)) {
            continue;
        }
        for (auto it = std::filesystem::recursive_directory_iterator(dir, ec);
             !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
            const std::string ext = lower(it->path().extension().string());
            if (ext == ".ttf" || ext == ".otf") {
                fonts.emplace(lower(it->path().filename().string()), it->path());
            }
        }
    }
    return fonts;
}

// The first of `names` that is installed.
std::filesystem::path findFont(std::initializer_list<const char*> names) {
    const auto& fonts = installedFonts();
    for (const char* name : names) {
        if (const auto it = fonts.find(lower(name)); it != fonts.end()) {
            return it->second;
        }
    }
    return {};
}

Font loadFont(const std::filesystem::path& path, float size) {
    if (path.empty()) {
        return GetFontDefault();
    }
    Font font = LoadFontEx(path.string().c_str(), static_cast<int>(std::lround(size * g_scale)), nullptr, 0);
    if (font.texture.id == 0) {
        return GetFontDefault();
    }
    SetTextureFilter(font.texture, TEXTURE_FILTER_BILINEAR);
    return font;
}

void unloadFont(Font& font) {
    if (font.texture.id != 0 && font.texture.id != GetFontDefault().texture.id) {
        UnloadFont(font);
    }
    font = {};
}

int rgba(Color c) {
    return ColorToInt(c);
}

void applyStyle() {
    // Base properties first: raygui copies DEFAULT's to every control.
    const struct {
        int property;
        Color color;
    } colors[] = {
        {BORDER_COLOR_NORMAL, kBorder},       {BASE_COLOR_NORMAL, kField},         {TEXT_COLOR_NORMAL, kText},
        {BORDER_COLOR_FOCUSED, kAccent},      {BASE_COLOR_FOCUSED, kFieldHover},   {TEXT_COLOR_FOCUSED, WHITE},
        {BORDER_COLOR_PRESSED, kAccent},      {BASE_COLOR_PRESSED, kAccentPressed}, {TEXT_COLOR_PRESSED, WHITE},
        {BORDER_COLOR_DISABLED, kBorder},     {BASE_COLOR_DISABLED, kPanel},       {TEXT_COLOR_DISABLED, kTextFaint},
    };
    for (const auto& c : colors) {
        GuiSetStyle(DEFAULT, c.property, rgba(c.color));
    }
    GuiSetStyle(DEFAULT, BORDER_WIDTH, 0);
    GuiSetStyle(DEFAULT, LINE_COLOR, rgba(kBorder));
    GuiSetStyle(DEFAULT, BACKGROUND_COLOR, rgba(kPanel));
    GuiSetFont(g_font);
    GuiSetStyle(DEFAULT, TEXT_SIZE, static_cast<int>(kFontSize));
    GuiSetStyle(DEFAULT, TEXT_SPACING, 0);

    // Text being edited: a field with an accent border, not an accent fill.
    GuiSetStyle(TEXTBOX, BORDER_WIDTH, 1);
    GuiSetStyle(TEXTBOX, BASE_COLOR_PRESSED, rgba(kField));
    GuiSetStyle(TEXTBOX, TEXT_COLOR_PRESSED, rgba(kText));
    GuiSetStyle(TEXTBOX, TEXT_PADDING, 6);

    // Thin scrollbars without arrows.
    GuiSetStyle(LISTVIEW, SCROLLBAR_WIDTH, 10);
    GuiSetStyle(SCROLLBAR, ARROWS_VISIBLE, 0);
    GuiSetStyle(SCROLLBAR, SCROLL_SLIDER_PADDING, 2);
    GuiSetStyle(SCROLLBAR, BASE_COLOR_NORMAL, rgba(kPanel));
    GuiSetStyle(SCROLLBAR, BORDER_COLOR_NORMAL, rgba(kPanel));
    GuiSetStyle(SCROLLBAR, BASE_COLOR_FOCUSED, rgba(kPanel));
    GuiSetStyle(SCROLLBAR, BASE_COLOR_PRESSED, rgba(kPanel));
    GuiSetStyle(DEFAULT, BACKGROUND_COLOR, rgba(kPanel));
}

void loadFonts() {
    unloadFont(g_font);
    unloadFont(g_bold);
    unloadFont(g_mono);
    unloadFont(g_title);
    g_font = loadFont(findFont({"Inter-Regular.ttf", "Inter-Regular.otf", "Inter-Regular-slnt=0.ttf", "segoeui.ttf",
                                "NotoSans-Regular.ttf", "DejaVuSans.ttf", "Roboto-Regular.ttf", "arial.ttf"}),
                      kFontSize);
    g_bold = loadFont(findFont({"Inter-SemiBold.ttf", "Inter-SemiBold.otf", "Inter-SemiBold-slnt=0.ttf", "Inter-Bold.ttf",
                                "Inter-Bold-slnt=0.ttf", "seguisb.ttf", "segoeuib.ttf", "NotoSans-SemiBold.ttf",
                                "NotoSans-Bold.ttf", "DejaVuSans-Bold.ttf", "Roboto-Medium.ttf", "arialbd.ttf"}),
                      kFontSize);
    g_title = loadFont(findFont({"Inter-SemiBold.ttf", "Inter-SemiBold.otf", "Inter-SemiBold-slnt=0.ttf", "Inter-Bold.ttf",
                                 "Inter-Bold-slnt=0.ttf", "seguisb.ttf", "segoeuib.ttf", "NotoSans-SemiBold.ttf",
                                 "NotoSans-Bold.ttf", "DejaVuSans-Bold.ttf", "Roboto-Medium.ttf", "arialbd.ttf"}),
                       kTitleFontSize);
    g_mono = loadFont(findFont({"CascadiaMono.ttf", "CascadiaCode.ttf", "JetBrainsMono-Regular.ttf", "consola.ttf",
                                "DejaVuSansMono.ttf", "RobotoMono-Regular.ttf"}),
                      kFontSize);
    g_loaded = true;
    applyStyle();
}

} // namespace

void load(float uiScale) {
    g_scale = std::clamp(uiScale, 0.75f, 2.0f);
    SetMouseScale(1.0f / g_scale, 1.0f / g_scale);
    loadFonts();
}

void unload() {
    if (g_loaded) {
        unloadFont(g_font);
        unloadFont(g_bold);
        unloadFont(g_mono);
        unloadFont(g_title);
        g_loaded = false;
    }
}

float uiScale() {
    return g_scale;
}

void setUiScale(float scale) {
    scale = std::clamp(scale, 0.75f, 2.0f);
    if (scale != g_scale) {
        load(scale);
    }
}

Font font() {
    return g_font;
}

Font bold() {
    return g_bold;
}

Font mono() {
    return g_mono;
}

Font title() {
    return g_title;
}

float fontSize() {
    return kFontSize;
}

void beginFrame() {
    rlPushMatrix();
    rlScalef(g_scale, g_scale, 1.0f);
}

void endFrame() {
    rlDrawRenderBatchActive();
    rlPopMatrix();
}

float screenWidth() {
    return static_cast<float>(GetScreenWidth()) / g_scale;
}

float screenHeight() {
    return static_cast<float>(GetScreenHeight()) / g_scale;
}

Vector2 mouseDelta() {
    const Vector2 d = GetMouseDelta();
    return {d.x / g_scale, d.y / g_scale};
}

Rectangle toPixels(Rectangle r) {
    return {r.x * g_scale, r.y * g_scale, r.width * g_scale, r.height * g_scale};
}

void beginCamera(Camera2D camera) {
    camera.offset.x *= g_scale;
    camera.offset.y *= g_scale;
    camera.zoom *= g_scale;
    BeginMode2D(camera); // replaces the frame's scale matrix...
}

void endCamera() {
    EndMode2D();
    rlScalef(g_scale, g_scale, 1.0f); // ...so put it back
}

} // namespace theme
