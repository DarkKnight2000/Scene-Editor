#include "Workspace.h"

#include "Components.h"
#include "Log.h"
#include "ProjectExport.h"
#include "SceneExporter.h"
#include "SceneSerializer.h"
#include "Theme.h"
#include "Widgets.h"

#include <raygui.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <utility>

namespace {
constexpr const char* kExtension = kSceneExtension;
constexpr float kButtonHeight = 28.0f;
constexpr float kListRowHeight = 28.0f;
constexpr Color kWarningColor = theme::kWarning;
constexpr Color kCloseColor = theme::kError;

bool ctrlDown() {
    return IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
}

bool shiftDown() {
    return IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
}

// Dims the editor and returns a centered dialog rectangle.
Rectangle beginModal(float width, float height) {
    const float sw = theme::screenWidth();
    const float sh = theme::screenHeight();
    DrawRectangleRec({0.0f, 0.0f, sw, sh}, Fade(BLACK, 0.55f));
    return {std::floor((sw - width) / 2.0f), std::floor((sh - height) / 2.0f), width, height};
}

// Trims spaces and a trailing ".json"; returns "" if the name can't be a file name.
std::string sanitizeName(std::string name) {
    const auto notSpace = [](unsigned char c) { return c != ' '; };
    name.erase(name.begin(), std::find_if(name.begin(), name.end(), notSpace));
    name.erase(std::find_if(name.rbegin(), name.rend(), notSpace).base(), name.end());
    if (name.size() > 5 && name.ends_with(kExtension)) {
        name.resize(name.size() - 5);
    }
    if (name.empty() || name == "." || name == ".." || name.find_first_of("\\/:*?\"<>|") != std::string::npos) {
        return {};
    }
    return name;
}

bool samePath(const std::filesystem::path& a, const std::filesystem::path& b) {
    std::error_code ec;
    const bool same = std::filesystem::equivalent(a, b, ec);
    return ec ? a == b : same;
}
} // namespace

Workspace::Workspace(Project project) : m_project(std::move(project)), m_dir(m_project.scenesDir()) {
    std::error_code ec;
    std::filesystem::create_directories(m_dir, ec);
    if (ec) {
        logError("Can't create scenes folder " + m_dir.string() + ": " + ec.message());
    } else {
        logInfo("Opened project " + m_project.name + " (" + m_project.root().string() + ")");
    }
    m_docs.push_back(makeUntitled());
}

void Workspace::beginFrame() {
    // Actions may queue further actions, so swap the list out first.
    std::vector<std::function<void()>> actions;
    actions.swap(m_deferred);
    for (auto& action : actions) {
        action();
    }

    // Re-check the active scene for unsaved changes a few times per second.
    if (--m_dirtyCheckCountdown <= 0) {
        computeDirty(active());
        m_dirtyCheckCountdown = 15;
    }
    updateWindowTitle();
    m_runner.update();
}

void Workspace::handleShortcuts() {
    if (isModalOpen() || ui::isTextEditing()) {
        return;
    }
    if (IsKeyPressed(KEY_F5)) {
        if (shiftDown()) {
            m_runner.stop();
        } else {
            play();
        }
        return;
    }
    if (!ctrlDown()) {
        return;
    }
    Document& doc = active();
    if (IsKeyPressed(KEY_Z)) {
        if (shiftDown()) {
            doc.history.redo(doc.scene, doc.selected);
        } else {
            doc.history.undo(doc.scene, doc.selected);
        }
    } else if (IsKeyPressed(KEY_Y)) {
        doc.history.redo(doc.scene, doc.selected);
    } else if (IsKeyPressed(KEY_N)) {
        requestNew();
    } else if (IsKeyPressed(KEY_O)) {
        requestOpen();
    } else if (IsKeyPressed(KEY_S)) {
        if (shiftDown()) {
            requestSaveAs(doc, false);
        } else {
            requestSave(doc);
        }
    } else if (IsKeyPressed(KEY_E)) {
        exportProject();
    } else if (IsKeyPressed(KEY_W)) {
        requestClose(doc);
    } else if (IsKeyPressed(KEY_TAB) && m_docs.size() > 1) {
        const std::size_t count = m_docs.size();
        const std::size_t next = shiftDown() ? (m_active + count - 1) % count : (m_active + 1) % count;
        Document* target = m_docs[next].get();
        defer([this, target] { activate(target); });
    }
}

// --- Toolbar and tabs ---

