#include "ProjectLauncher.h"

#include "Theme.h"
#include "Widgets.h"

#include <raygui.h>

#include <algorithm>
#include <cmath>

namespace {
constexpr float kPadding = 24.0f;
constexpr float kButtonHeight = 36.0f;
constexpr float kRowHeight = 58.0f;
constexpr Color kErrorColor{235, 100, 100, 255};
constexpr Color kRemoveColor{235, 100, 100, 255};

std::string toUtf8(const std::filesystem::path& path) {
    const std::u8string text = path.u8string();
    return {text.begin(), text.end()};
}

std::filesystem::path fromUtf8(const std::string& text) {
    return std::u8string(text.begin(), text.end());
}

void heading(float x, float y, const char* text, float size, Color color) {
    ui::drawText(size > theme::kFontSize ? theme::title() : theme::font(), text, {x, y}, size, color);
}
} // namespace

ProjectLauncher::ProjectLauncher(EditorConfig& config) : m_config(config) {
    refreshRecent();
}

void ProjectLauncher::refreshRecent() {
    m_recent.clear();
    for (const auto& file : m_config.recentProjects) {
        RecentEntry entry;
        entry.file = file;
        Project project;
        std::string error;
        if (loadProject(file, project, error)) {
            entry.name = project.name;
        } else {
            entry.name = file.stem().string();
            entry.missing = true;
        }
        m_recent.push_back(std::move(entry));
    }
}

void ProjectLauncher::showView(View view) {
    m_view = view;
    m_error.clear();
    if (view == View::New) {
        m_name = "MyGame";
        const std::filesystem::path location =
            m_config.newProjectLocation.empty() ? defaultProjectsLocation() : m_config.newProjectLocation;
        m_location = toUtf8(location);
        ui::focusTextField(m_name);
    } else if (view == View::Open) {
        m_openPath.clear();
        ui::focusTextField(m_openPath);
    }
}

std::optional<Project> ProjectLauncher::open(const std::filesystem::path& path) {
    Project project;
    if (!loadProject(path, project, m_error)) {
        return std::nullopt;
    }
    m_error.clear();
    m_config.addRecentProject(project.file);
    return project;
}

std::optional<Project> ProjectLauncher::draw(Rectangle area) {
    DrawRectangleRec(area, ui::styleColor(BACKGROUND_COLOR));

    // Header
    heading(area.x + kPadding, area.y + 24.0f, "Scene Editor", theme::kTitleFontSize, theme::kText);
    heading(area.x + kPadding, area.y + 64.0f, "Open a recent project or start a new one.", theme::kFontSize,
            theme::kTextDim);
    const Rectangle body{area.x + kPadding, area.y + 110.0f, area.width - 2.0f * kPadding,
                         area.height - 110.0f - kPadding};

    switch (m_view) {
    case View::Recent: return drawRecent(body);
    case View::New: return drawNew(body);
    case View::Open: return drawOpen(body);
    }
    return std::nullopt;
}

std::optional<Project> ProjectLauncher::drawRecent(Rectangle area) {
    std::optional<Project> result;
    const float buttonsWidth = 240.0f;
    const Rectangle list{area.x, area.y + 30.0f, area.width - buttonsWidth - kPadding, area.height - 30.0f - 30.0f};

    ui::label({area.x, area.y, list.width, 26.0f}, "Recent projects", ui::styleColor(TEXT_COLOR_NORMAL));

    Rectangle view{};
    const float contentHeight = kRowHeight * static_cast<float>(m_recent.size());
    GuiScrollPanel(list, nullptr, {0.0f, 0.0f, list.width - 16.0f, contentHeight}, &m_scroll, &view);
    if (m_recent.empty()) {
        ui::label({list.x + 14.0f, list.y + 10.0f, list.width - 28.0f, 26.0f}, "No recent projects yet.",
                  ui::styleColor(TEXT_COLOR_DISABLED));
    }

    const Vector2 mouse = GetMousePosition();
    ui::setInteractive(CheckCollisionPointRec(mouse, view));
    ui::pushClip(view);
    int removeIndex = -1;
    for (std::size_t i = 0; i < m_recent.size(); ++i) {
        const RecentEntry& entry = m_recent[i];
        const Rectangle row{view.x, view.y + m_scroll.y + kRowHeight * static_cast<float>(i), view.width, kRowHeight};
        const Rectangle removeButton{row.x + row.width - 34.0f, row.y + (kRowHeight - 24.0f) / 2.0f, 24.0f, 24.0f};
        const bool hot = ui::hovered(row);
        const bool overRemove = CheckCollisionPointRec(mouse, removeButton);
        if (hot) {
            ui::fill({row.x + 2.0f, row.y + 1.0f, row.width - 4.0f, row.height - 2.0f}, theme::kFieldHover);
        }

        const Color nameColor = entry.missing ? theme::kTextFaint : theme::kText;
        GuiDrawIcon(ICON_FOLDER_OPEN, static_cast<int>(row.x + 12.0f), static_cast<int>(row.y + 10.0f), 1,
                    entry.missing ? theme::kTextFaint : theme::kAccentHover);
        const std::string name = entry.missing ? entry.name + "  (missing)" : entry.name;
        ui::label({row.x + 38.0f, row.y + 4.0f, row.width - 80.0f, 26.0f}, name.c_str(), nameColor);
        const std::string location = toUtf8(entry.file.parent_path());
        ui::label({row.x + 38.0f, row.y + 28.0f, row.width - 80.0f, 24.0f}, location.c_str(),
                  ui::styleColor(TEXT_COLOR_DISABLED));

        if (hot && ui::iconButton(removeButton, ICON_CROSS_SMALL, kRemoveColor)) {
            removeIndex = static_cast<int>(i); // remove from the list only; files are untouched
        } else if (!overRemove && ui::clicked(row)) {
            result = open(entry.file);
        }
    }
    ui::popClip();
    ui::setInteractive(true);

    if (removeIndex >= 0) {
        std::erase(m_config.recentProjects, m_recent[static_cast<std::size_t>(removeIndex)].file);
        refreshRecent();
    }

    // Actions
    const float bx = area.x + area.width - buttonsWidth;
    if (ui::button({bx, list.y, buttonsWidth, kButtonHeight}, GuiIconText(ICON_FILE_NEW, "New Project"))) {
        showView(View::New);
    }
    if (ui::button({bx, list.y + kButtonHeight + 10.0f, buttonsWidth, kButtonHeight},
                   GuiIconText(ICON_FOLDER_FILE_OPEN, "Open Project..."))) {
        showView(View::Open);
    }

    if (!m_error.empty()) {
        ui::label({area.x, area.y + area.height - 26.0f, area.width, 26.0f}, m_error.c_str(), kErrorColor);
    }
    return result;
}

