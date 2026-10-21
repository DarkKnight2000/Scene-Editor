#pragma once

#include <raylib.h>

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "EditorConfig.h"
#include "Project.h"

// Start screen: recent projects, New Project and Open Project. Shown in a small window
// when no project was open at the last exit, or after Close Project.
class ProjectLauncher {
public:
    explicit ProjectLauncher(EditorConfig& config);

    // Draws the launcher into `area`. Returns the project once the user opens or creates
    // one; it's also moved to the front of the config's recent list.
    std::optional<Project> draw(Rectangle area);

private:
    enum class View { Recent, New, Open };

    struct RecentEntry {
        std::filesystem::path file;
        std::string name;
        bool missing = false;
    };

    void refreshRecent();
    std::optional<Project> open(const std::filesystem::path& path);
    void showView(View view);

    std::optional<Project> drawRecent(Rectangle area);
    std::optional<Project> drawNew(Rectangle area);
    std::optional<Project> drawOpen(Rectangle area);

    EditorConfig& m_config;
    std::vector<RecentEntry> m_recent;
    View m_view = View::Recent;
    Vector2 m_scroll{};
    std::string m_error;

    // Form fields (stable addresses for ui::textField).
    std::string m_name;
    std::string m_location;
    std::string m_openPath;
};
