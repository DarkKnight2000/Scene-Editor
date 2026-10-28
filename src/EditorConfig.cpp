#include "EditorConfig.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <type_traits>

namespace {
using json = nlohmann::ordered_json;

constexpr int kVersion = 1;

std::string environment(const char* name) {
#ifdef _WIN32
    char* value = nullptr;
    std::size_t length = 0;
    std::string result;
    if (_dupenv_s(&value, &length, name) == 0 && value != nullptr) {
        result = value;
    }
    std::free(value);
    return result;
#else
    const char* value = std::getenv(name);
    return value ? value : "";
#endif
}

// Paths are stored as UTF-8 strings.
std::string toUtf8(const std::filesystem::path& path) {
    const std::u8string text = path.u8string();
    return {text.begin(), text.end()};
}

std::filesystem::path fromUtf8(const std::string& text) {
    return std::u8string(text.begin(), text.end());
}

template <typename T>
void read(const json& j, const char* key, T& out) {
    const auto it = j.find(key);
    if (it == j.end()) {
        return;
    }
    if constexpr (std::is_same_v<T, bool>) {
        if (it->is_boolean()) {
            out = it->get<bool>();
        }
    } else {
        if (it->is_number()) {
            out = it->get<T>();
        }
    }
}
} // namespace

std::filesystem::path configPath() {
    // Override, e.g. for tests or a throwaway session that shouldn't touch the real config.
    if (const std::string custom = environment("SCENE_EDITOR_CONFIG"); !custom.empty()) {
        return fromUtf8(custom);
    }
    std::filesystem::path base;
#ifdef _WIN32
    base = environment("APPDATA");
#else
    base = environment("XDG_CONFIG_HOME");
    if (base.empty() && !environment("HOME").empty()) {
        base = std::filesystem::path(environment("HOME")) / ".config";
    }
#endif
    if (base.empty()) {
        base = std::filesystem::current_path();
    }
    return base / "SceneEditor" / "editor.json";
}

std::filesystem::path defaultProjectsLocation() {
#ifdef _WIN32
    std::filesystem::path home = environment("USERPROFILE");
#else
    std::filesystem::path home = environment("HOME");
#endif
    if (home.empty()) {
        home = std::filesystem::current_path();
    }
    return home / "Documents" / "SceneEditor Projects";
}

bool loadEditorConfig(const std::filesystem::path& path, EditorConfig& config, std::string& error) {
    if (!std::filesystem::exists(path)) {
        return true; // first run: defaults
    }
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        error = "can't open " + path.string();
        return false;
    }
    std::ostringstream text;
    text << file.rdbuf();
    const json j = json::parse(text.str(), nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded() || !j.is_object()) {
        error = "not valid JSON";
        return false;
    }

    if (const auto it = j.find("window"); it != j.end() && it->is_object()) {
        read(*it, "maximized", config.window.maximized);
        read(*it, "fullscreen", config.window.fullscreen);
        read(*it, "x", config.window.x);
        read(*it, "y", config.window.y);
        read(*it, "width", config.window.width);
        read(*it, "height", config.window.height);
    }
    if (const auto it = j.find("layout"); it != j.end() && it->is_object()) {
        read(*it, "entitiesWidth", config.layout.entitiesWidth);
        read(*it, "detailsWidth", config.layout.detailsWidth);
        read(*it, "logsHeight", config.layout.logsHeight);
        read(*it, "logsOpen", config.layout.logsOpen);
    }
    read(j, "uiScale", config.uiScale);
    if (const auto it = j.find("projects"); it != j.end() && it->is_object()) {
        if (const auto recent = it->find("recent"); recent != it->end() && recent->is_array()) {
            for (const json& entry : *recent) {
                if (entry.is_string() && config.recentProjects.size() < EditorConfig::kMaxRecent) {
                    config.recentProjects.push_back(fromUtf8(entry.get<std::string>()));
                }
            }
        }
        if (const auto last = it->find("last"); last != it->end() && last->is_string()) {
            config.lastProject = fromUtf8(last->get<std::string>());
        }
        if (const auto location = it->find("newProjectLocation"); location != it->end() && location->is_string()) {
            config.newProjectLocation = fromUtf8(location->get<std::string>());
        }
    }
    if (const auto it = j.find("play"); it != j.end() && it->is_object()) {
        const auto readText = [&](const char* key, std::string& out) {
            if (const auto value = it->find(key); value != it->end() && value->is_string()) {
                out = value->get<std::string>();
            }
        };
        readText("make", config.play.make);
        readText("cc", config.play.cc);
        readText("cflags", config.play.cflags);
        readText("raylibSource", config.play.raylibSource);
        readText("raylibInclude", config.play.raylibInclude);
        readText("raylibLibs", config.play.raylibLibs);
    }
    return true;
}

void EditorConfig::addRecentProject(const std::filesystem::path& project) {
    std::erase(recentProjects, project);
    recentProjects.insert(recentProjects.begin(), project);
    if (recentProjects.size() > kMaxRecent) {
        recentProjects.resize(kMaxRecent);
    }
}

bool saveEditorConfig(const std::filesystem::path& path, const EditorConfig& config, std::string& error) {
    json recent = json::array();
    for (const auto& project : config.recentProjects) {
        recent.push_back(toUtf8(project));
    }
    const json j{
        {"version", kVersion},
        {"window",
         {{"maximized", config.window.maximized},
          {"fullscreen", config.window.fullscreen},
          {"x", config.window.x},
          {"y", config.window.y},
          {"width", config.window.width},
          {"height", config.window.height}}},
        {"layout",
         {{"entitiesWidth", config.layout.entitiesWidth},
          {"detailsWidth", config.layout.detailsWidth},
          {"logsHeight", config.layout.logsHeight},
          {"logsOpen", config.layout.logsOpen}}},
        {"uiScale", config.uiScale},
        {"projects",
         {{"recent", std::move(recent)},
          {"last", toUtf8(config.lastProject)},
          {"newProjectLocation", toUtf8(config.newProjectLocation)}}},
        {"play",
         {{"make", config.play.make},
          {"cc", config.play.cc},
          {"cflags", config.play.cflags},
          {"raylibSource", config.play.raylibSource},
          {"raylibInclude", config.play.raylibInclude},
          {"raylibLibs", config.play.raylibLibs}}},
    };

    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    // Write a temporary file first so a failed write never leaves a half-written config.
    const std::filesystem::path temp = path.string() + ".tmp";
    {
        std::ofstream file(temp, std::ios::binary | std::ios::trunc);
        if (!file) {
            error = "can't write " + temp.string();
            return false;
        }
        file << j.dump(2) << "\n";
        if (!file) {
            error = "write failed for " + temp.string();
            return false;
        }
    }
    std::filesystem::rename(temp, path, ec);
    if (ec) {
        error = "can't replace " + path.string() + ": " + ec.message();
        std::filesystem::remove(temp, ec);
        return false;
    }
    return true;
}
