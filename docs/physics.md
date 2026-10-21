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

## Running the simulation

The [SceneManager](scenes.md#the-scenemanager) steps the current scene's physics in
`scene_manager_update`. `project_main.c` calls it every frame:

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

Without a SceneManager, call `scene_physics_step(scene, dt)` yourself, once per frame.

**What a step does:**

1. **Fixed steps:** it runs as many fixed steps as the elapsed time needs
   (`SCENE_PHYSICS_TIME_STEP`, 1/60 s, with 4 substeps). So the simulation doesn't
   depend on the frame rate.
2. **Bodies:** it creates the Box2D world and the bodies entities need. The first step
   after loading creates them all.
3. **Teleports:** any entity your code moved since the last step is moved in Box2D too.
4. **Write-back:** it writes dynamic and kinematic bodies back into their entities'
   transforms, parents before children.
5. **Events:** it records trigger and contact events in `scene->contacts`.

Physics happens in the World's own space. So moving the World transform (for example
for screen shake) moves the picture, not the bodies.

## Moving bodies from code

These functions work in scene units and convert to Box2D's metres:

```c
static void player_controls(SceneData *scene) {
    const int player = scene_find_entity(scene, "Player");
    Vector2 velocity = scene_get_velocity(scene, player); /* scene units per second */

    velocity.x = 0.0f;
    if (IsKeyDown(KEY_LEFT)) velocity.x = -300.0f;
    if (IsKeyDown(KEY_RIGHT)) velocity.x = 300.0f;
    if (IsKeyPressed(KEY_SPACE)) velocity.y = -6.0f * SCENE_PIXELS_PER_METER; /* jump */
    scene_set_velocity(scene, player, velocity);
}

static void explode(SceneData *scene, int crate) {
    /* kg * scene units per second: mass matters, unlike with set_velocity */
    scene_apply_impulse(scene, crate, (Vector2){0.0f, -2000.0f});
}

static void teleport(SceneData *scene, int id) {
    /* Move the entity; its body follows on the next step. */
    scene_entity(scene, id)->transform.position = (Vector2){0.0f, -200.0f};
}
```

`scene_apply_force` pushes for one step, so call it every frame for a steady push.

## Triggers and contacts

After each `scene_physics_step`, `scene->contacts[0 .. scene->contact_count - 1]` lists
what happened during that step:

| `kind` | Meaning |
|---|---|
| `SCENE_TRIGGER_ENTER` / `SCENE_TRIGGER_EXIT` | `other` started or stopped overlapping trigger `entity` |
| `SCENE_CONTACT_BEGIN` / `SCENE_CONTACT_END` | `entity` and `other` started or stopped touching |

```c
static int coins;

/* An on_draw hook: runs after the step (scene_manager_update comes before drawing). */
static void collect_coins(SceneData *scene, int *current) {
    int i;
    const int player = scene_find_entity(scene, "Player");
    for (i = 0; i < scene->contact_count; ++i) {
        const SceneContactEvent *c = &scene->contacts[i];
        if (c->kind == SCENE_TRIGGER_ENTER && c->other == player &&
            strncmp(scene->entities[c->entity].name, "Coin", 4) == 0) {
            ++coins;
            scene_remove_entity(scene, c->entity); /* its body goes with it */
        }
    }
    (void)current;
}
```

## Adding and changing bodies at runtime

- **Adding:** give an entity a Box Collider (`scene_set_collider`) and, unless it should
  be static, a Rigidbody (`scene_set_rigidbody`). Its body appears on the next step.
- **Changing components:** `scene_set_collider`, `scene_set_collider_material` and
  `scene_set_rigidbody` rebuild the body on the next step.
- **Changing fields directly:** if you change a rigidbody's or collider's fields through
  their pointers instead, call `scene_physics_rebuild(scene, id)`.
- **Removing:** removing the entity, its collider or its rigidbody destroys the body.
- **Scale:** the collider's size uses the entity's scale when the body is built. Rebuild
  after scaling a body.

```c
static int drop_crate(SceneData *scene, Vector2 at) {
    const int crate = scene_add_entity(scene, "Crate", SCENE_WORLD);
    scene_set_sprite(scene, crate, (Vector2){48.0f, 48.0f}, BROWN);
    scene_set_collider(scene, crate, (Vector2){0.0f, 0.0f}, (Vector2){48.0f, 48.0f}, false);
    scene_set_collider_material(scene, crate, 1.0f, 0.6f, 0.3f);
    scene_set_rigidbody(scene, crate, SCENE_BODY_DYNAMIC);
    scene_place_entity(scene, crate, at);
    return crate;
}
```

## Using Box2D directly

`scene_physics_body(scene, id)` returns the entity's `b2BodyId`, and `scene->physics_world`
is the `b2WorldId`. With those you can use the whole Box2D API: joints, ray casts, more
shapes. Box2D itself works in **metres**, so divide scene units by
`SCENE_PIXELS_PER_METER`:

```c
static void spin(SceneData *scene, int id) {
    const b2BodyId body = scene_physics_body(scene, id);
    if (B2_IS_NON_NULL(body)) {
        b2Body_SetAngularVelocity(body, 3.0f); /* radians per second */
    }
}

static bool ground_below(SceneData *scene, int id) {
    const ScenePose pose = scene_entity_pose(scene, id);
    const b2Vec2 origin = {pose.position.x / SCENE_PIXELS_PER_METER, pose.position.y / SCENE_PIXELS_PER_METER};
    const b2Vec2 down = {0.0f, 1.0f}; /* one metre down (y points down) */
    const b2RayResult hit = b2World_CastRayClosest(scene->physics_world, origin, down, b2DefaultQueryFilter());
    return hit.hit;
}
```

Bodies only exist after the first `scene_physics_step`. Before that,
`scene_physics_body` returns `b2_nullBodyId`.

## Building and memory

- **The `box2d/` folder:** the export contains Box2D's `include/` and `src/`, copied from
  the Box2D path in Project Settings.
- **How it's built:** the Makefile compiles Box2D once into `bin/box2d/`, with C17 and its
  warnings off.
- **C11 for your code:** Box2D's headers need C11, which is why the exported code builds
  with `-std=c11`.
- **Memory:** Box2D allocates from the same easy_memory region as the scenes, and that
  region defaults to 32 MiB when physics is on. See [Memory](memory.md).
- **Tests:** `make test` includes physics checks. A box falls through a trigger and comes
  to rest on the ground, a moved entity's body follows, and removed entities lose their
  bodies.
