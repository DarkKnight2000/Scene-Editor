# Getting started

## Exporting

In the editor, use **Export** (Ctrl+E) or **Play** (F5, which exports, builds and runs).
Both export every scene. Play also makes the game start in the scene you're editing (the
active tab); Export starts it in the first scene. See `SCENE_ID_START` in
[Scenes](scenes.md#the-scenemanager).
The project is written to `<project>/export/`:

```text
export/
  Makefile
  src/           project_main.c (an example main) and your own .c files
  generated/     one .h/.c per scene (its struct and functions), scene_manager.h/.c
  utils/         scene_common.h/.c (scaling, the shared types, drawing helpers)
  box2d/         Box2D, when physics is on (see Physics)
  .vscode/       c_cpp_properties.json (IntelliSense)
  .gitignore     ignores bin/
  bin/           build output (made by the Makefile)
```

- **Rewritten on every export:** everything in `generated/` and `utils/`.
- **Your choice:** the `Makefile`, `src/project_main.c`, the `.vscode` settings and the
  `.gitignore` follow the [export options](#export-options).
- **Kept:** files you add, in `src/` or anywhere else.
- **Designer-only sprites:** a sprite with **Export** unticked (in its Sprite section) is
  shown in the editor but left out of the C code, for guides and placeholders. Its entity
  and other components are still exported.
- **Include paths:** `src/`, `generated/` and `utils/` are all include directories, so
  any header can be included by its name alone: `#include "scene_manager.h"` gives you
  everything.

## Command line

| Command | What it does |
|---|---|
| `SceneEditor` | opens the editor: the project open last time, or the project launcher |
| `SceneEditor --open <project>` | opens the editor on a project: its `.sceneproj` file or its folder |
| `SceneEditor --export <project>` | exports a project without opening the editor, like **Export** |
| `SceneEditor --version` | prints the version |
| `SceneEditor --help` | prints the options |

`--export` prints the same messages as the Logs panel, with warnings and errors on
stderr. It exits with 0 when the export can be built and 1 when it can't. Bad options exit
with 2. It saves nothing in the editor's own settings, and a scene left unsaved in an open
editor isn't included, because only saved scene files are read.

**Exporting from a script:**

```bash
SceneEditor --export path/to/MyGame && make -C path/to/MyGame/export
```

On Windows the editor is a windowed program, so `cmd` and PowerShell don't wait for it:
the prompt can come back before its output. In scripts there:

- **cmd:** use `start /wait "" SceneEditor --export MyGame`.
- **PowerShell:** pipe the output, e.g. `SceneEditor --export MyGame | Write-Output`.

Git Bash and MSYS shells wait as usual.

## Export options

**Project Settings** has an Export section.

**Exported modes:** which of the [scale modes](scaling.md) (Expand, Fit, Fill) the
runtime supports.

- Only their code is exported, and `SceneScaleMode` lists only them.
- The project's default scale mode is always included.
- With Fit left out, its clipping and black-bar code isn't in the export at all.

**When to write the files you may edit by hand** (the Makefile, `project_main.c`, the
`.vscode` settings and the `.gitignore`), each set separately:

| Setting | Effect |
|---|---|
| Always | rewritten on every export (the default) |
| Only if missing | written when it doesn't exist, so your edited version is kept. Delete it to get a fresh copy. |
| Never | not written; an existing one is left alone |

Each export lists the files it didn't write in the Logs panel. Play needs a Makefile,
so with Makefile set to Never, keep your own.

## Building with the Makefile

The Makefile needs GNU make (on Windows, for example, `mingw32-make`) and a C11 compiler.
The code is C11 because Box2D's headers need it; with physics off, it also builds as C99.

```bash
make RAYLIB_SRC=path/to/raylib/src
```

| Target | What it does |
|---|---|
| `make` | builds `bin/<project>` |
| `make run` | builds and runs it |
| `make clean` | deletes `bin/` |

**Telling it where raylib is** (pick one):

| Variable | Meaning |
|---|---|
| `RAYLIB_SRC=…/raylib/src` | compiles raylib from source with your compiler (once, into `bin/raylib/`). Always links. |
| `RAYLIB_INCLUDE=…` + `RAYLIB_LIB_DIR=…` | a prebuilt raylib: the folder with `raylib.h`, and the folder with `libraylib.a` or `raylib.lib` |
| `RAYLIB_LIBS=…/libraylib.a` | the exact library to link (overrides `RAYLIB_LIB_DIR`) |
| *(none)* | an installed raylib (`-lraylib`) |

A prebuilt raylib only links with the compiler family that built it. MinGW gcc can't
link a library built for MSVC, and the other way round: errors like
`undefined reference to __chkstk` mean such a mismatch. If in doubt, use `RAYLIB_SRC`.

**Other variables:**

- **`CC`:** the compiler, e.g. `CC=clang`.
- **`CFLAGS`:** compiler flags (default `-std=c11 -O2 -Wall -Wextra`).
- **`EXCLUDE`:** source files to leave out, e.g. `EXCLUDE=src/project_main.c` when your
  game has its own `main()`.

Builds are incremental: object and dependency files go to `bin/obj/`, and only what changed
is recompiled. After changing `CFLAGS` or the compiler, run `make clean`.

Make also reads these variables from the environment. Setting `RAYLIB_SRC` (or
`RAYLIB_INCLUDE`) once in your environment works for both `make` and VS Code.

## Your own main()

`src/project_main.c` is a minimal program: a resizable window, a SceneManager, Tab / Shift+Tab
to switch scenes, and comments where your own drawing goes. The [README](README.md#a-whole-program)
shows it. Either edit a copy of it (in your own file in `src/`, built with
`EXCLUDE=src/project_main.c`, because the export rewrites `project_main.c`) or write your
own.

Every call that touches raylib (initialising scenes, drawing) must come after
`InitWindow()`.

## VS Code

The export includes `.vscode/c_cpp_properties.json` for the C/C++ extension, with the
include folders. `raylib.h` is found through the `RAYLIB_INCLUDE` or `RAYLIB_SRC`
environment variable, so set one of them before starting VS Code. To customise the file,
set **.vscode settings** to *Only if missing* in the [export options](#export-options).
