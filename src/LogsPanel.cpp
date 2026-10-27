#include "LogsPanel.h"

#include "EditorLayout.h"
#include "Log.h"
#include "Theme.h"
#include "Widgets.h"

#include <raygui.h>

#include <algorithm>

namespace {
constexpr float kLineHeight = 24.0f;

Color levelColor(LogLevel level) {
    switch (level) {
    case LogLevel::Warning: return theme::kWarning;
    case LogLevel::Error: return theme::kError;
    case LogLevel::Info: break;
    }
    return theme::kText;
}

int levelIcon(LogLevel level) {
    switch (level) {
    case LogLevel::Warning: return ICON_WARNING;
    case LogLevel::Error: return ICON_CROSS;
    case LogLevel::Info: break;
    }
    return ICON_INFO;
}

void replaceAll(std::string& text, const std::string& from, const std::string& to) {
    if (from.empty()) {
        return;
    }
    for (std::size_t at = text.find(from); at != std::string::npos; at = text.find(from, at + to.size())) {
        text.replace(at, from.size(), to);
    }
}

// A count badge, e.g. the number of warnings; returns its width.
float badge(float x, float y, int icon, int count, Color color) {
    const std::string text = std::to_string(count);
    const float w = 16.0f + 4.0f + ui::textWidth(theme::font(), text.c_str(), theme::kFontSize);
    GuiDrawIcon(icon, static_cast<int>(x), static_cast<int>(y - 8.0f), 1, count > 0 ? color : theme::kTextFaint);
    ui::drawText(theme::font(), text.c_str(), {x + 20.0f, y - theme::kFontSize / 2.0f}, theme::kFontSize,
                 count > 0 ? theme::kText : theme::kTextFaint);
    return w;
}
} // namespace

void LogsPanel::setProjectRoot(const std::filesystem::path& root) {
    // The same folder may be written with either separator.
    m_root = root.empty() ? std::string() : root.string() + "/";
    std::replace(m_root.begin(), m_root.end(), '\\', '/');
    m_rootSlashes = m_root;
    std::replace(m_root.begin(), m_root.end(), '/', '\\');
}

std::string LogsPanel::shorten(const std::string& text) const {
    std::string result = text;
    replaceAll(result, m_root, "");
    replaceAll(result, m_rootSlashes, "");
    return result;
}

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
        const Color color = levelColor(entry.level);
        const float textY = y + (kLineHeight - theme::kFontSize) / 2.0f;
        GuiDrawIcon(levelIcon(entry.level), static_cast<int>(view.x + 10.0f), static_cast<int>(y + (kLineHeight - 16.0f) / 2.0f),
                    1, entry.level == LogLevel::Info ? theme::kTextFaint : color);
        ui::drawText(theme::mono(), entry.time.c_str(), {view.x + 34.0f, textY}, theme::kFontSize, theme::kTextFaint);
        ui::drawText(theme::font(), shorten(entry.text).c_str(), {view.x + 110.0f, textY}, theme::kFontSize, color);
    }
    ui::popClip();

    // Clear button in the header.
    const float size = 24.0f;
    const Rectangle clearButton{panel.x + panel.width - size - 6.0f, panel.y + (EditorLayout::kHeaderHeight - size) / 2.0f,
                                size, size};
    if (!entries.empty()) {
        if (ui::iconButton(clearButton, ICON_BIN, theme::kError)) {
            clearLog();
        }
        ui::tooltip(clearButton, "Clear the log");
    }
}

void LogsPanel::drawStatusBar(Rectangle bar, bool& open) {
    const auto& entries = logEntries();

    // A new error opens the log.
    if (logRevision() != m_statusRevision) {
        m_statusRevision = logRevision();
        for (std::size_t i = std::min(m_seenCount, entries.size()); i < entries.size(); ++i) {
            if (entries[i].level == LogLevel::Error) {
                open = true;
            }
        }
        m_seenCount = entries.size();
    }

    const bool hot = ui::hovered(bar);
    DrawRectangleRec(bar, hot ? theme::kRaised : theme::kPanel);
    DrawLineV({bar.x, bar.y + 0.5f}, {bar.x + bar.width, bar.y + 0.5f}, theme::kBorder);

    int warnings = 0;
    int errors = 0;
    for (const LogEntry& entry : entries) {
        warnings += entry.level == LogLevel::Warning;
        errors += entry.level == LogLevel::Error;
    }

    // Right: the counts, and which way the panel will go.
    const float cy = bar.y + bar.height / 2.0f;
    float x = bar.x + bar.width - theme::kSpace3 - 16.0f;
    GuiDrawIcon(open ? ICON_ARROW_DOWN : ICON_ARROW_UP, static_cast<int>(x), static_cast<int>(cy - 8.0f), 1,
                hot ? theme::kText : theme::kTextDim);
    x -= theme::kSpace4 + 40.0f;
    badge(x, cy, ICON_CROSS, errors, theme::kError);
    x -= 52.0f;
    badge(x, cy, ICON_WARNING, warnings, theme::kWarning);

    // Left: the latest message.
    if (!entries.empty()) {
        const LogEntry& last = entries.back();
        const float maxWidth = x - bar.x - 40.0f;
        ui::pushClip({bar.x, bar.y, std::max(0.0f, maxWidth), bar.height});
        GuiDrawIcon(levelIcon(last.level), static_cast<int>(bar.x + theme::kSpace3), static_cast<int>(cy - 8.0f), 1,
                    last.level == LogLevel::Info ? theme::kTextFaint : levelColor(last.level));
        ui::drawText(theme::font(), shorten(last.text).c_str(), {bar.x + theme::kSpace3 + 24.0f, cy - theme::kFontSize / 2.0f},
                     theme::kFontSize, last.level == LogLevel::Info ? theme::kTextDim : levelColor(last.level));
        ui::popClip();
    }

    ui::tooltip(bar, open ? "Hide the log" : "Show the log");
    if (ui::clicked(bar)) {
        open = !open;
    }
}