void Workspace::drawToolbar(Rectangle bar) {
    Document& doc = active();
    DrawRectangleRec(bar, theme::kPanel);
    DrawLineV({bar.x, bar.y + bar.height - 0.5f}, {bar.x + bar.width, bar.y + bar.height - 0.5f}, theme::kBorder);

    //   [project name | new open save save-as | undo redo]      [ Play ]      [Settings  Export  Close]
    constexpr float kButton = 30.0f;
    constexpr float kGap = 2.0f;
    const float y = bar.y + (bar.height - kButton) / 2.0f;
    const float cy = bar.y + bar.height / 2.0f;
    const auto separator = [&](float x) {
        DrawLineV({x, bar.y + 12.0f}, {x, bar.y + bar.height - 12.0f}, theme::kBorder);
    };

    // The project, on the left.
    float x = bar.x + theme::kSpace3;
    GuiDrawIcon(ICON_FOLDER_OPEN, static_cast<int>(x), static_cast<int>(cy - 8.0f), 1, theme::kAccentHover);
    x += 24.0f;
    const float nameWidth = std::min(ui::textWidth(theme::bold(), m_project.name.c_str(), theme::kFontSize), 220.0f);
    ui::pushClip({x, bar.y, nameWidth, bar.height});
    ui::drawText(theme::bold(), m_project.name.c_str(), {x, cy - theme::kFontSize / 2.0f}, theme::kFontSize, theme::kText);
    ui::popClip();
    x += nameWidth + theme::kSpace3;
    separator(x);
    x += theme::kSpace3;

    // Scene actions: icon buttons, explained by their tooltips.
    struct Tool {
        int icon;
        const char* tip;
        bool enabled;
        bool separatorBefore;
    };
    const Tool tools[] = {
        {ICON_FILE_NEW, "New scene  (Ctrl+N)", true, false},
        {ICON_FOLDER_FILE_OPEN, "Open scene  (Ctrl+O)", true, false},
        {ICON_FILE_SAVE_CLASSIC, "Save  (Ctrl+S)", true, false},
        {ICON_FILE_SAVE, "Save as  (Ctrl+Shift+S)", true, false},
        {ICON_UNDO, "Undo  (Ctrl+Z)", doc.history.canUndo(), true},
        {ICON_REDO, "Redo  (Ctrl+Y)", doc.history.canRedo(), false},
    };
    int clicked = -1;
    for (int i = 0; i < static_cast<int>(std::size(tools)); ++i) {
        const Tool& tool = tools[i];
        if (tool.separatorBefore) {
            x += theme::kSpace1;
            separator(x);
            x += theme::kSpace2;
        }
        const Rectangle r{x, y, kButton, kButton};
        if (!tool.enabled) {
            GuiDisable();
        }
        if (ui::button(r, GuiIconText(tool.icon, nullptr), ui::ButtonKind::Ghost)) {
            clicked = i;
        }
        GuiEnable();
        ui::tooltip(r, tool.tip);
        x += kButton + kGap;
    }
    switch (clicked) {
    case 0: requestNew(); break;
    case 1: requestOpen(); break;
    case 2: requestSave(doc); break;
    case 3: requestSaveAs(doc, false); break;
    case 4: doc.history.undo(doc.scene, doc.selected); break;
    case 5: doc.history.redo(doc.scene, doc.selected); break;
    default: break;
    }
    const float leftEnd = x;

    // Project actions, on the right.
    struct Action {
        int icon;
        const char* label;
        const char* tip;
    };
    const Action actions[] = {
        {ICON_GEAR, "Settings", "Project settings"},
        {ICON_FILE_EXPORT, "Export", "Export the C code  (Ctrl+E)"},
        {ICON_EXIT, "Close", "Close the project"},
    };
    float rightWidth = 0.0f;
    std::vector<float> widths;
    for (const Action& action : actions) {
        widths.push_back(ui::textWidth(theme::font(), action.label, theme::kFontSize) + 16.0f + 6.0f + 2.0f * theme::kSpace3);
        rightWidth += widths.back() + kGap;
    }
    float rx = bar.x + bar.width - theme::kSpace2 - rightWidth;
    int action = -1;
    for (std::size_t i = 0; i < std::size(actions); ++i) {
        const Rectangle r{rx, y, widths[i], kButton};
        if (ui::button(r, GuiIconText(actions[i].icon, actions[i].label), ui::ButtonKind::Ghost)) {
            action = static_cast<int>(i);
        }
        ui::tooltip(r, actions[i].tip);
        rx += widths[i] + kGap;
    }
    switch (action) {
    case 0: openProjectSettings(); break;
    case 1: exportProject(); break;
    case 2: requestExit(Exit::CloseProject); break;
    default: break;
    }

    // Play, the one accent-coloured button, in the middle (or just right of the scene actions).
    const bool gameActive = m_runner.state() != GameRunner::State::Idle;
    const bool building = m_runner.state() == GameRunner::State::Building;
    const char* playLabel = building ? "Building" : gameActive ? "Stop" : "Play";
    constexpr float kPlayWidth = 112.0f;
    const float playX = std::max(std::floor(bar.x + (bar.width - kPlayWidth) / 2.0f), leftEnd + theme::kSpace4);
    const Rectangle play{playX, y, kPlayWidth, kButton};
    if (ui::button(play, GuiIconText(gameActive ? ICON_PLAYER_STOP : ICON_PLAYER_PLAY, playLabel),
                   gameActive ? ui::ButtonKind::Normal : ui::ButtonKind::Primary)) {
        if (gameActive) {
            m_runner.stop();
        } else {
            this->play();
        }
    }
    ui::tooltip(play, gameActive ? "Stop the game  (Shift+F5)" : "Export, build and run the game  (F5)");
}

