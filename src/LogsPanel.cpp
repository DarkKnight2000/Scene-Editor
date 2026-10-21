#include "LogsPanel.h"

#include "Log.h"
#include "Widgets.h"

#include <raygui.h>

#include <algorithm>

namespace {
constexpr float kLineHeight = 24.0f;
constexpr Color kWarningColor{230, 200, 80, 255};
constexpr Color kErrorColor{235, 100, 100, 255};
} // namespace

void LogsPanel::draw(Rectangle panel, Rectangle content) {
    const auto& entries = logEntries();
    const float contentHeight = kLineHeight * static_cast<float>(entries.size()) + 8.0f;

    // Follow new messages.
    if (logRevision() != m_seenRevision) {
        m_seenRevision = logRevision();
        m_scroll.y = std::min(0.0f, content.height - 2.0f - contentHeight);
    }

    Rectangle view{};
    GuiScrollPanel(content, nullptr, {0.0f, 0.0f, content.width - 16.0f, contentHeight}, &m_scroll, &view);

    ui::pushClip(view);
    const float top = view.y + m_scroll.y + 4.0f;
    const int first = std::max(0, static_cast<int>((view.y - top) / kLineHeight));
    for (int i = first; i < static_cast<int>(entries.size()); ++i) {
        const float y = top + kLineHeight * static_cast<float>(i);
        if (y > view.y + view.height) {
            break;
        }
        const LogEntry& entry = entries[static_cast<std::size_t>(i)];
        Color color = ui::styleColor(TEXT_COLOR_NORMAL);
        int icon = ICON_INFO;
        if (entry.level == LogLevel::Warning) {
            color = kWarningColor;
            icon = ICON_WARNING;
        } else if (entry.level == LogLevel::Error) {
            color = kErrorColor;
            icon = ICON_CROSS;
        }
        GuiDrawIcon(icon, static_cast<int>(view.x + 6.0f), static_cast<int>(y + (kLineHeight - 16.0f) / 2.0f), 1, color);
        ui::label({view.x + 28.0f, y, 90.0f, kLineHeight}, entry.time.c_str(), ui::styleColor(TEXT_COLOR_DISABLED));
        ui::label({view.x + 118.0f, y, view.width - 124.0f, kLineHeight}, entry.text.c_str(), color);
    }
    ui::popClip();

    // Clear button in the header.
    const Rectangle clearButton{panel.x + panel.width - 24.0f, panel.y + 2.0f, 20.0f, 20.0f};
    if (!entries.empty() && ui::iconButton(clearButton, ICON_BIN, kErrorColor)) {
        clearLog();
    }
}
