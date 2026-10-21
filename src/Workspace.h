#pragma once

#include <raylib.h>

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "Document.h"
#include "GameRunner.h"
#include "Project.h"

// The open project's scenes (tabs) and everything that manages them: the toolbar (New /
// Open / Save / Save As / Undo / Redo / Close Project), the tab bar, keyboard shortcuts,
// file dialogs and unsaved-change tracking. Scene files live in the project's scenes folder.
//
// Changes to the set of documents (new, open, close, switch) are queued and applied by
// beginFrame(), so the active Document stays valid for the whole frame.
class Workspace {
public:
    explicit Workspace(Project project);

    const Project& project() const { return m_project; }

    // How Play builds the game (from the editor config).
    void setPlaySettings(PlaySettings settings) { m_runner.setSettings(std::move(settings)); }

    // Applies queued tab changes. Call first thing each frame, before active().
    void beginFrame();

    Document& active() { return *m_docs[m_active]; }

    // Ctrl+N/O/S/Shift+S/W/E, Ctrl+Tab, Ctrl+Z/Y, F5 (Play), Shift+F5 (Stop). Call before drawing.
    void handleShortcuts();

    void drawToolbar(Rectangle bar);
    void drawTabs(Rectangle bar);

    // Draws the open dialog, if any, above everything else.
    void drawDialogs();

    // While true, the rest of the editor should ignore input.
    bool isModalOpen() const { return m_dialog != Dialog::None; }

    // Saved scenes currently open, in tab order; `activeIndex` is the active tab's
    // position in that list (0 if the active tab was never saved).
    std::vector<std::filesystem::path> openScenePaths(int& activeIndex) const;

    // Startup: reopens scenes from a previous session. Missing files are skipped.
    void restoreScenes(const std::vector<std::filesystem::path>& paths, int activeIndex);

    // Leaving the project: Quit (window close button, Alt+F4) or Close Project (back to
    // the launcher). Asks about unsaved scenes first; exitConfirmed() becomes true once
    // it's safe to leave, and exitKind() says which one was asked for.
    enum class Exit { Quit, CloseProject };
    void requestExit(Exit kind);
    bool exitConfirmed() const { return m_exitConfirmed; }
    Exit exitKind() const { return m_exitKind; }

private:
    enum class Dialog { None, Open, SaveAs, ConfirmClose, Quit, ProjectSettings };

    void defer(std::function<void()> action) { m_deferred.push_back(std::move(action)); }

    // Requests (may show a dialog first); the structural part runs deferred.
    void requestNew();
    void requestOpen();
    void requestSave(Document& doc);
    void requestSaveAs(Document& doc, bool closeAfterSave);
    void requestClose(Document& doc);

    // Exports every scene in the project to <project>/export as C (see SceneExporter).
    // Open tabs are exported as they are in the editor, including unsaved changes.
    // Returns false if nothing usable was exported.
    bool exportProject();

    // Play: export, then build the release with the exported Makefile and run it.
    void play();

    // Immediate operations, only called from deferred actions or dialogs.
    void openFile(const std::filesystem::path& path);
    bool saveDocument(Document& doc, const std::filesystem::path& path);
    void closeDocument(Document* doc);
    void activate(Document* doc);
    std::unique_ptr<Document> makeUntitled();

    void drawOpenDialog();
    void drawSaveAsDialog();
    void drawConfirmCloseDialog();
    void drawQuitDialog();
    void openProjectSettings();
    void drawProjectSettingsDialog();
    void continueQuitSaves();
    void cancelQuit();

    void refreshFileList();
    void markSaved(Document& doc);
    bool computeDirty(Document& doc);
    bool isPristine(Document& doc);
    int indexOf(const Document* doc) const;
    void updateWindowTitle();

    Project m_project;
    GameRunner m_runner;
    std::filesystem::path m_dir; // the project's scenes folder
    std::vector<std::unique_ptr<Document>> m_docs;
    std::size_t m_active = 0;
    std::vector<std::function<void()>> m_deferred;
    int m_dirtyCheckCountdown = 0;
    std::string m_appliedTitle;

    // Tab dragging: the pressed tab becomes a drag once the mouse moves sideways.
    Document* m_dragTab = nullptr;
    bool m_tabDragging = false;
    float m_tabPressX = 0.0f;
    float m_tabGrabOffset = 0.0f; // mouse x inside the tab when it was pressed

    Dialog m_dialog = Dialog::None;
    Document* m_dialogDoc = nullptr; // target of Save As / Confirm Close
    bool m_closeAfterSave = false;

    // Open dialog
    std::vector<std::string> m_files;
    int m_fileSelected = -1;
    Vector2 m_fileScroll{};
    double m_lastClickTime = 0.0;
    int m_lastClickIndex = -1;

    // Save As dialog
    std::string m_saveName;

    // Project Settings dialog (edited copies, applied on Save)
    float m_settingsWidth = 0.0f;
    float m_settingsHeight = 0.0f;
    int m_settingsMode = 0;
    std::string m_settingsEasyMemory;
    ExportSettings m_settingsExport;
    PhysicsSettings m_settingsPhysics;
    std::string m_settingsBox2d;

    // Quit dialog: unsaved scenes and whether to save each one.
    struct QuitItem {
        Document* doc;
        bool save;
    };
    std::vector<QuitItem> m_quitItems;
    Vector2 m_quitScroll{};
    std::vector<Document*> m_quitSaveAsQueue; // never-saved scenes still needing a file name
    bool m_quitFlow = false;                  // Save As dialogs are part of leaving
    bool m_exitConfirmed = false;
    Exit m_exitKind = Exit::Quit;
};