void Workspace::drawTabs(Rectangle bar) {
    DrawRectangleRec(bar, theme::kBackground);

    const Font font = GuiGetFont();
    const float fontSize = static_cast<float>(GuiGetStyle(DEFAULT, TEXT_SIZE));
    const float spacing = static_cast<float>(GuiGetStyle(DEFAULT, TEXT_SPACING));
    constexpr float kGap = 2.0f;
    constexpr float kPlusWidth = 30.0f;
    constexpr float kDragThreshold = 6.0f;

    // Tab widths from their titles, shrunk evenly if they don't all fit.
    std::vector<std::string> titles;
    std::vector<float> widths;
    float total = 0.0f;
    for (const auto& doc : m_docs) {
        titles.push_back(doc->displayName());
        const float w = std::clamp(MeasureTextEx(font, titles.back().c_str(), fontSize, spacing).x + 52.0f, 100.0f, 260.0f);
        widths.push_back(w);
        total += w + kGap;
    }
    const float available = bar.width - 8.0f - kPlusWidth - 6.0f;
    if (total > available) {
        const float scale = available / total;
        for (float& w : widths) {
            w = std::max(70.0f, w * scale);
        }
    }
    const auto tabPositions = [&] {
        std::vector<float> xs;
        float x = bar.x + 4.0f;
        for (const float w : widths) {
            xs.push_back(x);
            x += w + kGap;
        }
        return xs;
    };

    // Dragging: swap with a neighbour once the mouse passes its middle.
    const Vector2 mouse = GetMousePosition();
    if (m_dragTab != nullptr) {
        const int dragged = indexOf(m_dragTab);
        if (dragged < 0 || !IsMouseButtonDown(MOUSE_BUTTON_LEFT) || isModalOpen()) {
            m_dragTab = nullptr;
            m_tabDragging = false;
        } else {
            if (!m_tabDragging && std::fabs(mouse.x - m_tabPressX) > kDragThreshold) {
                m_tabDragging = true;
            }
            if (m_tabDragging) {
                const auto d = static_cast<std::size_t>(dragged);
                const std::vector<float> xs = tabPositions();
                std::size_t target = d;
                if (d > 0 && mouse.x < xs[d - 1] + widths[d - 1] / 2.0f) {
                    target = d - 1;
                } else if (d + 1 < m_docs.size() && mouse.x > xs[d + 1] + widths[d + 1] / 2.0f) {
                    target = d + 1;
                }
                if (target != d) {
                    Document* activeDoc = m_docs[m_active].get();
                    std::swap(m_docs[d], m_docs[target]);
                    std::swap(titles[d], titles[target]);
                    std::swap(widths[d], widths[target]);
                    m_active = static_cast<std::size_t>(indexOf(activeDoc));
                }
            }
        }
    }

    const std::vector<float> xs = tabPositions();
    Rectangle draggedTab{};
    std::size_t draggedIndex = m_docs.size();
    for (std::size_t i = 0; i < m_docs.size(); ++i) {
        Document* doc = m_docs[i].get();
        const bool isActive = i == m_active;
        const Rectangle tab{xs[i], bar.y + 4.0f, widths[i], bar.height - 8.0f};

        if (m_tabDragging && doc == m_dragTab) {
            // Leave an empty slot; the tab itself is drawn under the mouse below.
            ui::outline(tab, theme::kBorder);
            draggedTab = tab;
            draggedIndex = i;
            continue;
        }

        const bool hot = !m_tabDragging && ui::hovered(tab);
        if (isActive || hot) {
            ui::fill(tab, isActive ? theme::kPanel : Color{255, 255, 255, 8});
        }
        if (isActive) {
            ui::fill({tab.x + 8.0f, tab.y + tab.height - 2.0f, tab.width - 16.0f, 2.0f}, theme::kAccent, 1.0f);
        }

        const Rectangle closeButton{tab.x + tab.width - 26.0f, tab.y + (tab.height - 20.0f) / 2.0f, 20.0f, 20.0f};
        ui::label({tab.x + 8.0f, tab.y, tab.width - 38.0f, tab.height}, titles[i].c_str(),
                  isActive ? theme::kText : hot ? theme::kText : theme::kTextDim);
        if (m_tabDragging) {
            continue; // no clicks or closing while another tab is being dragged
        }

        // Unsaved changes: a dot where the close button goes, until the tab is hovered.
        bool closing = false;
        if (hot || (isActive && !doc->dirty)) {
            closing = ui::iconButton(closeButton, ICON_CROSS_SMALL, kCloseColor);
        } else if (doc->dirty) {
            DrawCircleV({closeButton.x + closeButton.width / 2.0f, closeButton.y + closeButton.height / 2.0f}, 4.0f,
                        isActive ? theme::kText : theme::kTextDim);
        }
        if (hot && IsMouseButtonPressed(MOUSE_BUTTON_MIDDLE)) {
            closing = true;
        }
        if (closing) {
            requestClose(*doc);
        } else if (ui::clicked(tab)) {
            // Press selects the tab and may start a drag.
            if (!isActive) {
                defer([this, doc] { activate(doc); });
            }
            m_dragTab = doc;
            m_tabPressX = mouse.x;
            m_tabGrabOffset = mouse.x - tab.x;
        }
    }

    // "+" opens a new tab.
    const float plusX = xs.empty() ? bar.x + 4.0f : xs.back() + widths.back() + kGap;
    const Rectangle plus{plusX + 4.0f, bar.y + 6.0f, bar.height - 12.0f, bar.height - 12.0f};
    if (ui::button(plus, "", ui::ButtonKind::Ghost)) {
        requestNew();
    }
    {
        const Color color = ui::hovered(plus) ? theme::kText : theme::kTextDim;
        const float cx = std::floor(plus.x + plus.width / 2.0f);
        const float cy = std::floor(plus.y + plus.height / 2.0f);
        DrawRectangleRec({cx - 6.0f, cy - 1.0f, 12.0f, 2.0f}, color);
        DrawRectangleRec({cx - 1.0f, cy - 6.0f, 2.0f, 12.0f}, color);
    }
    ui::tooltip(plus, "New scene  (Ctrl+N)");

    // The dragged tab follows the mouse, on top of the others.
    if (draggedIndex < m_docs.size()) {
        Rectangle floating = draggedTab;
        floating.x = std::clamp(mouse.x - m_tabGrabOffset, bar.x, bar.x + bar.width - floating.width);
        ui::fill({floating.x - 1.0f, floating.y + 1.0f, floating.width + 2.0f, floating.height + 2.0f}, Fade(BLACK, 0.3f));
        ui::fill(floating, theme::kRaised);
        ui::fill({floating.x + 8.0f, floating.y + floating.height - 2.0f, floating.width - 16.0f, 2.0f}, theme::kAccent, 1.0f);
        ui::label({floating.x + 8.0f, floating.y, floating.width - 38.0f, floating.height}, titles[draggedIndex].c_str(),
                  theme::kText);
    }
}

void Workspace::drawDialogs() {
    switch (m_dialog) {
    case Dialog::Open: drawOpenDialog(); break;
    case Dialog::SaveAs: drawSaveAsDialog(); break;
    case Dialog::ConfirmClose: drawConfirmCloseDialog(); break;
    case Dialog::Quit: drawQuitDialog(); break;
    case Dialog::ProjectSettings: drawProjectSettingsDialog(); break;
    case Dialog::None: break;
    }
}

// --- Requests ---

void Workspace::requestNew() {
    defer([this] {
        m_docs.push_back(makeUntitled());
        activate(m_docs.back().get());
        logInfo("New scene");
    });
}

void Workspace::requestOpen() {
    refreshFileList();
    m_dialog = Dialog::Open;
}

void Workspace::requestSave(Document& doc) {
    if (doc.path.empty()) {
        requestSaveAs(doc, false);
    } else {
        saveDocument(doc, doc.path);
    }
}

void Workspace::requestSaveAs(Document& doc, bool closeAfterSave) {
    m_dialogDoc = &doc;
    m_closeAfterSave = closeAfterSave;
    m_saveName = doc.path.empty() ? "untitled" : doc.path.stem().string();
    m_dialog = Dialog::SaveAs;
    ui::focusTextField(m_saveName);
}

