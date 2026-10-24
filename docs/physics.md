# Physics (Box2D)

Exported games can simulate physics with [Box2D](https://box2d.org/) v3.1 (MIT, included in
`box2d/`).

**Turning it on:** physics is on when **Project Settings > Physics (Box2D)** is ticked
(the default). The runtime then has `SCENE_PHYSICS` set to 1. With it off, nothing from
Box2D is exported or compiled, and rigidbodies are left out of the export.

## Setting up bodies in the editor

| You add | The entity becomes |
|---|---|
| Box Collider only | a **static** body (ground, walls) |
| Rigidbody + Box Collider | a body of the Rigidbody's type, shaped like the collider |
| Rigidbody only | a body without a shape (it falls, but touches nothing) |
| Box Collider with *Is Trigger* | a **sensor**: reports overlaps, doesn't collide |

**Rigidbody:**

- **Body type:**
  - **Static** never moves.
  - **Kinematic** moves only by its velocity.
  - **Dynamic** is moved by gravity, forces and collisions.
- **Gravity:** a multiplier, so 0 means it floats.
- **Damping:** linear (L) and angular (A).
- **Fixed rotation:** the body never turns, as for a platformer character.
- **Bullet:** continuous collision, for fast objects.

**Box Collider:** besides its size and offset, it has a **density** in kg per square
metre (default 1), a **friction** (default 0.6) and a **restitution** (bounciness from
0 to 1, default 0).

**Project Settings:**

- **Pixels per metre:** how many scene units make a metre, since Box2D works in metres
  (default 64).
- **Gravity:** in m/s², with y pointing down (default 0, 9.8).
- **Box2D path:** the folder exports copy Box2D from.

Box2D behaves best with objects between about 0.1 and 10 metres. At 64 pixels per metre,
that's roughly 6 to 640 pixels.

## Bodies in the exported scene

`scene_<name>_init` creates the scene's Box2D world (`s->physics_world`) and a body for
every entity with a rigidbody or a box collider, at its place in the scene. Each body is
in that entity's `body` field:

```c
SceneEntity *player = &level.player;
b2BodyId body = player->body; /* b2_nullBodyId for entities without one */
```

- **User data:** bodies and shapes carry their `SceneEntity *` as user data, so Box2D
  events and queries lead back to the entity.
- **Colliders:** the collider's values are in `entity.collider` too, so you can read the
  box in scene coordinates with `scene_box()` (see [Entities](entities.md#boxes-collider-and-sprite-areas-in-the-scene)).
- **Freeing:** `scene_<name>_deinit` destroys the world and every body in it.

## Running the simulation

Scenes with bodies have `scene_<name>_step(s, dt)`. The
[SceneManager](scenes.md#the-scenemanager) calls it for the current scene in
`scene_manager_update`, which `project_main.c` calls every frame:

```c
static void frame(SceneManager *manager, SceneScaleMode mode) {
    scene_manager_update(manager, GetFrameTime()); /* physics of the current scene */
    BeginDrawing();
    ClearBackground(BLACK);
    scene_begin(mode);
    scene_manager_draw(manager);
    scene_end();
    scene_manager_draw_ui(manager, mode);
    EndDrawing();
}
```

**What a step does:**

1. **Fixed steps:** it runs as many fixed steps as the elapsed time needs
   (`SCENE_PHYSICS_TIME_STEP`, 1/60 s, with 4 substeps), so the simulation doesn't depend
   on the frame rate.
2. **Write-back:** it writes dynamic and kinematic bodies back into their entities'
   transforms, parents before children. The next `update_poses` / `draw` turns them into
   poses.

Physics happens in the World's own space. So moving the World transform (for example
for screen shake) moves the picture, not the bodies.

## Moving bodies from code

Move bodies through Box2D: changing an entity's transform doesn't move its body (the
next step writes the body's place back). These helpers work in scene units and convert
to Box2D's metres:

```c
static void player_controls(SceneEntity *player) {
    Vector2 velocity = scene_get_velocity(player); /* scene units per second */

    velocity.x = 0.0f;
    if (IsKeyDown(KEY_LEFT)) velocity.x = -300.0f;
    if (IsKeyDown(KEY_RIGHT)) velocity.x = 300.0f;
    if (IsKeyPressed(KEY_SPACE)) velocity.y = -6.0f * SCENE_PIXELS_PER_METER; /* jump */
    scene_set_velocity(player, velocity);
}

static void explode(SceneEntity *crate) {
    /* kg * scene units per second: mass matters, unlike with set_velocity */
    scene_apply_impulse(crate, (Vector2){0.0f, -2000.0f});
}

static void teleport(SceneEntity *e) {
    b2Body_SetTransform(e->body, scene_to_meters((Vector2){0.0f, -200.0f}), b2Body_GetRotation(e->body));
}
```

`scene_apply_force` pushes for one step, so call it every frame for a steady push.

## Triggers and contacts

Use Box2D's events after a step. Shapes carry their `SceneEntity *`, so compare with your
scene's fields:

```c
static int coins;

static void collect_coins(LevelScene *level) {
    const b2SensorEvents events = b2World_GetSensorEvents(level->physics_world);
    int i;
    for (i = 0; i < events.beginCount; ++i) {
        SceneEntity *trigger = (SceneEntity *)b2Shape_GetUserData(events.beginEvents[i].sensorShapeId);
        SceneEntity *visitor = (SceneEntity *)b2Shape_GetUserData(events.beginEvents[i].visitorShapeId);
        if (trigger == &level->coin && visitor == &level->player && level->coin.visible) {
            ++coins;
            level->coin.visible = false;
            b2Body_Disable(level->coin.body); /* gone from the simulation */
        }
    }
}
```

Contacts between solid colliders work the same with `b2World_GetContactEvents`.

**Several steps per frame:** Box2D keeps the events of the last fixed step only. When a
frame runs more than one step (a slow frame), earlier steps' events are gone by the time
you read them.

## Using Box2D directly

`entity.body` and `s->physics_world` give you the whole Box2D API: joints, ray casts,
more shapes. Box2D itself works in **metres**: `scene_to_meters` and
`scene_to_scene_units` convert.

```c
static void spin(SceneEntity *e) {
    b2Body_SetAngularVelocity(e->body, 3.0f); /* radians per second */
}

static bool ground_below(LevelScene *level) {
    /* Bodies live in the World's own space: with a moved World, undo its transform first. */
    const b2Vec2 origin = scene_to_meters(level->player.pose.position);
    const b2Vec2 down = {0.0f, 1.0f}; /* one metre down (y points down) */
    const b2RayResult hit = b2World_CastRayClosest(level->physics_world, origin, down, b2DefaultQueryFilter());
    return hit.hit;
}
```

## Building

- **The `box2d/` folder:** the export contains Box2D's `include/` and `src/`, copied from
  the Box2D path in Project Settings.
- **How it's built:** the Makefile compiles Box2D once into `bin/box2d/`, with C17 and its
  warnings off.
- **C11 for your code:** Box2D's headers need C11, which is why the exported code builds
  with `-std=c11`.
