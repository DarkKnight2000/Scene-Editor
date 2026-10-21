# Getting started

## Exporting

In the editor, use **Export** (Ctrl+E) or **Play** (F5, which exports, builds and runs).
The project is written to `<project>/export/`:

```text
export/
  Makefile
  src/           project_main.c (an example main) and your own .c files
  generated/     one .h/.c per scene, scenes.h, scene_manager.h/.c
  utils/         scene_common.h/.c (the runtime), easy_memory.h/.c (memory regions)
  box2d/         Box2D, when physics is on (see Physics)
  .vscode/       c_cpp_properties.json (IntelliSense)
  .gitignore     ignores bin/
  bin/           build output (made by the Makefile)
```

- **Rewritten on every export:** everything in `generated/` and `utils/`.
- **Your choice:** the `Makefile`, `src/project_main.c`, the `.vscode` settings and the
  `.gitignore` follow the [export options](#export-options).
- **Kept:** files you add, in `src/` or anywhere else.
- **Include paths:** `src/`, `generated/` and `utils/` are all include directories, so
  any header can be included by its name alone: `#include "scenes.h"` gives you
  everything.

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
make release RAYLIB_SRC=path/to/raylib/src
```

| Target | What it does |
|---|---|
| `make` / `make release` | builds `bin/<project>` without test code |
| `make run` | builds the release and runs it fullscreen (`ARGS=--windowed` for a window) |
| `make test` | strict build with `-DSCENE_TESTS -Werror`, then a smoke test of every scene |
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

Builds are incremental: object and dependency files go to `bin/release/` and `bin/test/`,
and only what changed is recompiled. After changing `CFLAGS` or the compiler, run
`make clean`.

Make also reads these variables from the environment. Setting `RAYLIB_SRC` (or
`RAYLIB_INCLUDE`) once in your environment works for both `make` and VS Code.

## Your own main()

`src/project_main.c` is a complete example: borderless fullscreen, a SceneManager, scene
switching, clicking to add entities, and UI text. Read it, then either:

- **Start from it:** copy it to your own file in `src/` and build with
  `EXCLUDE=src/project_main.c`, because the export rewrites `project_main.c`.
- **Use your own main():** the [README](README.md#the-shortest-working-program) has the
  smallest one, and [Scenes](scenes.md) shows one with a SceneManager.

Every call that touches raylib (loading scenes, drawing) must come after `InitWindow()`.

## The example's controls

| Key | Action |
|---|---|
| Left / Right | previous / next scene (done by the scenes' own logic hook) |
| M | cycle the scale mode |
| Click | add an entity at the mouse (clicks on the UI don't) |
| Delete | remove the last added entity |
| C | show or hide box colliders |
| Space | with physics: make an entity called "Player" jump |
| R | reload the scene from its exported data |
| F11 / Alt+Enter | toggle fullscreen |
| Esc | quit |

## VS Code

The export includes `.vscode/c_cpp_properties.json` for the C/C++ extension. It sets
the include folders and the defines for two configurations:

- **Release**
- **Test**, which defines `SCENE_TESTS` so the test code isn't greyed out.

`raylib.h` is found through the `RAYLIB_INCLUDE` or `RAYLIB_SRC` environment variable,
so set one of them before starting VS Code. To customise the file, set **.vscode
settings** to *Only if missing* in the [export options](#export-options).

## Tests

`make test` builds with `-DSCENE_TESTS` and runs `bin/<project>_test --frames N`. That
exercises the runtime API on every scene and draws every scene in every scale mode, then
exits with a non-zero code on failure. Release builds contain none of this code.