void Workspace::requestClose(Document& doc) {
    if (computeDirty(doc)) {
        m_dialogDoc = &doc;
        m_dialog = Dialog::ConfirmClose;
        Document* target = &doc;
        defer([this, target] { activate(target); }); // show the scene being asked about
    } else {
        Document* target = &doc;
        defer([this, target] { closeDocument(target); });
    }
}

// --- Operations ---

void Workspace::openFile(const std::filesystem::path& path) {
    // Already open: just switch to it.
    for (const auto& doc : m_docs) {
        if (!doc->path.empty() && samePath(doc->path, path)) {
            activate(doc.get());
            return;
        }
    }

    auto doc = std::make_unique<Document>();
    std::string error;
    if (!loadSceneFile(path, doc->scene, error)) {
        logError("Failed to open " + path.filename().string() + ": " + error);
        return;
    }
    doc->selected = doc->scene.world();
    doc->path = path;
    markSaved(*doc);
    logInfo("Opened " + path.filename().string() + " (" + std::to_string(doc->scene.entities().size() - 2) +
            " entities)");

    // Replace an untouched empty "Untitled" tab instead of piling up blank tabs.
    if (isPristine(active())) {
        m_docs[m_active] = std::move(doc);
    } else {
        m_docs.push_back(std::move(doc));
        activate(m_docs.back().get());
    }
}

bool Workspace::saveDocument(Document& doc, const std::filesystem::path& path) {
    std::string error;
    if (!saveSceneFile(doc.scene, path, error)) {
        logError("Failed to save " + path.filename().string() + ": " + error);
        return false;
    }
    doc.path = path;
    markSaved(doc);
    logInfo("Saved " + path.filename().string() + " (" + std::to_string(doc.scene.entities().size() - 2) +
            " entities)");
    return true;
}

void Workspace::closeDocument(Document* doc) {
    const int index = indexOf(doc);
    if (index < 0) {
        return;
    }
    logInfo("Closed " + doc->displayName());
    const auto closed = static_cast<std::size_t>(index);
    m_docs.erase(m_docs.begin() + index);

    // Always keep one tab open.
    if (m_docs.empty()) {
        m_docs.push_back(makeUntitled());
        m_active = 0;
    } else if (closed < m_active || m_active >= m_docs.size()) {
        m_active = m_active > 0 ? m_active - 1 : 0;
    }
}

void Workspace::activate(Document* doc) {
    const int index = indexOf(doc);
    if (index < 0 || static_cast<std::size_t>(index) == m_active) {
        return;
    }
    active().history.flush(); // finish any edit in progress on the old tab
    m_active = static_cast<std::size_t>(index);
    m_dirtyCheckCountdown = 0;
}

std::unique_ptr<Document> Workspace::makeUntitled() {
    // Lowest number not used by another open, never-saved scene: closing everything
    // gives "Untitled" again rather than "Untitled 7".
    int number = 1;
    while (std::any_of(m_docs.begin(), m_docs.end(), [&](const std::unique_ptr<Document>& d) {
        return d->path.empty() && d->untitledNumber == number;
    })) {
        ++number;
    }
    auto doc = std::make_unique<Document>();
    doc->untitledNumber = number;
    markSaved(*doc);
    return doc;
}

// --- Dialogs ---

void Workspace::drawOpenDialog() {
    const Rectangle box = beginModal(560.0f, 460.0f);
    bool close = ui::windowBox(box, "Open Scene");

    ui::label({box.x + 12.0f, box.y + 30.0f, box.width - 24.0f, 24.0f}, m_dir.string().c_str(),
              ui::styleColor(TEXT_COLOR_DISABLED));

    const Rectangle list{box.x + 12.0f, box.y + 60.0f, box.width - 24.0f, box.height - 60.0f - 56.0f};
    const float contentHeight = kListRowHeight * static_cast<float>(m_files.size());
    Rectangle view{};
    GuiScrollPanel(list, nullptr, {0.0f, 0.0f, list.width - 16.0f, contentHeight}, &m_fileScroll, &view);

    bool open = false;
    if (m_files.empty()) {
        ui::label({list.x + 10.0f, list.y + 6.0f, list.width - 20.0f, kListRowHeight}, "No saved scenes yet.",
                  ui::styleColor(TEXT_COLOR_DISABLED));
    }
    ui::pushClip(view);
    for (std::size_t i = 0; i < m_files.size(); ++i) {
        const Rectangle row{view.x, view.y + m_fileScroll.y + kListRowHeight * static_cast<float>(i), view.width,
                            kListRowHeight};
        const int index = static_cast<int>(i);
        const bool hot = ui::hovered(row) && CheckCollisionPointRec(GetMousePosition(), view);
        if (index == m_fileSelected) {
            DrawRectangleRec(row, ui::styleColor(BASE_COLOR_PRESSED));
        } else if (hot) {
            DrawRectangleRec(row, ui::styleColor(BASE_COLOR_FOCUSED));
        }
        if (hot && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
            const double now = GetTime();
            open = index == m_lastClickIndex && now - m_lastClickTime < 0.4; // double-click
            m_fileSelected = index;
            m_lastClickIndex = index;
            m_lastClickTime = now;
        }
        const Color text = ui::styleColor(index == m_fileSelected ? TEXT_COLOR_PRESSED : TEXT_COLOR_NORMAL);
        GuiDrawIcon(ICON_FILETYPE_TEXT, static_cast<int>(row.x + 8.0f), static_cast<int>(row.y + 6.0f), 1, text);
        ui::label({row.x + 32.0f, row.y, row.width - 40.0f, row.height}, m_files[i].c_str(), text);
    }
    ui::popClip();

    const float by = box.y + box.height - 12.0f - kButtonHeight;
    if (ui::button({box.x + box.width - 12.0f - 110.0f, by, 110.0f, kButtonHeight}, "Open", ui::ButtonKind::Primary) ||
        (IsKeyPressed(KEY_ENTER) && m_fileSelected >= 0)) {
        open = m_fileSelected >= 0;
    }
    if (ui::button({box.x + box.width - 12.0f - 230.0f, by, 110.0f, kButtonHeight}, "Cancel") ||
        IsKeyPressed(KEY_ESCAPE)) {
        close = true;
    }

    if (open && m_fileSelected >= 0 && m_fileSelected < static_cast<int>(m_files.size())) {
        const std::filesystem::path path = m_dir / m_files[static_cast<std::size_t>(m_fileSelected)];
        m_dialog = Dialog::None;
        defer([this, path] { openFile(path); });
    } else if (close) {
        m_dialog = Dialog::None;
    }
}

