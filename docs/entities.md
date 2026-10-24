# Entities

Every entity in a scene's World tree is a `SceneEntity` field of the scene struct:

```c
typedef struct SceneEntity {
    SceneTransform transform;  /* relative to its parent */
    ScenePose pose;            /* in the scene: from the last _update_poses / _draw */
    SceneSprite sprite;        /* unused by entities without a sprite */
    SceneBoxCollider collider; /* unused by entities without a box collider */
    bool visible;              /* false: its sprite isn't drawn (its children's still are) */
#if SCENE_PHYSICS
    b2BodyId body;             /* its Box2D body (rigidbody or collider), else b2_nullBodyId */
#endif
} SceneEntity;
```

The comment after each field in the scene's header says which of these the entity has,
e.g. `/* "Ground": rectangle, collider */`.

## Transforms and poses

**`transform`** is what you set in the editor: position, rotation (degrees) and scale,
relative to the entity's parent.

```c
typedef struct SceneTransform {
    Vector2 position;
    float rotation;
    Vector2 scale;
    bool inherit_rotation; /* false: ignore the parents' rotation (e.g. a health bar) */
    bool inherit_scale;    /* false: ignore the parents' scale */
} SceneTransform;
```

**`pose`** is where the entity ends up in the scene, after its parents and the World
transform:

```c
typedef struct ScenePose {
    Vector2 position;
    float rotation;
    Vector2 scale;
} ScenePose;
```

- **When poses update:** `scene_<name>_draw` recomputes every pose before drawing. After
  changing transforms, call `scene_<name>_update_poses` yourself if you read poses before
  the next draw.
- **The World root:** `s->world` moves, rotates and zooms the whole scene, e.g. for camera
  movement or screen shake.

```c
/* Moving: change the transform; children follow. */
level.platform.transform.position.x += 60.0f * GetFrameTime();

/* Screen shake: move the whole World. */
level.world.position = (Vector2){(float)GetRandomValue(-4, 4), (float)GetRandomValue(-4, 4)};
```

## Drawing your own things

Between `scene_begin(mode)` and `scene_end()`, raylib draws in scene coordinates. So
anything drawn at an entity's pose lands where the editor shows it, at any window size:

```c
scene_begin(mode);
scene_level_draw(&level);

/* An animation frame at the player's place, turned and scaled like the player. */
DrawTexturePro(sheet, frames[frame], (Rectangle){level.player.pose.position.x, level.player.pose.position.y, 48.0f, 48.0f},
               (Vector2){24.0f, 24.0f}, level.player.pose.rotation, WHITE);

scene_end();
```

**Hiding the placeholder:** to draw your own art instead of the sprite, set
`visible = false`, or untick the sprite's **Export** box in the editor so it isn't
exported at all. The entity is still exported either way.

## Sprites

```c
typedef struct SceneSprite {
    Vector2 size; /* before scaling */
    Color tint;
} SceneSprite;
```

- **Shape:** the shape (rectangle, circle or triangle) is fixed at export.
- **Size and position:** the sprite is centred on the entity's position and sized `size`
  times the entity's scale.
- **Your own sprites:** `scene_draw_rectangle`, `scene_draw_circle` and
  `scene_draw_triangle` draw any `SceneSprite` at any `ScenePose`.

## Boxes: collider and sprite areas in the scene

A **box collider** is a box at `offset` from the entity's position, before scaling.
`scene_box()` places a box in the scene: centre, scaled size and rotation.

```c
typedef struct SceneBox {
    Vector2 center;
    Vector2 size;
    float rotation; /* degrees */
} SceneBox;

SceneBox scene_box(ScenePose pose, Vector2 offset, Vector2 size);
void scene_box_corners(SceneBox box, Vector2 corners[4]);
void scene_draw_box_lines(SceneBox box, float thick, Color color);
```

```c
/* The ground's collider, in scene coordinates: e.g. to place generated tiles on it. */
const SceneBox ground = scene_box(level.ground.pose, level.ground.collider.offset, level.ground.collider.size);
const float top = ground.center.y - ground.size.y / 2.0f; /* unrotated */
float x;
for (x = ground.center.x - ground.size.x / 2.0f; x < ground.center.x + ground.size.x / 2.0f; x += 32.0f) {
    DrawTextureV(grass, (Vector2){x, top - 32.0f}, WHITE);
}

/* A sprite's area: the same with no offset. */
const SceneBox door = scene_box(level.door.pose, (Vector2){0.0f, 0.0f}, level.door.sprite.size);
```

- **Rotated boxes:** `scene_box_corners` gives the four corners, rotated.
- **Debug view:** `scene_<name>_draw_colliders(s, color)` outlines every collider (the
  example's C key).
- **Without physics:** collider boxes are exported with physics off too, so you can use
  them as plain level geometry: trigger areas, spawn zones, platforms.
- **With physics:** the collider is also the shape of the entity's Box2D body. See
  [Physics](physics.md).

## Pose maths

The helpers the generated code uses are in `scene_common.h`, so your code can use them
too:

| Helper | What it gives |
|---|---|
| `scene_transform(x, y, rotation, sx, sy)` | a transform that inherits rotation and scale |
| `scene_pose_of(t)` | a transform's values as a pose |
| `scene_pose_identity()` | position 0, rotation 0, scale 1 |
| `scene_child_pose(parent, t)` | where a child with local transform `t` ends up |
| `scene_compose(parent, rot, t)` | the same, with `rot = scene_rot(parent.rotation)` worked out once |

**Example:** where a point attached to an entity is, e.g. a gun's muzzle 20 units in
front of a turret:

```c
const ScenePose muzzle = scene_child_pose(level.turret.pose, scene_transform(20.0f, 0.0f, 0.0f, 1.0f, 1.0f));
```
