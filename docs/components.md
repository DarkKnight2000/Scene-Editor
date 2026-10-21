# Components

Components are optional data attached to an entity. Scene entities can have:

| Component | Type | Fields |
|---|---|---|
| Sprite | `SceneSprite` | `size` (before scaling), `tint`, `shape`. A filled shape centred on the entity. |
| Box collider | `SceneBoxCollider` | `offset` (from the entity's position, before scaling), `size`, `is_trigger` |

UI elements have their own components; see [UI](ui.md).

## Set, get, remove

Each component type has the same three functions:

```c
static void components_basics(SceneData *scene, int id) {
    /* set: adds the component, or replaces its values; returns it (NULL if `id` isn't a
       live entity, or out of memory) */
    SceneSprite *sprite = scene_set_sprite(scene, id, (Vector2){32.0f, 32.0f}, GOLD);
    scene_set_collider(scene, id, (Vector2){0.0f, 0.0f}, (Vector2){32.0f, 32.0f}, true);

    /* get: the component, or NULL if the entity doesn't have one */
    SceneBoxCollider *collider = scene_get_collider(scene, id);
    if (collider != NULL && collider->is_trigger) {
        collider->size.x *= 2.0f;
    }

    /* remove: drops it (nothing happens if there's none) */
    scene_remove_sprite(scene, id);
    (void)sprite;
}
```

## Sprite shapes

A sprite draws a shape that fills its size:

| `shape` | Draws |
|---|---|
| `SCENE_SHAPE_RECTANGLE` | a rectangle (a new sprite's shape) |
| `SCENE_SHAPE_CIRCLE` | a circle, or an ellipse when the size isn't square |
| `SCENE_SHAPE_TRIANGLE` | a triangle pointing up; down when flipped by a negative y scale |

All three rotate and scale with the entity. `scene_set_sprite` keeps the shape of an
existing sprite, so set the shape after it:

```c
static int add_coin(SceneData *scene, Vector2 at) {
    const int coin = scene_add_entity(scene, "Coin", SCENE_WORLD);
    scene_set_sprite(scene, coin, (Vector2){24.0f, 24.0f}, GOLD);
    scene_set_sprite_shape(scene, coin, SCENE_SHAPE_CIRCLE);
    scene_place_entity(scene, coin, at);
    return coin;
}
```

`scene_set_sprite_shape` does nothing if the entity has no sprite. You can also change
`scene_get_sprite(scene, id)->shape` directly.

## Pointer lifetime

A component pointer stays valid until a component of the same type is added or removed,
in any entity. Removing an entity removes all its components.

## Looping over a component type

Components are stored in pools: one packed array per type. Looping over a pool visits
exactly the entities that have that component, with no gaps. Use this for systems such
as physics or animation:

```c
/* Every trigger the point is inside of, e.g. pickups under the player. */
static int count_triggers_at(SceneData *scene, Vector2 point) {
    int i;
    int hits = 0;
    scene_update_poses(scene); /* scene->poses[id] for every entity */
    for (i = 0; i < scene->colliders.count; ++i) {
        const SceneBoxCollider *c = &scene->colliders.data[i];
        const int id = scene->colliders.entity[i]; /* the entity owning data[i] */
        const ScenePose pose = scene->poses[id];
        const Rectangle box = {
            pose.position.x + c->offset.x * pose.scale.x - c->size.x * fabsf(pose.scale.x) / 2.0f,
            pose.position.y + c->offset.y * pose.scale.y - c->size.y * fabsf(pose.scale.y) / 2.0f,
            c->size.x * fabsf(pose.scale.x),
            c->size.y * fabsf(pose.scale.y),
        };
        if (c->is_trigger && CheckCollisionPointRec(point, box)) {
            ++hits;
        }
    }
    return hits;
}
```

(That rectangle ignores rotation. `scene_draw_colliders` shows the rotated outlines.)

Each pool has:

- **`data[0..count-1]`:** the components, packed.
- **`entity[i]`:** the id of the entity that owns `data[i]`.
- **`sparse[id]`:** the index in `data` of entity `id`'s component, or -1.

Removing a component moves the pool's last component into the freed slot. So the packed
order changes over time: don't rely on it for drawing order, and don't add or remove
components of a type while looping over that type's pool.

## Drawing

| Function | |
|---|---|
| `scene_draw(scene)` | every sprite, in the scene's drawing order (`scene->order`) |
| `scene_draw_colliders(scene, color)` | debug outlines of every box collider, rotated with their entities |

Call both between `scene_begin` and `scene_end`. A scene's generated `scene_<name>_draw`
calls `scene_draw`, then the scene's `on_draw` hook.
