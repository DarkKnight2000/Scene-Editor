# Physics (Box2D)

Exported games can simulate physics with [Box2D](https://box2d.org/) v3.1 (MIT, included in
`box2d/`).

**Turning it on:** physics is on when **Project Settings > Physics (Box2D)** is ticked
(the default). The runtime then has `SCENE_PHYSICS` set to 1. With it off, nothing from
Box2D is exported or compiled, and rigidbodies are left out of the export.

## Setting up bodies in the editor

| You add | The entity becomes |
|---|---|
| Collider only | a **static** body (ground, walls) |
| Rigidbody + Collider | a body of the Rigidbody's type, shaped like the collider |
| Rigidbody only | a body without a shape (it falls, but touches nothing) |
| Collider with *Is Trigger* | a **sensor**: reports overlaps, doesn't collide |

**Rigidbody:**

- **Body type:**
  - **Static** never moves.
  - **Kinematic** moves only by its velocity.
  - **Dynamic** is moved by gravity, forces and collisions.
- **Gravity:** a multiplier, so 0 means it floats.
- **Damping:** linear (L) and angular (A).
- **Fixed rotation:** the body never turns, as for a platformer character.
- **Bullet:** continuous collision, for fast objects.

**Collider:** a **Box**, **Circle** or **Triangle** shape (see
[Entities](entities.md#colliders)). Besides its size (a radius for a circle) and offset, it
has a **density** in kg per square
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

`scene_<name>_init` creates the scene's Box2D world (`physics_world`) and a body for every
entity with a rigidbody or a collider, at its place in the scene. Each body is in that
entity's `body` field. Bodies and shapes carry their entity's `SceneEntity *` (`&e.entity`)
as user data, so Box2D events and queries lead back to the entity.

**Each frame**, `scene_<name>_step(s, dt)` (or `scene_manager_update`) runs fixed 1/60 s
steps for the time that passed, then writes dynamic and kinematic bodies back into their
entities' transforms, parents first.

**Freeing:** `scene_<name>_deinit` (or `scene_manager_deinit`) destroys the world and every
body in it.

Physics happens in the World's own space, so moving the World transform (for example for
screen shake) moves the picture, not the bodies.

## Moving bodies

Use the Box2D API on `entity.body`. Box2D works in metres: `scene_to_meters` and
`scene_to_scene_units` convert from and to scene units.

```c
static void player_controls(b2BodyId body) {
    b2Vec2 velocity = b2Body_GetLinearVelocity(body); /* metres per second */
    velocity.x = 0.0f;
    if (IsKeyDown(KEY_LEFT)) velocity.x = -5.0f;
    if (IsKeyDown(KEY_RIGHT)) velocity.x = 5.0f;
    if (IsKeyPressed(KEY_SPACE)) velocity.y = -6.0f; /* jump: y points down */
    b2Body_SetLinearVelocity(body, velocity);
}

static void teleport(b2BodyId body, Vector2 to) {
    b2Body_SetTransform(body, scene_to_meters(to), b2Body_GetRotation(body));
}

player_controls(level.player.body);
```

Changing an entity's transform doesn't move its body: the next step writes the body's place
back into the transform.

## Platformer controller

Add a **Platformer Controller** to an entity with a dynamic Rigidbody (with **Fixed Rot.**
on) and a Collider, and the exported game moves it like a platformer character: left /
right with **A / D** or the arrow keys, jump with **Space**, **W** or **Up**.

| Setting | Default | What it does |
|---|---|---|
| Move Speed | 300 /s | top running speed, in scene units |
| Acceleration | 2400 /s² | how quickly it gets to that speed (and stops) on the ground |
| Air Control | 0.6 | the share of that acceleration it has in the air |
| Jump Height | 120 | how high a full jump goes, in scene units |
| Coyote Time | 0.1 s | a jump still works this long after running off a ledge |
| Jump Buffer | 0.12 s | a jump pressed this long before landing happens on landing |
| Jump Cut | 0.5 | upward speed kept when jump is released early: tap for a small hop |
| Max Fall | 900 /s | fastest falling speed |

- **Jump height:** the jump speed is worked out from the gravity (Project Settings and the
  Rigidbody's Gravity), so a full jump reaches Jump Height whatever the gravity is.
- **Walls:** set the character's collider friction to 0, or it sticks to walls it runs into.
- **In the exported code:** the settings and the state are in `entity.platformer`, a
  `ScenePlatformer`. The scene's `_step` calls
  `scene_platformer_update(&entity.platformer, entity.body, dt)` before stepping the physics.

**Reading its state**, e.g. to pick an animation:

```c
const ScenePlatformer *p = &level.player.platformer;
const char *animation = !p->grounded ? (p->rising ? "jump" : "fall") : p->move != 0.0f ? "run" : "idle";
```

**Other input** (a gamepad, touch, a replay): set `manual_input`, then fill in the input
before each step. `jump_pressed` is cleared by the step, so set it on the frame the button
goes down:

```c
ScenePlatformer *p = &level.player.platformer;
p->manual_input = true;
p->move = GetGamepadAxisMovement(0, GAMEPAD_AXIS_LEFT_X);
p->jump_pressed = IsGamepadButtonPressed(0, GAMEPAD_BUTTON_RIGHT_FACE_DOWN);
p->jump_held = IsGamepadButtonDown(0, GAMEPAD_BUTTON_RIGHT_FACE_DOWN);
scene_manager_update(&scenes, GetFrameTime());
```

**Tuning while playing:** the settings are plain fields, so you can change them at runtime,
e.g. `level.player.platformer.jump_height = 200.0f;` for a power-up.

## Triggers and contacts

Read Box2D's events after a step. Shapes carry their entity's `SceneEntity *`, so compare
with your scene's fields' `entity`:

```c
static void collect_coin(LevelScene *level) {
    const b2SensorEvents events = b2World_GetSensorEvents(level->physics_world);
    int i;
    for (i = 0; i < events.beginCount; ++i) {
        SceneEntity *trigger = b2Shape_GetUserData(events.beginEvents[i].sensorShapeId);
        SceneEntity *visitor = b2Shape_GetUserData(events.beginEvents[i].visitorShapeId);
        if (trigger == &level->coin.entity && visitor == &level->player.entity) {
            level->coin.visible = false;
            b2Body_Disable(level->coin.body);
        }
    }
}
```

Contacts between solid colliders work the same with `b2World_GetContactEvents`. Box2D keeps
the events of the last fixed step only, so when a slow frame runs several steps, earlier
steps' events are gone by the time you read them.

## Building

- **The `box2d/` folder:** the export contains Box2D's `include/` and `src/`, copied from
  the Box2D path in Project Settings.
- **How it's built:** the Makefile compiles Box2D once into `bin/box2d/`, with C17 and its
  warnings off.
- **C11 for your code:** Box2D's headers need C11, which is why the exported code builds
  with `-std=c11`.
