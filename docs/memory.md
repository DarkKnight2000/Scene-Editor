# Memory

Every scene's arrays (entities, drawing order, caches and component pools) live in one
shared memory region from [easy_memory](https://github.com/EasyMem/easy_memory) (MIT,
included in `utils/`). The runtime doesn't call `malloc`, `realloc` or `free`.

- **Created on first use:** the shared region is created the first time a scene needs
  memory, with `SCENE_MEMORY_SIZE` bytes (default 8 MiB).
- **Doesn't grow:** when the region is full, adding an entity returns -1 and setting a
  component returns NULL, as running out of memory would.
- **Reused:** freed space is reused, so loading and unloading scenes doesn't use the
  region up.
- **Returned on unload:** `scene_unload` (and `scene_manager_deinit`) give all of a
  scene's memory back.

## Changing the size

Compile with a different size, for example:

```bash
make CFLAGS="-std=c99 -O2 -Wall -Wextra -DSCENE_MEMORY_SIZE=33554432"
```

As a rough guide, each entity costs its `SceneEntity` (about 100 bytes), its pose and UI
cache entries, and an `int` in every pool's index, plus its components. A few thousand
entities fit easily in the default size.

## Freeing the shared region

```c
static void shutdown_scenes(SceneManager *manager) {
    scene_manager_deinit(manager); /* every scene gives its memory back */
    scene_memory_destroy();        /* then the region itself is freed */
}
```

Call `scene_memory_destroy` only once no scene uses the region any more.

## A region of your own for a scene

Set `SceneData.memory` before loading a scene to put it in its own region. NULL means
the shared one. The setting survives `scene_unload` and reloads. Any easy_memory region
works:

```c
static unsigned char level_buffer[512 * 1024];

static void use_own_memory(SceneManager *manager) {
    /* A region carved out of the shared one... */
    manager->scenes[SCENE_ID_DEMO].memory = em_create_nested(scene_memory(), 1024 * 1024);
    /* ...or one over your own static buffer: no heap at all. */
    manager->scenes[SCENE_ID_HUD].memory = em_create_static(level_buffer, sizeof(level_buffer));
}
```

Include `easy_memory.h` to use these functions. Set them before `scene_manager_init`
loads the scenes. A scene in its own region runs out of space independently of the
others. When you're done, call `em_destroy` on a region you created, after unloading the
scenes that use it.

## Building easy_memory

The export contains:

- `utils/easy_memory.h`, copied from the folder set in Project Settings;
- `utils/easy_memory.c`, which compiles its implementation;
- its license, `utils/easy_memory_LICENSE.txt`.

The Makefile compiles `easy_memory.c` without warnings: gcc reports a false
`-Warray-bounds` warning inside it at `-O2`. That way, strict flags like `-Werror` only
judge your code and the scenes.