void Workspace::drawSaveAsDialog() {
    Document& doc = *m_dialogDoc;
    const Rectangle box = beginModal(500.0f, 200.0f);
    bool close = ui::windowBox(box, "Save Scene As");

    ui::label({box.x + 12.0f, box.y + 34.0f, 120.0f, 28.0f}, "Name", ui::styleColor(TEXT_COLOR_NORMAL));
    const Rectangle field{box.x + 90.0f, box.y + 36.0f, box.width - 102.0f - 70.0f, 26.0f};
    ui::textField(field, m_saveName);
    ui::label({field.x + field.width + 6.0f, field.y, 64.0f, field.height}, kExtension,
              ui::styleColor(TEXT_COLOR_DISABLED));

    const std::string name = sanitizeName(m_saveName);
    const std::filesystem::path path = m_dir / (name + kExtension);
    const bool exists = !name.empty() && std::filesystem::exists(path);
    if (name.empty()) {
        ui::label({box.x + 12.0f, box.y + 74.0f, box.width - 24.0f, 26.0f}, "Enter a valid file name.",
                  ui::styleColor(TEXT_COLOR_DISABLED));
    } else if (exists && path != doc.path) {
        ui::label({box.x + 12.0f, box.y + 74.0f, box.width - 24.0f, 26.0f}, "A scene with this name exists.",
                  kWarningColor);
    }

    const float by = box.y + box.height - 12.0f - kButtonHeight;
    const Rectangle saveButton{box.x + box.width - 12.0f - 130.0f, by, 130.0f, kButtonHeight};
    bool save = false;
    if (name.empty()) {
        GuiDisable();
        ui::button(saveButton, "Save", ui::ButtonKind::Primary);
        GuiEnable();
    } else {
        save = ui::button(saveButton, exists ? "Overwrite" : "Save", ui::ButtonKind::Primary) ||
               (IsKeyPressed(KEY_ENTER) && !ui::isTextEditing());
    }
    if (ui::button({box.x + box.width - 12.0f - 250.0f, by, 110.0f, kButtonHeight}, "Cancel") ||
        (IsKeyPressed(KEY_ESCAPE) && !ui::isTextEditing())) {
        close = true;
    }

    if (save) {
        m_dialog = Dialog::None;
        const bool saved = saveDocument(doc, path);
        if (m_quitFlow) {
            if (saved) {
                continueQuitSaves();
            } else {
                cancelQuit();
            }
        } else if (saved && m_closeAfterSave) {
            Document* target = &doc;
            defer([this, target] { closeDocument(target); });
        }
    } else if (close) {
        m_dialog = Dialog::None;
        if (m_quitFlow) {
            cancelQuit(); // no file name given: don't lose that scene
        }
    }
}

void Workspace::drawConfirmCloseDialog() {
    Document& doc = *m_dialogDoc;
    const Rectangle box = beginModal(560.0f, 170.0f);
    bool cancel = ui::windowBox(box, "Unsaved Changes");

    const std::string message = "Save changes to " + doc.displayName() + " before closing?";
    GuiDrawIcon(ICON_WARNING, static_cast<int>(box.x + 14.0f), static_cast<int>(box.y + 46.0f), 1, kWarningColor);
    ui::label({box.x + 40.0f, box.y + 40.0f, box.width - 52.0f, 28.0f}, message.c_str(),
              ui::styleColor(TEXT_COLOR_NORMAL));

    const float by = box.y + box.height - 12.0f - kButtonHeight;
    const bool save = ui::button({box.x + box.width - 12.0f - 110.0f, by, 110.0f, kButtonHeight}, "Save", ui::ButtonKind::Primary);
    const bool discard = ui::button({box.x + box.width - 12.0f - 260.0f, by, 140.0f, kButtonHeight}, "Don't Save");
    if (ui::button({box.x + box.width - 12.0f - 370.0f, by, 100.0f, kButtonHeight}, "Cancel") ||
        IsKeyPressed(KEY_ESCAPE)) {
        cancel = true;
    }

    Document* target = &doc;
    if (save) {
        m_dialog = Dialog::None;
        if (doc.path.empty()) {
            requestSaveAs(doc, /*closeAfterSave=*/true);
        } else if (saveDocument(doc, doc.path)) {
            defer([this, target] { closeDocument(target); });
        }
    } else if (discard) {
        m_dialog = Dialog::None;
        defer([this, target] { closeDocument(target); });
    } else if (cancel) {
        m_dialog = Dialog::None;
    }
}

// --- Project settings ---

void Workspace::openProjectSettings() {
    m_settingsWidth = static_cast<float>(m_project.display.width);
    m_settingsHeight = static_cast<float>(m_project.display.height);
    m_settingsMode = 0;
    m_settingsExport = m_project.exportSettings;
    m_settingsPhysics = m_project.physics;
    m_settingsBox2d = m_project.box2dDir;
    for (std::size_t i = 0; i < std::size(kScaleModes); ++i) {
        if (kScaleModes[i] == m_project.display.scaleMode) {
            m_settingsMode = static_cast<int>(i);
        }
    }
    m_dialog = Dialog::ProjectSettings;
}

