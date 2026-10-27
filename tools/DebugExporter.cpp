// Internal tool for checking the exporter (not part of what users get): exports a project
// like `SceneEditor --export`, then adds debug/ to the export with checks that only make
// sense while developing the exporter:
//
//   debug/debug_scenes.h/.c   debug_check(): every pose and UI rect is finite;
//                             debug_draw_colliders(): outlines of the current scene's colliders;
//                             debug_scene_names[]
//   debug/debug_main.c        --frames N: draws every scene (with collider outlines), then
//                             runs debug_check; exit code 1 on failure.
//                             --shots PREFIX: renders every scene, and again after 2 s of
//                             physics, to PREFIX_<scene>.png and PREFIX_<scene>_after.png
//
// Build and run it from the export folder with the export's own Makefile, swapping src/ for
// debug/ and warnings for errors:
//
//   make BIN_DIR=bin/debug TARGET_NAME=debug INCLUDE_DIRS="debug generated utils"
//        CFLAGS="-std=c11 -O2 -Wall -Wextra -Wpedantic -Werror" RAYLIB_SRC=...
//   bin/debug/debug --frames 210
//
// The editor's own export removes debug/ again (its files carry the generated marker).
//
// It also loads and saves every scene twice in memory: the second save must match the first
// (exit code 1 if not). The saved scenes go to debug/scenes/, to compare editor versions.

#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "Log.h"
#include "Project.h"
#include "ProjectExport.h"
#include "SceneExporter.h"
#include "SceneSerializer.h"

namespace {

bool write(const std::filesystem::path& path, const std::string& text) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file << text;
    if (!file) {
        std::fprintf(stderr, "can't write %s\n", path.string().c_str());
        return false;
    }
    return true;
}

std::string upperId(std::string id) {
    for (char& c : id) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return id;
}

std::string debugScenes(const std::vector<ExportedScene>& scenes) {
    std::ostringstream names;
    std::ostringstream check;
    std::ostringstream colliders;
    for (const ExportedScene& s : scenes) {
        const std::string scene = "m->" + s.member;
        names << "    \"" << s.name << "\",\n";
        for (const std::string& e : s.entities) {
            check << "    bad += bad_pose(" << scene << "." << e << ".pose);\n";
        }
        for (const std::string& e : s.elements) {
            check << "    bad += bad_rect(" << scene << "." << e << ".rect);\n";
        }
        if (!s.colliders.empty()) {
            colliders << "    case SCENE_ID_" << upperId(s.id) << ":\n";
            for (const std::string& e : s.colliders) {
                colliders << "        draw_box(scene_box(" << scene << "." << e << ".pose, " << scene << "." << e
                          << ".collider.offset, " << scene << "." << e << ".collider.size), color);\n";
            }
            colliders << "        break;\n";
        }
    }
    return std::string(kGeneratedMarker) + R"(: debug checks (SceneDebugExport). Not part of the game.
#include "debug_scenes.h"

#include <math.h>

const char *const debug_scene_names[SCENE_ID_COUNT] = {
)" + names.str() + R"(};

static int bad_pose(ScenePose p) {
    return !(isfinite(p.position.x) && isfinite(p.position.y) && isfinite(p.rotation) && isfinite(p.scale.x) &&
             isfinite(p.scale.y));
}

static int bad_rect(Rectangle r) {
    return !(isfinite(r.x) && isfinite(r.y) && isfinite(r.width) && isfinite(r.height));
}

static void draw_box(SceneBox box, Color color) {
    const float c = cosf(box.rotation * DEG2RAD);
    const float s = sinf(box.rotation * DEG2RAD);
    const float xs[4] = {-0.5f, 0.5f, 0.5f, -0.5f};
    const float ys[4] = {-0.5f, -0.5f, 0.5f, 0.5f};
    Vector2 corners[4];
    int i;
    for (i = 0; i < 4; ++i) {
        const float x = xs[i] * box.size.x;
        const float y = ys[i] * box.size.y;
        corners[i] = (Vector2){box.center.x + x * c - y * s, box.center.y + x * s + y * c};
    }
    for (i = 0; i < 4; ++i) {
        DrawLineEx(corners[i], corners[(i + 1) % 4], 2.0f, color);
    }
}

int debug_check(const SceneManager *m) {
    int bad = 0;
)" + check.str() + R"(    (void)m;
    (void)bad_pose;
    (void)bad_rect;
    return bad;
}

void debug_draw_colliders(const SceneManager *m, Color color) {
    switch (m->current) {
)" + colliders.str() + R"(    default: break;
    }
    (void)draw_box;
    (void)color;
}
)";
}

constexpr const char* kDebugHeader = R"(: debug checks (SceneDebugExport). Not part of the game.
#ifndef DEBUG_SCENES_H
#define DEBUG_SCENES_H

#include "scene_manager.h"

extern const char *const debug_scene_names[SCENE_ID_COUNT];

/* Number of entity poses and UI rects that aren't finite, in every scene. */
int debug_check(const SceneManager *m);

