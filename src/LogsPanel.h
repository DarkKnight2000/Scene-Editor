#pragma once

#include <raylib.h>

#include <cstddef>

// Shows the editor log, newest at the bottom, with a clear button in the header.
class LogsPanel {
public:
    void draw(Rectangle panel, Rectangle content);

private:
    Vector2 m_scroll{};
    std::size_t m_seenRevision = 0;
};