void Workspace::drawProjectSettingsDialog() {
    const Rectangle box = beginModal(760.0f, 660.0f);
    bool close = ui::windowBox(box, "Project Settings");
    const float lx = box.x + 16.0f;
    const float fx = box.x + 200.0f;
    const float fw = box.width - 216.0f;
    const Color text = ui::styleColor(TEXT_COLOR_NORMAL);
    const Color dim = ui::styleColor(TEXT_COLOR_DISABLED);

    // Design resolution: the scene area the game is designed for, centred on the origin.
    ui::label({lx, box.y + 40.0f, 180.0f, 26.0f}, "Design resolution", text);
    Rectangle w;
    Rectangle h;
    ui::splitHalves({fx, box.y + 40.0f, fw, 26.0f}, w, h);
    ui::dragFloat(w, &m_settingsWidth, 1.0f, "W", "%.0f");
    ui::dragFloat(h, &m_settingsHeight, 1.0f, "H", "%.0f");

    struct Preset {
        const char* label;
        float width;
        float height;
    };
    constexpr Preset presets[] = {{"1280x720", 1280, 720}, {"1920x1080", 1920, 1080}, {"640x360", 640, 360}, {"320x180", 320, 180}};
    float px = fx;
    const float presetWidth = (fw - 3.0f * 6.0f) / 4.0f;
    for (const Preset& preset : presets) {
        if (ui::button({px, box.y + 74.0f, presetWidth, 24.0f}, preset.label)) {
            m_settingsWidth = preset.width;
            m_settingsHeight = preset.height;
        }
        px += presetWidth + 6.0f;
    }

    ui::label({lx, box.y + 120.0f, 180.0f, 26.0f}, "Scale mode", text);
    std::vector<std::string> modes;
    for (const ScaleMode mode : kScaleModes) {
        modes.emplace_back(scaleModeName(mode));
    }
    ui::combo({fx, box.y + 120.0f, fw, 26.0f}, &m_settingsMode, modes, &m_settingsMode);
    const char* help[] = {
        "Whole design area visible; wider screens see more.",
        "Whole design area visible; bars fill extra space.",
        "No bars; the design area is cropped to fit.",
    };
    ui::label({fx, box.y + 152.0f, fw, 24.0f}, help[std::clamp(m_settingsMode, 0, 2)], dim);
    ui::label({fx, box.y + 176.0f, fw, 24.0f}, "The exported game always scales this way.", dim);

    // Export: where to, and when files you may edit are written.
    DrawLineV({lx, box.y + 216.0f}, {box.x + box.width - 16.0f, box.y + 216.0f}, ui::styleColor(LINE_COLOR));
    ui::label({lx, box.y + 228.0f, 180.0f, 26.0f}, "Export folder", text);
    ui::textField({fx, box.y + 228.0f, fw - 96.0f, 26.0f}, m_settingsExport.folder);
    if (ui::button({fx + fw - 90.0f, box.y + 228.0f, 90.0f, 26.0f}, "Default")) {
        m_settingsExport.folder = kDefaultExportFolder;
    }
    {
        Project candidate = m_project;
        candidate.exportSettings.folder = m_settingsExport.folder;
        const std::u8string shown = candidate.exportPath().u8string();
        ui::label({fx, box.y + 256.0f, fw, 24.0f}, std::string(shown.begin(), shown.end()).c_str(), dim);
    }

    std::vector<std::string> policies;
    for (const ExportPolicy policy : kExportPolicies) {
        policies.emplace_back(exportPolicyLabel(policy));
    }
    struct FileRow {
        const char* label;
        ExportPolicy* policy;
    };
    const FileRow rows[] = {
        {"Makefile", &m_settingsExport.makefile},
        {"project_main.c", &m_settingsExport.projectMain},
        {".vscode settings", &m_settingsExport.vscodeSettings},
        {".gitignore", &m_settingsExport.gitignore},
    };
    // Bottom row first: an open dropdown draws over the rows below it.
    for (int i = static_cast<int>(std::size(rows)) - 1; i >= 0; --i) {
        const float y = box.y + 288.0f + static_cast<float>(i) * 32.0f;
        ui::label({lx, y, 180.0f, 26.0f}, rows[i].label, text);
        int current = static_cast<int>(*rows[i].policy);
        if (ui::combo({fx, y, 200.0f, 26.0f}, rows[i].policy, policies, &current)) {
            *rows[i].policy = kExportPolicies[std::clamp(current, 0, 2)];
        }
    }
    ui::label({fx + 212.0f, box.y + 288.0f, fw - 212.0f, 24.0f}, "Always: replaced on export", dim);
    ui::label({fx + 212.0f, box.y + 320.0f, fw - 212.0f, 24.0f}, "If missing: your edits stay", dim);

    // Physics (Box2D) in the exported game.
    const float py = box.y + 440.0f;
    DrawLineV({lx, py - 12.0f}, {box.x + box.width - 16.0f, py - 12.0f}, ui::styleColor(LINE_COLOR));
    ui::label({lx, py, 180.0f, 26.0f}, "Physics (Box2D)", text);
    ui::checkbox({fx, py + 3.0f, 20.0f, 20.0f}, &m_settingsPhysics.enabled);
    ui::label({fx + 26.0f, py, 300.0f, 26.0f}, "Export Box2D and rigidbodies", text);
    ui::label({lx, py + 34.0f, 180.0f, 26.0f}, "Pixels per metre", text);
    ui::dragFloat({fx, py + 34.0f, 200.0f, 26.0f}, &m_settingsPhysics.pixelsPerMeter, 0.5f);
    m_settingsPhysics.pixelsPerMeter = std::clamp(m_settingsPhysics.pixelsPerMeter, 1.0f, 100000.0f);
    ui::label({lx, py + 66.0f, 180.0f, 26.0f}, "Gravity (m/s^2)", text);
    Rectangle gx;
    Rectangle gy;
    ui::splitHalves({fx, py + 66.0f, 300.0f, 26.0f}, gx, gy);
    ui::dragFloat(gx, &m_settingsPhysics.gravityX, 0.05f, "X");
    ui::dragFloat(gy, &m_settingsPhysics.gravityY, 0.05f, "Y");
    ui::label({fx + 310.0f, py + 66.0f, fw - 310.0f, 26.0f}, "y points down", dim);
    ui::label({lx, py + 98.0f, 180.0f, 26.0f}, "Box2D path", text);
    ui::textField({fx, py + 98.0f, fw - 96.0f, 26.0f}, m_settingsBox2d);
    if (ui::button({fx + fw - 90.0f, py + 98.0f, 90.0f, 26.0f}, "Default")) {
        m_settingsBox2d = Project::defaultBox2dDir();
    }
    {
        Project candidateBox2d = m_project;
        candidateBox2d.box2dDir = m_settingsBox2d;
        std::error_code box2dEc;
        const bool box2dFound =
            std::filesystem::is_regular_file(candidateBox2d.box2dPath() / "include" / "box2d" / "box2d.h", box2dEc);
        ui::label({fx, py + 130.0f, fw, 24.0f},
                  box2dFound ? "Found." : "include/box2d/box2d.h not found here: exports with physics will fail.",
                  box2dFound ? dim : Color{230, 120, 90, 255});
    }

    const float by = box.y + box.height - 12.0f - kButtonHeight;
    const bool save = ui::button({box.x + box.width - 12.0f - 120.0f, by, 120.0f, kButtonHeight}, "Save", ui::ButtonKind::Primary);
    if (ui::button({box.x + box.width - 12.0f - 240.0f, by, 110.0f, kButtonHeight}, "Cancel") ||
        (IsKeyPressed(KEY_ESCAPE) && !ui::isTextEditing())) {
        close = true;
    }

    if (save) {
        m_project.display.width = std::clamp(static_cast<int>(std::lround(m_settingsWidth)), 16, 16384);
        m_project.display.height = std::clamp(static_cast<int>(std::lround(m_settingsHeight)), 16, 16384);
        m_project.display.scaleMode = kScaleModes[std::clamp(m_settingsMode, 0, 2)];
        m_project.exportSettings = m_settingsExport;
        if (m_project.exportSettings.folder.empty()) {
            m_project.exportSettings.folder = kDefaultExportFolder;
        }
        m_project.physics = m_settingsPhysics;
        m_project.box2dDir = m_settingsBox2d.empty() ? Project::defaultBox2dDir() : m_settingsBox2d;
        std::string error;
        if (saveProject(m_project, error)) {
            logInfo("Project settings saved: " + std::to_string(m_project.display.width) + "x" +
                    std::to_string(m_project.display.height) + ", " + scaleModeName(m_project.display.scaleMode));
        } else {
            logError("Couldn't save project settings: " + error);
        }
        m_dialog = Dialog::None;
    } else if (close) {
        m_dialog = Dialog::None;
    }
}

