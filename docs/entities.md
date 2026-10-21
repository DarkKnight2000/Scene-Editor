# Entities and transforms

## Ids

Every entity has an `int` id that indexes the scene's arrays.

- **Lifetime:** an id stays valid until that entity is removed or the scene is unloaded.
- **Never reused:** ids of removed entities are never given out again, so a stale id
  never points at a different entity.
- **Failure:** functions return `-1` for "none" or "failed" (out of memory, for example).

Each entity has a name, a parent and a transform. Everything else is a
[component](components.md).

```c
typedef struct SceneEntity {
    char name[SCENE_NAME_MAX];  /* truncated to 63 bytes */
    int parent;                 /* entity id, or SCENE_WORLD */
    SceneTransform transform;   /* relative to the parent */
    bool alive;                 /* false once removed */
} SceneEntity;
```

## Finding, adding and removing

```c
static void entity_basics(SceneData *scene) {
    /* The first live entity with this name, in drawing order; -1 if none. */
    const int player = scene_find_entity(scene, "Player");

    /* A new entity at its parent's position, drawn on top of the others. */
    const int coin = scene_add_entity(scene, "Coin", SCENE_WORLD);
    const int sparkle = scene_add_entity(scene, "Sparkle", coin); /* a child of the coin */

    /* Read or change the name, parent or transform. NULL if the id isn't a live entity. */
    SceneEntity *e = scene_entity(scene, coin);
    if (e != NULL) {
        e->transform.position = (Vector2){120.0f, -40.0f};
        e->transform.rotation = 45.0f;
    }

    /* Removing an entity removes its components. Its children move to its parent without
       moving on screen. Returns false if the id isn't a live entity. */
    scene_remove_entity(scene, sparkle);
    (void)player;
}
```

`scene_entity` returns a pointer into the entity array. Adding an entity can move that
array, so look the pointer up again after `scene_add_entity` instead of keeping it.

To visit every live entity in drawing order:

```c
static void list_entities(SceneData *scene) {
    int i;
    for (i = 0; i < scene->order_count; ++i) {
        const int id = scene->order[i];
        TraceLog(LOG_INFO, "%d: %s", id, scene->entities[id].name);
    }
}
```

## Transforms

```c
typedef struct SceneTransform {
    Vector2 position;       /* relative to the parent, in the parent's scaled, rotated space */
    float rotation;         /* degrees */
    Vector2 scale;
    bool inherit_rotation;  /* false: ignore the parents' rotation (e.g. a health bar) */
    bool inherit_scale;     /* false: ignore the parents' scale */
} SceneTransform;
```

`scene_transform(x, y, rotation, scale_x, scale_y)` makes one that inherits rotation and
scale.

**The World.** `scene->world` is the root above every top-level entity. Changing it
moves, rotates or zooms the whole scene, which makes it handy for screen shake:

```c
static void shake(SceneData *scene, float strength) {
    scene->world.position.x = (float)GetRandomValue(-100, 100) / 100.0f * strength;
    scene->world.position.y = (float)GetRandomValue(-100, 100) / 100.0f * strength;
}
```

## Parents

`scene_set_parent(scene, id, parent, keep_placement)` makes `parent` (an entity id or
`SCENE_WORLD`) the entity's parent.

- **`keep_placement` true:** the entity stays where it is on screen, and its transform
  is recalculated for the new parent.
- **`keep_placement` false:** the transform keeps its values, which are now relative to
  the new parent.

It returns `false` for invalid ids, for a parent in the other tree (a UI element; see
[UI](ui.md)), or for a loop, where the new parent is the entity itself or one of its
children.

```c
static void pick_up(SceneData *scene, int hand, int sword) {
    scene_set_parent(scene, sword, hand, true); /* the sword now follows the hand */
}
```

## Final placement

An entity's transform is relative to its parent. Where it actually ends up, after all its
parents and the World, is a `ScenePose`:

```c
typedef struct ScenePose {
    Vector2 position; /* scene coordinates */
    float rotation;
    Vector2 scale;
} ScenePose;
```

| Function | |
|---|---|
| `scene_entity_pose(scene, id)` | computed from the parent chain; always up to date. `SCENE_WORLD` gives the World's own pose. |
| `scene_update_poses(scene)` then `scene->poses[id]` | every entity's pose, each computed once, parents first. Cheaper when you need many. |
| `scene_place_entity(scene, id, point)` | moves the entity so its final position is `point` |

`scene->poses[id]` is valid after `scene_update_poses` until a transform, parent or the
World changes. `scene_draw` and `scene_draw_colliders` update the poses themselves.

```c
static void follow_mouse(SceneData *scene, int id, SceneScaleMode mode) {
    /* Window pixels to scene coordinates, with the same camera scene_begin uses. */
    const Vector2 point = GetScreenToWorld2D(GetMousePosition(), scene_camera(mode));
    scene_place_entity(scene, id, point);
}

static float distance_between(SceneData *scene, int a, int b) {
    const ScenePose pa = scene_entity_pose(scene, a);
    const ScenePose pb = scene_entity_pose(scene, b);
    const float dx = pa.position.x - pb.position.x;
    const float dy = pa.position.y - pb.position.y;
    return sqrtf(dx * dx + dy * dy);
}
```
