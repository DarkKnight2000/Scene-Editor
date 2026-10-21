#include "Workspace.h"

#include "Components.h"
#include "Log.h"
#include "SceneExporter.h"
#include "SceneSerializer.h"
#include "Widgets.h"

#include <raygui.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <utility>

namespace {
constexpr const char* kExtension = ".json";
constexpr float kButtonHeight = 28.0f;
constexpr float kListRowHeight = 28.0f;
constexpr Color kWarningColor{230, 200, 80, 255};
constexpr Color kCloseColor{235, 100, 100, 255};

bool ctrlDown() {
    return IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
}

bool shiftDown() {
    return IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
}

// Dims the editor and returns a centered dialog rectangle.
Rectangle beginModal(float width, float height) {
    const float sw = static_cast<float>(GetScreenWidth());
    const float sh = static_cast<float>(GetScreenHeight());
    DrawRectangle(0, 0, GetScreenWidth(), GetScreenHeight(), Fade(BLACK, 0.55f));
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
std::string upperId(std::string id) {
    for (char& c : id) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return id;
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
    DrawRectangleRec(bar, ui::styleColor(BASE_COLOR_NORMAL));

    // Two groups, each in its own frame with a small caption:
    //   [Scene: New  Open  Save  Save As | Undo  Redo]   [Project: <name>  Play | Settings  Export  Close Project]
    constexpr float kButtonH = 26.0f;
    constexpr float kGap = 4.0f;
    constexpr float kGroupPadding = 6.0f;
    constexpr float kSeparatorGap = 10.0f;
    const float y = bar.y + (bar.height - kButtonH) / 2.0f;
    const Font font = GuiGetFont();
    const float fontSize = static_cast<float>(GuiGetStyle(DEFAULT, TEXT_SIZE));
    const float spacing = static_cast<float>(GuiGetStyle(DEFAULT, TEXT_SPACING));
    const auto textWidth = [&](const char* text) { return MeasureTextEx(font, text, fontSize, spacing).x; };

    struct Item {
        int icon;
        const char* label;
        float width;
        bool enabled = true;
        bool separatorBefore = false; // thin divider between sub-groups
    };
    const auto groupWidth = [&](const char* caption, const std::vector<Item>& items, float extra) {
        float w = kGroupPadding + textWidth(caption) + 10.0f + extra;
        for (const Item& item : items) {
            w += item.width + kGap + (item.separatorBefore ? kSeparatorGap : 0.0f);
        }
        return w - kGap + kGroupPadding;
    };
    // Draws the frame and caption; returns where the group's content starts.
    const auto beginGroup = [&](float x, float width, const char* caption) {
        const Rectangle frame{x, bar.y + 3.0f, width, bar.height - 6.0f};
        DrawRectangleRec(frame, ui::styleColor(BACKGROUND_COLOR));
        DrawRectangleLinesEx(frame, 1.0f, ui::styleColor(LINE_COLOR));
        ui::label({x + kGroupPadding, bar.y, textWidth(caption) + 4.0f, bar.height}, caption,
                  ui::styleColor(TEXT_COLOR_DISABLED));
        return x + kGroupPadding + textWidth(caption) + 10.0f;
    };
    // Draws the buttons; returns the index of the one clicked, or -1.
    const auto drawItems = [&](float x, const std::vector<Item>& items) {
        int clicked = -1;
        for (std::size_t i = 0; i < items.size(); ++i) {
            const Item& item = items[i];
            if (item.separatorBefore) {
                const float sx = x + kSeparatorGap / 2.0f - kGap / 2.0f;
                DrawLineV({sx, bar.y + 8.0f}, {sx, bar.y + bar.height - 8.0f}, ui::styleColor(LINE_COLOR));
                x += kSeparatorGap;
            }
            const Rectangle r{x, y, item.width, kButtonH};
            if (!item.enabled) {
                GuiDisable();
            }
            if (ui::button(r, GuiIconText(item.icon, item.label)) && item.enabled) {
                clicked = static_cast<int>(i);
            }
            GuiEnable();
            x += item.width + kGap;
        }
        return clicked;
    };

    // --- Scene group (acts on the scene tabs) ---
    const std::vector<Item> sceneItems{
        {ICON_FILE_NEW, "New", 84.0f},
        {ICON_FOLDER_FILE_OPEN, "Open", 92.0f},
        {ICON_FILE_SAVE_CLASSIC, "Save", 92.0f},
        {ICON_FILE_SAVE_CLASSIC, "Save As", 124.0f},
        {ICON_UNDO, "Undo", 92.0f, doc.history.canUndo(), true},
        {ICON_REDO, "Redo", 92.0f, doc.history.canRedo()},
    };
    const float sceneX = bar.x + 6.0f;
    switch (drawItems(beginGroup(sceneX, groupWidth("Scene", sceneItems, 0.0f), "Scene"), sceneItems)) {
    case 0: requestNew(); break;
    case 1: requestOpen(); break;
    case 2: requestSave(doc); break;
    case 3: requestSaveAs(doc, false); break;
    case 4: doc.history.undo(doc.scene, doc.selected); break;
    case 5: doc.history.redo(doc.scene, doc.selected); break;
    default: break;
    }

    // --- Project group (right-aligned) ---
    const bool gameActive = m_runner.state() != GameRunner::State::Idle;
    const char* playLabel = m_runner.state() == GameRunner::State::Building ? "Building" : (gameActive ? "Stop" : "Play");
    const std::vector<Item> projectItems{
        {gameActive ? ICON_PLAYER_STOP : ICON_PLAYER_PLAY, playLabel, 112.0f},
        {ICON_GEAR, "Settings", 116.0f, true, true},
        {ICON_FILE_EXPORT, "Export", 104.0f},
        {ICON_EXIT, "Close Project", 160.0f},
    };
    const float nameWidth = std::min(textWidth(m_project.name.c_str()), 260.0f) + 24.0f + 10.0f; // icon + name
    const float projectWidth = groupWidth("Project", projectItems, nameWidth);
    const float projectX = bar.x + bar.width - 6.0f - projectWidth;
    float px = beginGroup(projectX, projectWidth, "Project");
    GuiDrawIcon(ICON_FOLDER_OPEN, static_cast<int>(px), static_cast<int>(bar.y + (bar.height - 16.0f) / 2.0f), 1,
                ui::styleColor(TEXT_COLOR_NORMAL));
    ui::label({px + 22.0f, bar.y, nameWidth - 24.0f, bar.height}, m_project.name.c_str(),
              ui::styleColor(TEXT_COLOR_FOCUSED));
    px += nameWidth;
    switch (drawItems(px, projectItems)) {
    case 0:
        if (gameActive) {
            m_runner.stop();
        } else {
            play();
        }
        break;
    case 1: openProjectSettings(); break;
    case 2: exportProject(); break;
    case 3: requestExit(Exit::CloseProject); break;
    default: break;
    }
}

void Workspace::drawTabs(Rectangle bar) {
    DrawRectangleRec(bar, Color{24, 24, 28, 255});
    DrawLineV({bar.x, bar.y + bar.height - 1.0f}, {bar.x + bar.width, bar.y + bar.height - 1.0f},
              ui::styleColor(BORDER_COLOR_NORMAL));

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
        titles.push_back(doc->displayName() + (doc->dirty ? " *" : ""));
        const float w = std::clamp(MeasureTextEx(font, titles.back().c_str(), fontSize, spacing).x + 48.0f, 110.0f, 260.0f);
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
        const Rectangle tab{xs[i], bar.y + 4.0f, widths[i], bar.height - 4.0f};

        if (m_tabDragging && doc == m_dragTab) {
            // Leave an empty slot; the tab itself is drawn under the mouse below.
            DrawRectangleLinesEx(tab, 1.0f, ui::styleColor(BORDER_COLOR_NORMAL));
            draggedTab = tab;
            draggedIndex = i;
            continue;
        }

        const bool hot = !m_tabDragging && ui::hovered(tab);
        DrawRectangleRec(tab, isActive ? ui::styleColor(BACKGROUND_COLOR)
                              : hot    ? ui::styleColor(BASE_COLOR_FOCUSED)
                                       : Color{32, 32, 37, 255});
        if (isActive) {
            DrawRectangleRec({tab.x, tab.y, tab.width, 2.0f}, ui::styleColor(BORDER_COLOR_FOCUSED));
        }

        const Rectangle closeButton{tab.x + tab.width - 24.0f, tab.y + (tab.height - 20.0f) / 2.0f, 20.0f, 20.0f};
        ui::label({tab.x + 10.0f, tab.y, tab.width - 38.0f, tab.height}, titles[i].c_str(),
                  ui::styleColor(isActive ? TEXT_COLOR_FOCUSED : TEXT_COLOR_NORMAL));
        if (m_tabDragging) {
            continue; // no clicks or closing while another tab is being dragged
        }

        bool closing = false;
        if (isActive || hot) {
            closing = ui::iconButton(closeButton, ICON_CROSS_SMALL, kCloseColor);
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
    const Rectangle plus{plusX + 4.0f, bar.y + 5.0f, kPlusWidth - 4.0f, bar.height - 8.0f};
    if (ui::button(plus, "+")) {
        requestNew();
    }

    // The dragged tab follows the mouse, on top of the others.
    if (draggedIndex < m_docs.size()) {
        Rectangle floating = draggedTab;
        floating.x = std::clamp(mouse.x - m_tabGrabOffset, bar.x, bar.x + bar.width - floating.width);
        DrawRectangleRec(floating, ui::styleColor(BACKGROUND_COLOR));
        DrawRectangleRec({floating.x, floating.y, floating.width, 2.0f}, ui::styleColor(BORDER_COLOR_FOCUSED));
        DrawRectangleLinesEx(floating, 1.0f, ui::styleColor(BORDER_COLOR_FOCUSED));
        ui::label({floating.x + 10.0f, floating.y, floating.width - 38.0f, floating.height},
                  titles[draggedIndex].c_str(), ui::styleColor(TEXT_COLOR_FOCUSED));
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
    if (ui::button({box.x + box.width - 12.0f - 110.0f, by, 110.0f, kButtonHeight}, "Open") ||
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
        ui::button(saveButton, "Save");
        GuiEnable();
    } else {
        save = ui::button(saveButton, exists ? "Overwrite" : "Save") ||
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
    const bool save = ui::button({box.x + box.width - 12.0f - 110.0f, by, 110.0f, kButtonHeight}, "Save");
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
    m_settingsEasyMemory = m_project.easyMemoryDir;
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
    const Rectangle box = beginModal(760.0f, 720.0f);
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
    ui::label({lx, box.y + 196.0f, box.width - 32.0f, 24.0f},
              "Shown as a frame in the Scene view and used by Export.", dim);

    // easy_memory: copied into every export (utils/), from this folder.
    ui::label({lx, box.y + 240.0f, 180.0f, 26.0f}, "easy_memory path", text);
    ui::textField({fx, box.y + 240.0f, fw - 96.0f, 26.0f}, m_settingsEasyMemory);
    if (ui::button({fx + fw - 90.0f, box.y + 240.0f, 90.0f, 26.0f}, "Default")) {
        m_settingsEasyMemory = Project::defaultEasyMemoryDir();
    }
    Project candidate = m_project;
    candidate.easyMemoryDir = m_settingsEasyMemory;
    std::error_code ec;
    const bool found = std::filesystem::is_regular_file(candidate.easyMemoryPath() / "easy_memory.h", ec);
    ui::label({fx, box.y + 272.0f, fw, 24.0f},
              found ? "Found. Relative paths start at the project folder."
                    : "easy_memory.h not found here: exports will fail.",
              found ? dim : Color{230, 120, 90, 255});

    // Export: which scale modes the runtime supports, and when files you may edit are written.
    DrawLineV({lx, box.y + 312.0f}, {box.x + box.width - 16.0f, box.y + 312.0f}, ui::styleColor(LINE_COLOR));
    ui::label({lx, box.y + 324.0f, 180.0f, 26.0f}, "Exported modes", text);
    const int defaultMode = std::clamp(m_settingsMode, 0, 2);
    m_settingsExport.scaleModes[defaultMode] = true; // the default mode is always exported
    for (int i = 0; i < 3; ++i) {
        const Rectangle r{fx + static_cast<float>(i) * 130.0f, box.y + 327.0f, 20.0f, 20.0f};
        if (i == defaultMode) {
            GuiDisable();
        }
        ui::checkbox(r, &m_settingsExport.scaleModes[i]);
        GuiEnable();
        ui::label({r.x + 26.0f, box.y + 324.0f, 100.0f, 26.0f}, scaleModeName(kScaleModes[i]),
                  i == defaultMode ? dim : text);
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
        const float y = box.y + 362.0f + static_cast<float>(i) * 32.0f;
        ui::label({lx, y, 180.0f, 26.0f}, rows[i].label, text);
        int current = static_cast<int>(*rows[i].policy);
        if (ui::combo({fx, y, 200.0f, 26.0f}, rows[i].policy, policies, &current)) {
            *rows[i].policy = kExportPolicies[std::clamp(current, 0, 2)];
        }
    }
    ui::label({fx + 212.0f, box.y + 362.0f, fw - 212.0f, 24.0f}, "Always: replaced on export", dim);
    ui::label({fx + 212.0f, box.y + 394.0f, fw - 212.0f, 24.0f}, "If missing: your edits stay", dim);

    // Physics (Box2D) in the exported game.
    const float py = box.y + 500.0f;
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
    const bool save = ui::button({box.x + box.width - 12.0f - 120.0f, by, 120.0f, kButtonHeight}, "Save");
    if (ui::button({box.x + box.width - 12.0f - 240.0f, by, 110.0f, kButtonHeight}, "Cancel") ||
        (IsKeyPressed(KEY_ESCAPE) && !ui::isTextEditing())) {
        close = true;
    }

    if (save) {
        m_project.display.width = std::clamp(static_cast<int>(std::lround(m_settingsWidth)), 16, 16384);
        m_project.display.height = std::clamp(static_cast<int>(std::lround(m_settingsHeight)), 16, 16384);
        m_project.display.scaleMode = kScaleModes[std::clamp(m_settingsMode, 0, 2)];
        m_project.easyMemoryDir = m_settingsEasyMemory.empty() ? Project::defaultEasyMemoryDir() : m_settingsEasyMemory;
        m_project.exportSettings = m_settingsExport;
        m_project.physics = m_settingsPhysics;
        m_project.box2dDir = m_settingsBox2d.empty() ? Project::defaultBox2dDir() : m_settingsBox2d;
        m_project.exportSettings.includeMode(m_project.display.scaleMode);
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
    if (exportProject()) {
        const std::filesystem::path outDir = m_project.root() / "export";
        m_runner.play(outDir, exportedExecutable(outDir, m_project.name));
    } else {
        logError("Play: nothing to run, the export failed");
    }
}

bool Workspace::exportProject() {
    const std::filesystem::path outDir = m_project.root() / "export";

    // Files you may edit by hand, and when Project Settings says to write them.
    const ExportSettings& options = m_project.exportSettings;
    struct EditableFile {
        std::filesystem::path path;
        ExportPolicy policy;
        bool write = false;
    };
    EditableFile editable[] = {
        {outDir / "Makefile", options.makefile},
        {outDir / "src" / "project_main.c", options.projectMain},
        {outDir / ".vscode" / "c_cpp_properties.json", options.vscodeSettings},
        {outDir / ".gitignore", options.gitignore},
    };
    std::vector<std::filesystem::path> keep;
    std::string notWritten;
    for (EditableFile& file : editable) {
        std::error_code exists;
        file.write = file.policy == ExportPolicy::Always ||
                     (file.policy == ExportPolicy::IfMissing && !std::filesystem::exists(file.path, exists));
        if (!file.write) {
            keep.push_back(file.path);
            notWritten += (notWritten.empty() ? "" : ", ") + file.path.filename().string() + " (" +
                          (file.policy == ExportPolicy::Never ? "never" : "already there") + ")";
        }
    }

    // Start clean: drop everything an earlier export generated (renamed / deleted scenes,
    // older formats), except the files that won't be written again. Files the user added
    // to the folder are kept.
    if (const int removed = removeGeneratedFiles(outDir, keep); removed > 0) {
        logInfo("Export: replacing " + std::to_string(removed) + " previously generated file(s)");
    }

    // Every scene file in the project, by name.
    std::vector<std::filesystem::path> files;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(m_dir, ec)) {
        if (entry.is_regular_file() && entry.path().extension() == kExtension) {
            files.push_back(entry.path());
        }
    }
    std::sort(files.begin(), files.end());

    std::vector<std::string> exported;
    std::vector<std::string> identifiers;
    int failed = 0;
    for (const auto& file : files) {
        const std::string name = file.stem().string();
        const std::string id = sceneIdentifier(name);
        if (std::find(identifiers.begin(), identifiers.end(), id) != identifiers.end()) {
            logError("Export: skipped " + file.filename().string() + ": its C++ name '" + id +
                     "' clashes with another scene; rename one of them");
            ++failed;
            continue;
        }

        // Open in a tab: export what the editor shows. Otherwise: the saved file.
        const Scene* scene = nullptr;
        for (const auto& doc : m_docs) {
            if (!doc->path.empty() && samePath(doc->path, file)) {
                scene = &doc->scene;
                break;
            }
        }
        Scene loaded;
        std::string error;
        if (scene == nullptr) {
            if (!loadSceneFile(file, loaded, error)) {
                logError("Export: can't read " + file.filename().string() + ": " + error);
                ++failed;
                continue;
            }
            scene = &loaded;
        }

        if (!m_project.physics.enabled && !scene->registry().view<RigidbodyComponent>().empty()) {
            logWarning("Export: " + file.filename().string() +
                       " has rigidbodies, but physics is off in Project Settings: they aren't exported");
        }
        if (!exportScene(*scene, name, outDir, m_project.physics.enabled, error)) {
            logError("Export: " + file.filename().string() + ": " + error);
            ++failed;
            continue;
        }
        exported.push_back(name);
        identifiers.push_back(id);
        logInfo("Exported " + id + ".h/.c  (scene_" + id + "_load / _draw, SCENE_ID_" + upperId(id) + ")");
    }

    for (const auto& doc : m_docs) {
        if (doc->path.empty() && computeDirty(*doc)) {
            logWarning("Export: " + doc->displayName() + " isn't saved in the project yet, so it wasn't exported");
        }
    }

    std::string error;
    bool usable = !exported.empty();
    if (usable && !(writeSceneCommon(outDir, m_project.display, options, m_project.physics, error) &&
                    writeScenesHeader(exported, outDir, error) && writeSceneManager(exported, outDir, error) &&
                    (!editable[1].write || writeProjectMain(m_project.name, exported, outDir, error)) &&
                    (!editable[0].write || writeMakefile(m_project.name, exported, options.modeCount(), outDir, error)) &&
                    (!editable[2].write || writeVSCodeFiles(outDir, m_project.name, error)) &&
                    (!editable[3].write || writeGitignore(outDir, m_project.name, error)))) {
        logError("Export: " + error);
        ++failed;
        usable = false;
    }
    if (!notWritten.empty()) {
        logInfo("Export: not written, as set in Project Settings: " + notWritten);
    }
    if (usable && m_project.physics.enabled && !writeBox2D(outDir, m_project.box2dPath(), error)) {
        logError("Export: " + error + ". Set the Box2D path in Project Settings, or turn physics off.");
        ++failed;
        usable = false; // the export can't build without it
    }
    if (usable && !writeEasyMemory(outDir, m_project.easyMemoryPath(), error)) {
        logError("Export: " + error + ". Set the easy_memory folder in Project Settings.");
        ++failed;
        usable = false; // the export can't build without it
    }
    if (files.empty()) {
        logWarning("Export: the project has no saved scenes yet");
    } else {
        logInfo("Export finished: " + std::to_string(exported.size()) + " scene(s) to " + outDir.string() +
                (failed > 0 ? " (" + std::to_string(failed) + " failed)" : ""));
    }
    return usable;
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
                                                                 : (anyChecked ? "Save & Close" : "Close")) ||
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
                                       [&](entt::entity e) { return doc.scene.isRoot(e); });
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
