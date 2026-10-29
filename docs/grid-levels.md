# Grid levels

A level made of cells, such as a maze, a puzzle board or a roguelike room, with a fixed number of
rows and columns. The editor places the board on the screen. Your code keeps the grid,
generates the level and moves the player from cell to cell.

## In the editor

1. **Board:** an entity with a rectangle sprite, at position (0, 0). Make its size divide
   evenly by your columns and rows: 640 × 480 gives 20 × 15 cells of 32 units. Keep the
   sprite's **Export** box ticked: the code reads `board.sprite.size`.
2. **Player:** an entity whose origin is Board, about one cell in size. Its position is
   measured from the board's centre, so moving it to a cell is setting its position.

No physics is needed: moving is a lookup in your grid.

## Centred and as big as possible

- **Centred:** the scene origin is always at the window's centre, so a board at (0, 0)
  stays centred on every screen.
- **As big as possible:** set the design resolution (Project Settings) to the board plus a
  margin, e.g. 720 × 540 for a 640 × 480 board. With Expand, that whole area always fits
  the window, so the board fills it on its tighter axis, never cropped or stretched.
- **HUD:** UI elements pinned to the screen's corners stay outside the board on wide
  screens.

## The code

For a scene called `level`:

```c
#include <stdlib.h>
#include "level.h"

#define COLS 20
#define ROWS 15

typedef enum { FLOOR, WALL } Tile;
static Tile grid[ROWS][COLS];
static int player_col = 1, player_row = 1;

static Vector2 cell_size(const LevelScene *s) {
    return (Vector2){s->board.sprite.size.x / COLS, s->board.sprite.size.y / ROWS};
}

/* A cell's centre, from the board's centre (the board's own units). */
static Vector2 cell_center(const LevelScene *s, int col, int row) {
    const Vector2 cell = cell_size(s);
    return (Vector2){(col + 0.5f) * cell.x - s->board.sprite.size.x / 2.0f,
                     (row + 0.5f) * cell.y - s->board.sprite.size.y / 2.0f};
}

/* Walls around the edge and at random inside; the start stays clear. */
static void generate_level(void) {
    for (int r = 0; r < ROWS; ++r) {
        for (int c = 0; c < COLS; ++c) {
            const bool edge = r == 0 || c == 0 || r == ROWS - 1 || c == COLS - 1;
            grid[r][c] = edge || rand() % 5 == 0 ? WALL : FLOOR;
        }
    }
    grid[player_row][player_col] = FLOOR;
}

static void move_player(LevelScene *s) {
    int c = player_col, r = player_row;
    if (IsKeyPressed(KEY_RIGHT)) c++;
    if (IsKeyPressed(KEY_LEFT)) c--;
    if (IsKeyPressed(KEY_DOWN)) r++;
    if (IsKeyPressed(KEY_UP)) r--;
    if (grid[r][c] == FLOOR) { /* the edge walls keep r and c inside the grid */
        player_col = c;
        player_row = r;
    }
    s->player.transform.position = cell_center(s, player_col, player_row);
}

/* Each wall drawn in its cell, wherever the board is. */
static void draw_walls(const LevelScene *s) {
    const SceneSprite wall = {cell_size(s), DARKGRAY};
    for (int r = 0; r < ROWS; ++r) {
        for (int c = 0; c < COLS; ++c) {
            if (grid[r][c] == WALL) {
                const SceneTransform t = {.position = cell_center(s, c, r), .scale = {1.0f, 1.0f}};
                scene_draw_rectangle(wall, scene_child_pose(s->board.pose, t));
            }
        }
    }
}

int main(void) {
    static LevelScene level;
    SetConfigFlags(FLAG_WINDOW_RESIZABLE);
    InitWindow((int)SCENE_DESIGN_WIDTH, (int)SCENE_DESIGN_HEIGHT, "Grid game");
    SetTargetFPS(60);
    scene_level_init(&level);
    generate_level();
    while (!WindowShouldClose()) {
        move_player(&level);
        BeginDrawing();
        ClearBackground(BLACK);
        scene_begin();
        scene_level_draw(&level); /* the board and the player */
        draw_walls(&level);       /* after: the poses are updated by now */
        scene_end();
        EndDrawing();
    }
    CloseWindow();
    return 0;
}
```

- **Placement:** `scene_child_pose()` places each cell from the board's pose, so the grid
  follows the board if you move, rotate or scale it in the editor.
- **Grid size:** change `COLS` and `ROWS` in the code, and the board's size in the editor
  to match.

## Going further

- **Smooth moves:** keep the target cell and move `player.transform.position` part of the
  way to `cell_center()` each frame.
- **More tiles:** add values to `Tile` (coin, exit, enemy) and draw each its own way, e.g.
  `DrawTexturePro` at the cell's pose.
- **Hand-made levels:** fill `grid` from a string map such as `"#####\n#P..#"` instead of
  generating it.
- **Pixel art:** cells are rarely a whole number of pixels, so tile edges can show thin
  seams. Round each tile's position and size to whole pixels, or keep the window a whole
  multiple of the design resolution.