std::optional<Project> ProjectLauncher::drawNew(Rectangle area) {
    std::optional<Project> result;
    ui::label({area.x, area.y, area.width, 26.0f}, "Create a new project", ui::styleColor(TEXT_COLOR_NORMAL));

    const float labelWidth = 110.0f;
    const float fieldX = area.x + labelWidth;
    const float fieldWidth = area.width - labelWidth;
    ui::label({area.x, area.y + 50.0f, labelWidth, 30.0f}, "Name", ui::styleColor(TEXT_COLOR_NORMAL));
    ui::textField({fieldX, area.y + 50.0f, fieldWidth, 30.0f}, m_name);
    ui::label({area.x, area.y + 96.0f, labelWidth, 30.0f}, "Location", ui::styleColor(TEXT_COLOR_NORMAL));
    ui::textField({fieldX, area.y + 96.0f, fieldWidth, 30.0f}, m_location);

    const std::string problem = validateProjectName(m_name);
    const std::filesystem::path root = fromUtf8(m_location) / fromUtf8(m_name);
    const std::string preview = problem.empty()
                                    ? "Creates " + toUtf8(root / (m_name + kProjectExtension))
                                    : problem;
    ui::label({fieldX, area.y + 136.0f, fieldWidth, 26.0f}, preview.c_str(),
              problem.empty() ? ui::styleColor(TEXT_COLOR_DISABLED) : kErrorColor);
    ui::label({fieldX, area.y + 162.0f, fieldWidth, 26.0f}, "with a scenes folder inside.",
              ui::styleColor(TEXT_COLOR_DISABLED));

    const float by = area.y + area.height - kButtonHeight - 30.0f;
    bool create = false;
    const Rectangle createButton{area.x + area.width - 160.0f, by, 160.0f, kButtonHeight};
    if (problem.empty() && !m_location.empty()) {
        create = ui::button(createButton, "Create", ui::ButtonKind::Primary) || (IsKeyPressed(KEY_ENTER) && !ui::isTextEditing());
    } else {
        GuiDisable();
        ui::button(createButton, "Create", ui::ButtonKind::Primary);
        GuiEnable();
    }
    if (ui::button({area.x + area.width - 330.0f, by, 160.0f, kButtonHeight}, "Back") ||
        (IsKeyPressed(KEY_ESCAPE) && !ui::isTextEditing())) {
        showView(View::Recent);
        return std::nullopt;
    }

    if (create) {
        Project project;
        if (createProject(fromUtf8(m_location), m_name, project, m_error)) {
            m_config.newProjectLocation = fromUtf8(m_location);
            m_config.addRecentProject(project.file);
            result = project;
        }
    }
    if (!m_error.empty()) {
        ui::label({area.x, area.y + area.height - 26.0f, area.width, 26.0f}, m_error.c_str(), kErrorColor);
    }
    return result;
}

std::optional<Project> ProjectLauncher::drawOpen(Rectangle area) {
    std::optional<Project> result;
    ui::label({area.x, area.y, area.width, 26.0f}, "Open a project", ui::styleColor(TEXT_COLOR_NORMAL));
    ui::label({area.x, area.y + 50.0f, area.width, 26.0f},
              std::string("Path to a project folder or its ").append(kProjectExtension).append(" file:").c_str(),
              ui::styleColor(TEXT_COLOR_DISABLED));
    ui::textField({area.x, area.y + 82.0f, area.width, 30.0f}, m_openPath);

    const float by = area.y + area.height - kButtonHeight - 30.0f;
    if (ui::button({area.x + area.width - 160.0f, by, 160.0f, kButtonHeight}, "Open", ui::ButtonKind::Primary) ||
        (IsKeyPressed(KEY_ENTER) && !ui::isTextEditing() && !m_openPath.empty())) {
        // Accept paths pasted with surrounding quotes.
        std::string path = m_openPath;
        if (path.size() >= 2 && path.front() == '"' && path.back() == '"') {
            path = path.substr(1, path.size() - 2);
        }
        result = open(fromUtf8(path));
    }
    if (ui::button({area.x + area.width - 330.0f, by, 160.0f, kButtonHeight}, "Back") ||
        (IsKeyPressed(KEY_ESCAPE) && !ui::isTextEditing())) {
        showView(View::Recent);
        return std::nullopt;
    }
    if (!m_error.empty()) {
        ui::label({area.x, area.y + area.height - 26.0f, area.width, 26.0f}, m_error.c_str(), kErrorColor);
    }
    return result;
}