// --- Export ---

void Workspace::play() {
    // Start in the scene being edited (a saved one; otherwise the first scene).
    const Document& doc = active();
    const std::string startScene = doc.path.empty() ? std::string() : doc.path.stem().string();
    if (exportProject(startScene)) {
        if (!startScene.empty()) {
            logInfo("Play: starting in " + startScene);
        }
        const std::filesystem::path outDir = projectExportDir(m_project);
        m_runner.play(outDir, exportedExecutable(outDir, m_project.name));
    } else {
        logError("Play: nothing to run, the export failed");
    }
}

bool Workspace::exportProject(const std::string& startScene) {
    // Save open scenes first (Export and Play both come here), so the scene files match what
    // gets exported. Never-saved scenes have no file yet, so they can't be exported.
    for (const auto& doc : m_docs) {
        if (!doc->path.empty() && computeDirty(*doc)) {
            doc->history.flush();
            saveDocument(*doc, doc->path);
        }
    }
    for (const auto& doc : m_docs) {
        if (doc->path.empty() && computeDirty(*doc)) {
            logWarning("Export: " + doc->displayName() +
                       " has never been saved (use Save As to give it a name), so it isn't exported");
        }
    }

    // Scenes open in a tab: export what the editor shows.
    return ::exportProject(
        m_project,
        [this](const std::filesystem::path& file) -> const Scene* {
            for (const auto& doc : m_docs) {
                if (!doc->path.empty() && samePath(doc->path, file)) {
                    return &doc->scene;
                }
            }
            return nullptr;
        },
        nullptr, startScene);
}

// --- Session ---

std::vector<std::filesystem::path> Workspace::openScenePaths(int& activeIndex) const {
    std::vector<std::filesystem::path> paths;
    activeIndex = 0;
    for (std::size_t i = 0; i < m_docs.size(); ++i) {
        if (m_docs[i]->path.empty()) {
            continue; // never saved: nothing to reopen
        }
        if (i == m_active) {
            activeIndex = static_cast<int>(paths.size());
        }
        paths.push_back(m_docs[i]->path);
    }
    return paths;
}

void Workspace::restoreScenes(const std::vector<std::filesystem::path>& paths, int activeIndex) {
    Document* toActivate = nullptr;
    for (std::size_t i = 0; i < paths.size(); ++i) {
        std::error_code ec;
        if (!std::filesystem::is_regular_file(paths[i], ec)) {
            logWarning("Previously open scene is missing: " + paths[i].string());
            continue;
        }
        openFile(paths[i]); // opens and activates it (or replaces the empty Untitled tab)
        if (static_cast<int>(i) == activeIndex || toActivate == nullptr) {
            toActivate = &active();
        }
    }
    if (toActivate) {
        activate(toActivate);
    }
}

// --- Quitting ---

void Workspace::requestExit(Exit kind) {
    if (m_dialog == Dialog::Quit || m_quitFlow) {
        return; // already asking
    }
    m_exitKind = kind;
    active().history.flush();

    m_quitItems.clear();
    for (const auto& doc : m_docs) {
        if (computeDirty(*doc)) {
            m_quitItems.push_back({doc.get(), true});
        }
    }
    if (m_quitItems.empty()) {
        m_exitConfirmed = true;
        return;
    }
    // A quit request replaces whatever dialog was open.
    m_quitScroll = {};
    m_dialog = Dialog::Quit;
}

