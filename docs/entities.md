# Entities

Every entity in a scene's World tree is a `SceneEntity` field of the scene struct:

```c
typedef struct SceneEntity {
    SceneTransform transform;
    ScenePose pose;            /* from the last _update_poses or _draw */
    SceneSprite sprite;        /* if it has one */
    SceneBoxCollider collider; /* if it has one */
    bool visible;              /* false: its sprite isn't drawn */
#if SCENE_PHYSICS
    b2BodyId body;              /* if it has a rigidbody or a collider */
    ScenePlatformer platformer; /* if it has a platformer controller (see Physics) */
#endif
} SceneEntity;
```

## Transforms and poses

**`transform`** is what you set in the editor, relative to the entity's parent:

```c
typedef struct SceneTransform {
    Vector2 position;
    float rotation;              /* degrees */
    Vector2 scale;
    bool ignore_parent_rotation; /* e.g. a health bar that shouldn't spin with its character */
    bool ignore_parent_scale;
} SceneTransform;
```

**`pose`** is where the entity ends up in the scene, after its parents and the World
transform: `position`, `rotation` and `scale`.

- **When poses update:** `scene_<name>_draw` recomputes every pose before drawing. After
  changing transforms, call `scene_<name>_update_poses` if you read poses before the next
  draw.
- **The World root:** the scene's `world` transform moves, rotates and zooms the whole
  scene, e.g. for camera movement or screen shake.

```c
level.platform.transform.position.x += 60.0f * GetFrameTime(); /* children follow */
level.world.position = (Vector2){(float)GetRandomValue(-4, 4), 0.0f}; /* screen shake */
```

## Drawing your own things

Between `scene_begin()` and `scene_end()`, raylib draws in scene coordinates, so
anything drawn at an entity's pose lands where the editor shows it, at any window size:

```c
scene_begin();
scene_level_draw(&level);
DrawTexturePro(sheet, frames[frame], (Rectangle){level.player.pose.position.x, level.player.pose.position.y, 48.0f, 48.0f},
               (Vector2){24.0f, 24.0f}, level.player.pose.rotation, WHITE);
scene_end();
```

**Hiding the placeholder:** set `visible = false` to draw your own art instead of the
sprite, or untick the sprite's **Export** box in the editor so it isn't exported at all.

**A point attached to an entity**, e.g. a muzzle 20 units in front of a turret:

```c
const ScenePose muzzle = scene_child_pose(level.turret.pose, (SceneTransform){.position = {20.0f, 0.0f}, .scale = {1.0f, 1.0f}});
```

## Sprites

`sprite` has a `size` (before scaling) and a `tint`; its shape (rectangle, circle or
triangle) is fixed at export. It's centred on the entity's position and scaled with it.
`scene_draw_rectangle`, `scene_draw_circle` and `scene_draw_triangle` draw any sprite at any
pose.

## Collider boxes

A box collider is a box at `offset` from the entity's position, before scaling.
`scene_box()` places it in the scene:

```c
typedef struct SceneBox {
    Vector2 center;
    Vector2 size;   /* scaled */
    float rotation; /* degrees */
} SceneBox;

SceneBox scene_box(ScenePose pose, Vector2 offset, Vector2 size);
```

```c
/* Grass tiles along the top of the ground's collider. */
const SceneBox ground = scene_box(level.ground.pose, level.ground.collider.offset, level.ground.collider.size);
float x;
for (x = ground.center.x - ground.size.x / 2.0f; x < ground.center.x + ground.size.x / 2.0f; x += 32.0f) {
    DrawTextureV(grass, (Vector2){x, ground.center.y - ground.size.y / 2.0f - 32.0f}, WHITE);
}

/* Any box, rotated, with raylib: */
DrawRectanglePro((Rectangle){ground.center.x, ground.center.y, ground.size.x, ground.size.y},
                 (Vector2){ground.size.x / 2.0f, ground.size.y / 2.0f}, ground.rotation, BROWN);
```

- **Without physics:** colliders are exported with physics off too, so they work as plain
  level geometry: platforms, trigger areas, spawn zones.
- **With physics:** the collider is also the shape of the entity's Box2D body. See
  [Physics](physics.md).
- **A sprite's area:** `scene_box(e.pose, (Vector2){0, 0}, e.sprite.size)`.
