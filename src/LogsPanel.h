#pragma once

#include <raylib.h>

#include <cstddef>
#include <filesystem>
#include <string>

// The editor log: a one-line status bar at the bottom of the window (the latest message,
// and warning / error counts), and the Logs panel it opens with the whole log, newest at
// the bottom. Paths inside the project are shown relative to it.
class LogsPanel {
public:
    // Paths under `root` are shown relative to it.
    void setProjectRoot(const std::filesystem::path& root);

    // The Logs panel (when open).
    void draw(Rectangle panel, Rectangle content);

    // The status bar. Clicking it toggles `open`; a new error opens the panel.
    void drawStatusBar(Rectangle bar, bool& open);

private:
    std::string shorten(const std::string& text) const;

    Vector2 m_scroll{};
    std::size_t m_seenRevision = 0;
    std::size_t m_statusRevision = 0;
    std::size_t m_seenCount = 0; // entries already looked at for new errors
    std::string m_root;          // project folder, with a trailing separator
    std::string m_rootSlashes;   // the same with forward slashes
};