void Workspace::drawQuitDialog() {
    constexpr float kRow = 30.0f;
    constexpr int kMaxVisibleRows = 8;
    const int visibleRows = std::min(static_cast<int>(m_quitItems.size()), kMaxVisibleRows);
    const float listHeight = kRow * static_cast<float>(visibleRows) + 4.0f;
    const Rectangle box = beginModal(600.0f, 150.0f + listHeight);
    bool cancel = ui::windowBox(box, "Unsaved Changes");

    const std::string message = m_quitItems.size() == 1
                                    ? std::string("This scene has unsaved changes:")
                                    : std::to_string(m_quitItems.size()) + " scenes have unsaved changes:";
    GuiDrawIcon(ICON_WARNING, static_cast<int>(box.x + 14.0f), static_cast<int>(box.y + 40.0f), 1, kWarningColor);
    ui::label({box.x + 40.0f, box.y + 34.0f, box.width - 52.0f, 28.0f}, message.c_str(),
              ui::styleColor(TEXT_COLOR_NORMAL));

    // One row per scene: [x] name   (asks for a name)
    const Rectangle list{box.x + 12.0f, box.y + 68.0f, box.width - 24.0f, listHeight};
    Rectangle view{};
    GuiScrollPanel(list, nullptr, {0.0f, 0.0f, list.width - 16.0f, kRow * static_cast<float>(m_quitItems.size())},
                   &m_quitScroll, &view);
    ui::pushClip(view);
    ui::setInteractive(CheckCollisionPointRec(GetMousePosition(), view));
    for (std::size_t i = 0; i < m_quitItems.size(); ++i) {
        QuitItem& item = m_quitItems[i];
        const Rectangle row{view.x + 8.0f, view.y + 2.0f + m_quitScroll.y + kRow * static_cast<float>(i),
                            view.width - 16.0f, kRow};
        ui::checkbox({row.x, row.y, 24.0f, row.height}, &item.save);
        const Color text = ui::styleColor(item.save ? TEXT_COLOR_NORMAL : TEXT_COLOR_DISABLED);
        ui::label({row.x + 30.0f, row.y, row.width - 210.0f, row.height}, item.doc->displayName().c_str(), text);
        if (item.doc->path.empty()) {
            ui::label({row.x + row.width - 180.0f, row.y, 180.0f, row.height}, "(asks for a name)",
                      ui::styleColor(TEXT_COLOR_DISABLED));
        }
    }
    ui::setInteractive(true);
    ui::popClip();

    const float by = box.y + box.height - 12.0f - kButtonHeight;
    const bool anyChecked =
        std::any_of(m_quitItems.begin(), m_quitItems.end(), [](const QuitItem& i) { return i.save; });
    const bool saveAndQuit = ui::button({box.x + box.width - 12.0f - 150.0f, by, 150.0f, kButtonHeight},
                                        m_exitKind == Exit::Quit ? (anyChecked ? "Save & Quit" : "Quit")
                                                                 : (anyChecked ? "Save & Close" : "Close"),
                                        ui::ButtonKind::Primary) ||
                             IsKeyPressed(KEY_ENTER);
    const bool discard =
        ui::button({box.x + box.width - 12.0f - 300.0f, by, 140.0f, kButtonHeight}, "Don't Save");
    if (ui::button({box.x + box.width - 12.0f - 410.0f, by, 100.0f, kButtonHeight}, "Cancel") ||
        IsKeyPressed(KEY_ESCAPE)) {
        cancel = true;
    }

    if (saveAndQuit) {
        m_dialog = Dialog::None;
        m_quitSaveAsQueue.clear();
        for (const QuitItem& item : m_quitItems) {
            if (!item.save) {
                continue;
            }
            if (item.doc->path.empty()) {
                m_quitSaveAsQueue.push_back(item.doc);
            } else if (!saveDocument(*item.doc, item.doc->path)) {
                // Keep the editor open so nothing is lost; the log says what failed.
                cancelQuit();
                return;
            }
        }
        m_quitFlow = true;
        continueQuitSaves();
    } else if (discard) {
        m_dialog = Dialog::None;
        m_exitConfirmed = true;
    } else if (cancel) {
        cancelQuit();
    }
}

// Asks for the next never-saved scene's file name, or quits when none are left.
void Workspace::continueQuitSaves() {
    if (m_quitSaveAsQueue.empty()) {
        m_quitFlow = false;
        m_exitConfirmed = true;
        return;
    }
    Document* doc = m_quitSaveAsQueue.front();
    m_quitSaveAsQueue.erase(m_quitSaveAsQueue.begin());
    defer([this, doc] { activate(doc); }); // show the scene being named
    requestSaveAs(*doc, /*closeAfterSave=*/false);
}

void Workspace::cancelQuit() {
    m_dialog = Dialog::None;
    m_quitFlow = false;
    m_quitSaveAsQueue.clear();
    m_quitItems.clear();
    logInfo(m_exitKind == Exit::Quit ? "Quit cancelled" : "Close Project cancelled");
}

// --- Helpers ---

void Workspace::refreshFileList() {
    m_files.clear();
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(m_dir, ec)) {
        if (entry.is_regular_file() && entry.path().extension() == kExtension) {
            m_files.push_back(entry.path().filename().string());
        }
    }
    std::sort(m_files.begin(), m_files.end());

    m_fileSelected = -1;
    m_fileScroll = {};
    m_lastClickIndex = -1;
    const Document& doc = active();
    if (!doc.path.empty()) {
        const auto it = std::find(m_files.begin(), m_files.end(), doc.path.filename().string());
        if (it != m_files.end()) {
            m_fileSelected = static_cast<int>(it - m_files.begin());
        }
    }
}

void Workspace::markSaved(Document& doc) {
    doc.savedSnapshot = serializeScene(doc.scene);
    doc.dirty = false;
}

bool Workspace::computeDirty(Document& doc) {
    doc.dirty = serializeScene(doc.scene) != doc.savedSnapshot;
    return doc.dirty;
}

bool Workspace::isPristine(Document& doc) {
    const bool onlyRoots = std::all_of(doc.scene.entities().begin(), doc.scene.entities().end(),
                                       [&](const Entity& e) { return doc.scene.isRoot(e.id); });
    return doc.path.empty() && onlyRoots && !doc.history.canUndo() && !computeDirty(doc);
}

int Workspace::indexOf(const Document* doc) const {
    for (std::size_t i = 0; i < m_docs.size(); ++i) {
        if (m_docs[i].get() == doc) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

void Workspace::updateWindowTitle() {
    const Document& doc = active();
    const std::string title = doc.displayName() + (doc.dirty ? "*" : "") + " - " + m_project.name + " - Scene Editor";
    if (title != m_appliedTitle) {
        SetWindowTitle(title.c_str());
        m_appliedTitle = title;
    }
}