/* Outlines of the current scene's box colliders (between scene_begin and scene_end). */
void debug_draw_colliders(const SceneManager *m, Color color);

#endif
)";

constexpr const char* kDebugMain = R"(: debug runs (SceneDebugExport). Not part of the game.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <raylib.h>
#include <rlgl.h>

#include "debug_scenes.h"

static void frame(SceneManager *m, int colliders) {
    BeginDrawing();
    ClearBackground((Color){20, 20, 23, 255});
    scene_begin();
    scene_manager_draw(m);
    if (colliders) {
        debug_draw_colliders(m, GREEN);
    }
    scene_end();
    scene_manager_draw_ui(m);
}

/* The second of two frames, saved as a PNG. */
static void shot(SceneManager *m, const char *path) {
    Image image;
    frame(m, 0);
    EndDrawing();
    frame(m, 0);
    rlDrawRenderBatchActive(); /* the UI is still batched */
    image = LoadImageFromScreen();
    EndDrawing();
    ExportImage(image, path);
    UnloadImage(image);
}

int main(int argc, char **argv) {
    static SceneManager scenes;
    const char *prefix = NULL;
    int frames = -1;
    int bad;
    if (argc == 3 && strcmp(argv[1], "--frames") == 0) {
        frames = atoi(argv[2]);
    } else if (argc == 3 && strcmp(argv[1], "--shots") == 0) {
        prefix = argv[2];
    } else {
        fprintf(stderr, "usage: %s --frames N | --shots PREFIX\n", argv[0]);
        return 2;
    }
    SetTraceLogLevel(LOG_WARNING);
    if (prefix != NULL) {
        int i, step;
        InitWindow(1100, 650, "debug shots"); /* not the design's shape: scaling matters */
        scene_manager_init(&scenes, 0);
        for (i = 0; i < SCENE_ID_COUNT; ++i) {
            scenes.current = i;
            shot(&scenes, TextFormat("%s_%s.png", prefix, debug_scene_names[i]));
            for (step = 0; step < 120; ++step) {
                scene_manager_update(&scenes, 1.0f / 60.0f);
            }
            shot(&scenes, TextFormat("%s_%s_after.png", prefix, debug_scene_names[i]));
        }
    } else {
        int i;
        InitWindow((int)SCENE_DESIGN_WIDTH, (int)SCENE_DESIGN_HEIGHT, "debug frames");
        scene_manager_init(&scenes, 0);
        for (i = 0; i < frames && !WindowShouldClose(); ++i) {
            scenes.current = (i / 10) % SCENE_ID_COUNT;
            scene_manager_update(&scenes, 1.0f / 60.0f);
            frame(&scenes, i % 2);
            EndDrawing();
        }
    }
    bad = debug_check(&scenes);
    scene_manager_deinit(&scenes);
    CloseWindow();
    if (bad > 0) {
        printf("debug check FAILED: %d pose(s) or rect(s) not finite\n", bad);
        return 1;
    }
    printf("debug check passed: %d scene(s)%s\n", SCENE_ID_COUNT, prefix != NULL ? ", shots saved" : "");
    return 0;
}
)";

// Loads and saves every scene file (in memory), and loads and saves the result again: both
// texts must be the same. Writes the saved text to `outDir` for comparing exporter versions.
// Returns how many scenes failed.
int roundTripScenes(const Project& project, const std::filesystem::path& outDir) {
    int failed = 0;
    std::error_code ec;
    std::filesystem::create_directories(outDir, ec);
    for (const auto& entry : std::filesystem::directory_iterator(project.scenesDir(), ec)) {
        if (entry.path().extension() != kSceneExtension) {
            continue;
        }
        Scene first;
        Scene second;
        std::string error;
        if (!loadSceneFile(entry.path(), first, error)) {
            logError("Round trip: " + entry.path().filename().string() + ": " + error);
            ++failed;
            continue;
        }
        const std::string saved = serializeScene(first);
        if (!deserializeScene(saved, second, error) || serializeScene(second) != saved) {
            logError("Round trip: " + entry.path().filename().string() + " changes when saved again " + error);
            ++failed;
        }
        write(outDir / entry.path().filename(), saved);
    }
    return failed;
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: SceneDebugExport <project>\n");
        return 2;
    }
    setLogToConsole(true);
    Project project;
    std::string error;
    if (!loadProject(argv[1], project, error)) {
        logError("Can't open the project: " + error);
        return 1;
    }
    std::vector<ExportedScene> scenes;
    if (!exportProject(project, {}, &scenes)) {
        return 1;
    }
    const std::filesystem::path debug = projectExportDir(project) / "debug";
    if (roundTripScenes(project, debug / "scenes") > 0) {
        return 1;
    }
    const std::string marker = kGeneratedMarker;
    if (!write(debug / "debug_scenes.h", marker + kDebugHeader) || !write(debug / "debug_scenes.c", debugScenes(scenes)) ||
        !write(debug / "debug_main.c", marker + kDebugMain)) {
        return 1;
    }
    std::printf("debug files written to %s\n", debug.string().c_str());
    return 0;
}
