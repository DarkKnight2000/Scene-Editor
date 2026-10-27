#include "Project.h"

#include "EditorPaths.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>
#include <sstream>

namespace {
using json = nlohmann::ordered_json;

constexpr const char* kFormat = "SceneEditor.project";
constexpr int kVersion = 1;

// Forward slashes, so session files read the same on every platform.
std::string toGenericUtf8(const std::filesystem::path& path) {
    const std::u8string text = path.generic_u8string();
    return {text.begin(), text.end()};
}

std::filesystem::path fromUtf8(const std::string& text) {
    return std::u8string(text.begin(), text.end());
}

bool readJson(const std::filesystem::path& path, json& out, std::string& error) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        error = "can't open " + path.string();
        return false;
    }
    std::ostringstream text;
    text << file.rdbuf();
    out = json::parse(text.str(), nullptr, /*allow_exceptions=*/false);
    if (out.is_discarded() || !out.is_object()) {
        error = "not valid JSON: " + path.string();
        return false;
    }
    return true;
}

// Writes via a temporary file so a failed write never leaves a half-written file.
bool writeJson(const std::filesystem::path& path, const json& j, std::string& error) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
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

constexpr const char* kPolicyKeys[] = {"always", "ifMissing", "never"};

const char* policyKey(ExportPolicy policy) {
    return kPolicyKeys[static_cast<int>(policy)];
}

void readPolicy(const json& j, const char* key, ExportPolicy& out) {
    const std::string value = j.value(key, std::string());
    for (int i = 0; i < 3; ++i) {
        if (value == kPolicyKeys[i]) {
            out = static_cast<ExportPolicy>(i);
        }
    }
}

json exportJson(const ExportSettings& e) {
    return {{"makefile", policyKey(e.makefile)},
            {"projectMain", policyKey(e.projectMain)},
            {"vscodeSettings", policyKey(e.vscodeSettings)},
            {"gitignore", policyKey(e.gitignore)}};
}

json projectJson(const Project& project) {
    return {{"format", kFormat},
            {"version", kVersion},
            {"name", project.name},
            {"scenesFolder", project.scenesFolder},
            {"display",
             {{"width", project.display.width},
              {"height", project.display.height},
              {"scaleMode", scaleModeName(project.display.scaleMode)}}},
            {"export", exportJson(project.exportSettings)},
            {"physics",
             {{"enabled", project.physics.enabled},
              {"pixelsPerMeter", project.physics.pixelsPerMeter},
              {"gravity", {project.physics.gravityX, project.physics.gravityY}},
              {"box2dDir", project.box2dDir}}}};
}

std::filesystem::path sessionPath(const Project& project) {
    return project.root() / ".sceneeditor" / "session.json";
}
} // namespace

const char* exportPolicyLabel(ExportPolicy policy) {
    switch (policy) {
    case ExportPolicy::IfMissing: return "Only if missing";
    case ExportPolicy::Never: return "Never";
    case ExportPolicy::Always: break;
    }
    return "Always";
}

std::string Project::defaultBox2dDir() {
    return SCENE_EDITOR_BOX2D_DIR;
}

std::filesystem::path Project::box2dPath() const {
    const std::filesystem::path dir = fromUtf8(box2dDir);
    return dir.is_absolute() ? dir : (root() / dir).lexically_normal();
}

const char* scaleModeName(ScaleMode mode) {
    switch (mode) {
    case ScaleMode::Fit: return "Fit";
    case ScaleMode::Fill: return "Fill";
    case ScaleMode::Expand: break;
    }
    return "Expand";
}

std::string validateProjectName(const std::string& name) {
    if (name.empty()) {
        return "Enter a project name.";
    }
    if (name.front() == ' ' || name.back() == ' ' || name.back() == '.') {
        return "The name can't start or end with a space, or end with a dot.";
    }
    if (name.find_first_of("\\/:*?\"<>|") != std::string::npos) {
        return "The name can't contain \\ / : * ? \" < > |";
    }
    return {};
}

