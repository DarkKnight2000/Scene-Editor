#include "ProjectExport.h"

#include "Components.h"
#include "Log.h"
#include "SceneExporter.h"
#include "SceneSerializer.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

namespace {
std::string upperId(std::string id) {
    for (char& c : id) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return id;
}
} // namespace

std::filesystem::path projectExportDir(const Project& project) {
    return project.exportPath();
}

bool exportProject(const Project& project,
                   const std::function<const Scene*(const std::filesystem::path& file)>& openScene,
                   std::vector<ExportedScene>* exported, const std::string& startScene) {
    const std::filesystem::path outDir = projectExportDir(project);

    // Files you may edit by hand, and when Project Settings says to write them.
    const ExportSettings& options = project.exportSettings;
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
    for (const auto& entry : std::filesystem::directory_iterator(project.scenesDir(), ec)) {
        if (entry.is_regular_file() && entry.path().extension() == kSceneExtension) {
            files.push_back(entry.path());
        }
    }
    std::sort(files.begin(), files.end());

    std::vector<ExportedScene> scenes;
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

        // Open in the editor: export what it shows. Otherwise: the saved file.
        const Scene* scene = openScene ? openScene(file) : nullptr;
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

        const bool hasRigidbodies = std::any_of(scene->entities().begin(), scene->entities().end(),
                                                [](const Entity& e) { return e.rigidbody.has_value(); });
        if (!project.physics.enabled && hasRigidbodies) {
            logWarning("Export: " + file.filename().string() +
                       " has rigidbodies, but physics is off in Project Settings: they aren't exported");
        }
        ExportedScene info;
        if (!exportScene(*scene, name, outDir, project.physics.enabled, info, error)) {
            logError("Export: " + file.filename().string() + ": " + error);
            ++failed;
            continue;
        }
        identifiers.push_back(id);
        logInfo("Exported " + id + ".h/.c  (" + info.type + ", SCENE_ID_" + upperId(id) + ")");
        scenes.push_back(std::move(info));
    }

    std::string error;
    bool usable = !scenes.empty();
    if (usable && !(writeSceneCommon(outDir, project.display, project.physics, error) &&
                    writeSceneManager(scenes, outDir, startScene, error) &&
                    (!editable[1].write || writeProjectMain(project.name, outDir, error)) &&
                    (!editable[0].write || writeMakefile(project.name, outDir, error)) &&
                    (!editable[2].write || writeVSCodeFiles(outDir, project.name, error)) &&
                    (!editable[3].write || writeGitignore(outDir, project.name, error)))) {
        logError("Export: " + error);
        ++failed;
        usable = false;
    }
    if (!notWritten.empty()) {
        logInfo("Export: not written, as set in Project Settings: " + notWritten);
    }
    if (usable && project.physics.enabled && !writeBox2D(outDir, project.box2dPath(), error)) {
        logError("Export: " + error + ". Set the Box2D path in Project Settings, or turn physics off.");
        ++failed;
        usable = false; // the export can't build without it
    }
    if (files.empty()) {
        logWarning("Export: the project has no saved scenes yet");
    } else {
        logInfo("Export finished: " + std::to_string(scenes.size()) + " scene(s) to " + outDir.string() +
                (failed > 0 ? " (" + std::to_string(failed) + " failed)" : ""));
    }
    if (exported != nullptr) {
        *exported = std::move(scenes);
    }
    return usable;
}
