// pong.h - Pong game header
#ifndef PONG_H
#define PONG_H

#include "../vga/vga.h"

#define GAME_WIDTH 60
#define GAME_HEIGHT 20
#define MAX_SNAKE_LENGTH 300
#define INITIAL_SPEED 150
#define SPEED_INCREMENT 5
#define MIN_SPEED 50

typedef enum {
    DIR_UP,
    DIR_DOWN,
    DIR_LEFT,
    DIR_RIGHT
} Direction;

typedef struct {
    int x;
    int y;
} Point;

typedef struct {
    Point segments[MAX_SNAKE_LENGTH];
    int length;
    Direction direction;
    Direction next_direction;  // Buffer for direction changes
    bool alive;
} Snake;

typedef struct {
    Point position;
    bool exists;
} Food;

typedef struct {
    vga_window_t gh_win;
    Snake snake;
    Food food;
    int score;
    int speed;
    bool gh_running;
    bool paused;
    uint32_t last_move_time;  // For non-blocking movement
} SnakeGame;


// Function to start and run the Pong game
void play_pong(void);
void init_snake(Snake* snake);
#endif // PONG_H