bool loadProject(const std::filesystem::path& path, Project& project, std::string& error) {
    std::error_code ec;
    std::filesystem::path file = path;
    if (std::filesystem::is_directory(path, ec)) {
        file.clear();
        for (const auto& entry : std::filesystem::directory_iterator(path, ec)) {
            if (entry.is_regular_file() && entry.path().extension() == kProjectExtension) {
                file = entry.path();
                break;
            }
        }
        if (file.empty()) {
            error = "no " + std::string(kProjectExtension) + " file in " + path.string();
            return false;
        }
    }
    if (!std::filesystem::is_regular_file(file, ec)) {
        error = "project not found: " + file.string();
        return false;
    }

    json j;
    if (!readJson(file, j, error)) {
        return false;
    }
    if (j.value("format", "") != kFormat) {
        error = "not a SceneEditor project: " + file.string();
        return false;
    }
    if (const int version = j.value("version", 0); version != kVersion) {
        error = "unsupported project version " + std::to_string(version);
        return false;
    }

    Project loaded;
    loaded.file = std::filesystem::absolute(file, ec).lexically_normal();
    loaded.name = j.value("name", file.stem().string());
    loaded.scenesFolder = j.value("scenesFolder", std::string("scenes"));
    if (const auto display = j.find("display"); display != j.end() && display->is_object()) {
        loaded.display.width = std::clamp(display->value("width", loaded.display.width), 16, 16384);
        loaded.display.height = std::clamp(display->value("height", loaded.display.height), 16, 16384);
        const std::string mode = display->value("scaleMode", std::string("Expand"));
        for (const ScaleMode candidate : kScaleModes) {
            if (mode == scaleModeName(candidate)) {
                loaded.display.scaleMode = candidate;
            }
        }
    }
    if (const auto e = j.find("export"); e != j.end() && e->is_object()) {
        ExportSettings& settings = loaded.exportSettings;
        readPolicy(*e, "makefile", settings.makefile);
        readPolicy(*e, "projectMain", settings.projectMain);
        readPolicy(*e, "vscodeSettings", settings.vscodeSettings);
        readPolicy(*e, "gitignore", settings.gitignore);
    }
    if (const auto p = j.find("physics"); p != j.end() && p->is_object()) {
        PhysicsSettings& physics = loaded.physics;
        physics.enabled = p->value("enabled", physics.enabled);
        physics.pixelsPerMeter = std::clamp(p->value("pixelsPerMeter", physics.pixelsPerMeter), 0.001f, 100000.0f);
        if (const auto g = p->find("gravity"); g != p->end() && g->is_array() && g->size() == 2 && (*g)[0].is_number() &&
                                              (*g)[1].is_number()) {
            physics.gravityX = (*g)[0].get<float>();
            physics.gravityY = (*g)[1].get<float>();
        }
        const std::string dir = p->value("box2dDir", std::string());
        if (!dir.empty()) {
            loaded.box2dDir = dir;
        }
    }
    std::filesystem::create_directories(loaded.scenesDir(), ec);
    project = std::move(loaded);
    return true;
}

bool createProject(const std::filesystem::path& location, const std::string& name, Project& project,
                   std::string& error) {
    if (const std::string problem = validateProjectName(name); !problem.empty()) {
        error = problem;
        return false;
    }
    if (location.empty()) {
        error = "Choose a location.";
        return false;
    }

    std::error_code ec;
    const std::filesystem::path root = std::filesystem::absolute(location / fromUtf8(name), ec).lexically_normal();
    if (std::filesystem::exists(root, ec) && !std::filesystem::is_empty(root, ec)) {
        error = "The folder already exists and isn't empty: " + root.string();
        return false;
    }

    Project created;
    created.file = root / (name + kProjectExtension);
    created.name = name;
    std::filesystem::create_directories(created.scenesDir(), ec);
    if (ec) {
        error = "Can't create " + created.scenesDir().string() + ": " + ec.message();
        return false;
    }
    if (!writeJson(created.file, projectJson(created), error)) {
        return false;
    }
    {
        // Per-user state stays out of version control; the export has its own .gitignore.
        std::ofstream gitignore(root / ".gitignore", std::ios::binary);
        gitignore << "# Per-user editor state (open tabs)\n.sceneeditor/\n";
    }
    project = std::move(created);
    return true;
}

bool saveProject(const Project& project, std::string& error) {
    return writeJson(project.file, projectJson(project), error);
}

ProjectSession loadProjectSession(const Project& project) {
    ProjectSession session;
    json j;
    std::string error;
    std::error_code ec;
    if (!std::filesystem::exists(sessionPath(project), ec) || !readJson(sessionPath(project), j, error)) {
        return session;
    }
    if (const auto open = j.find("openScenes"); open != j.end() && open->is_array()) {
        for (const json& entry : *open) {
            if (entry.is_string()) {
                const std::filesystem::path stored = fromUtf8(entry.get<std::string>());
                session.openScenes.push_back(stored.is_absolute() ? stored : (project.root() / stored).lexically_normal());
            }
        }
    }
    if (const auto active = j.find("activeScene"); active != j.end() && active->is_number_integer()) {
        session.activeScene = active->get<int>();
    }
    return session;
}

bool saveProjectSession(const Project& project, const ProjectSession& session, std::string& error) {
    json open = json::array();
    for (const auto& scene : session.openScenes) {
        // Relative to the project so the folder can be moved or shared.
        const std::filesystem::path relative = scene.lexically_relative(project.root());
        const bool inside = !relative.empty() && *relative.begin() != "..";
        open.push_back(toGenericUtf8(inside ? relative : scene));
    }
    const json j{{"openScenes", std::move(open)}, {"activeScene", session.activeScene}};
    return writeJson(sessionPath(project), j, error);
}
