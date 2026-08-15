// snake.c - Classic Snake gh using VGA windows
// VERSION: Non-blocking input (no keyboard_available needed)
#include <stdint.h>
#include <stdbool.h>

#include "../terminal/terminal.h"
#include "../vga/vga.h"
#include "../keyboard/keyboard.h"
#include "../timers/timer.h"
#include "pong.h"
// Global game state
SnakeGame gh;

// Random number generator (simple LCG)
uint32_t rand_seed = 12345;
int f(void) {
    rand_seed = rand_seed * 1103515245 + 12345;
    return (rand_seed / 65536) % 32768;
}

void init_snake(Snake* snake) {
    snake->length = 3;
    snake->direction = DIR_RIGHT;
    snake->next_direction = DIR_RIGHT;
    snake->alive = true;
    
    // Start in the middle
    int start_x = GAME_WIDTH / 2;
    int start_y = GAME_HEIGHT / 2;
    
    for (int i = 0; i < snake->length; i++) {
        snake->segments[i].x = start_x - i;
        snake->segments[i].y = start_y;
    }
}

void spawn_food(Food* food, Snake* snake) {
    bool valid_position;
    
    do {
        valid_position = true;
        food->position.x = 2 + (f() % (GAME_WIDTH - 4));
        food->position.y = 2 + (f() % (GAME_HEIGHT - 4));
        
        // Check if food spawns on snake
        for (int i = 0; i < snake->length; i++) {
            if (snake->segments[i].x == food->position.x && 
                snake->segments[i].y == food->position.y) {
                valid_position = false;
                break;
            }
        }
    } while (!valid_position);
    
    food->exists = true;
}

bool check_collision(Snake* snake) {
    Point head = snake->segments[0];
    
    // Check wall collision
    if (head.x <= 1 || head.x >= GAME_WIDTH - 2 || 
        head.y <= 1 || head.y >= GAME_HEIGHT - 2) {
        return true;
    }
    
    // Check self collision
    for (int i = 1; i < snake->length; i++) {
        if (head.x == snake->segments[i].x && head.y == snake->segments[i].y) {
            return true;
        }
    }
    
    return false;
}

void move_snake(Snake* snake, bool grow) {
    // Update direction (prevent 180-degree turns)
    if (snake->next_direction == DIR_UP && snake->direction != DIR_DOWN) {
        snake->direction = DIR_UP;
    } else if (snake->next_direction == DIR_DOWN && snake->direction != DIR_UP) {
        snake->direction = DIR_DOWN;
    } else if (snake->next_direction == DIR_LEFT && snake->direction != DIR_RIGHT) {
        snake->direction = DIR_LEFT;
    } else if (snake->next_direction == DIR_RIGHT && snake->direction != DIR_LEFT) {
        snake->direction = DIR_RIGHT;
    }
    
    // Move body segments
    if (!grow) {
        for (int i = snake->length - 1; i > 0; i--) {
            snake->segments[i] = snake->segments[i - 1];
        }
    } else {
        // Growing - shift all segments and add new head
        for (int i = snake->length; i > 0; i--) {
            snake->segments[i] = snake->segments[i - 1];
        }
        snake->length++;
    }
    
    // Move head
    switch (snake->direction) {
        case DIR_UP:
            snake->segments[0].y--;
            break;
        case DIR_DOWN:
            snake->segments[0].y++;
            break;
        case DIR_LEFT:
            snake->segments[0].x--;
            break;
        case DIR_RIGHT:
            snake->segments[0].x++;
            break;
    }
}

void draw_border(vga_window_t* win) {
    uint8_t border_color = vga_entry_color(VGA_COLOR_LIGHT_BROWN, VGA_COLOR_BLACK);
    
    // Top and bottom borders
    for (int x = 1; x < GAME_WIDTH - 1; x++) {
        vga_win_putc_colored(win, x, 1, 0xC4, border_color);
        vga_win_putc_colored(win, x, GAME_HEIGHT - 2, 0xC4, border_color);
    }
    
    // Left and right borders
    for (int y = 1; y < GAME_HEIGHT - 1; y++) {
        vga_win_putc_colored(win, 1, y, 0xB3, border_color);
        vga_win_putc_colored(win, GAME_WIDTH - 2, y, 0xB3, border_color);
    }
    
    // Corners
    vga_win_putc_colored(win, 1, 1, 0xDA, border_color);
    vga_win_putc_colored(win, GAME_WIDTH - 2, 1, 0xBF, border_color);
    vga_win_putc_colored(win, 1, GAME_HEIGHT - 2, 0xC0, border_color);
    vga_win_putc_colored(win, GAME_WIDTH - 2, GAME_HEIGHT - 2, 0xD9, border_color);
}

void draw_snake(vga_window_t* win, Snake* snake) {
    // Draw head
    vga_win_putc_colored(win, snake->segments[0].x, snake->segments[0].y, '@',
        vga_entry_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK));
    
    // Draw body
    for (int i = 1; i < snake->length; i++) {
        vga_win_putc_colored(win, snake->segments[i].x, snake->segments[i].y, 'O',
            vga_entry_color(VGA_COLOR_GREEN, VGA_COLOR_BLACK));
    }
}

void draw_food(vga_window_t* win, Food* food) {
    if (food->exists) {
        vga_win_putc_colored(win, food->position.x, food->position.y, '*',
            vga_entry_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK));
    }
}

void draw_score(vga_window_t* win, int score) {
    char score_text[32];
    strcpy(score_text, "Score: ");
    char score_num[16];
    itoa(score, score_num, 10);
    strcat(score_text, score_num);
    
    vga_win_puts_colored(win, 3, 0, score_text,
        vga_entry_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK));
}

void draw_controls(vga_window_t* win) {
    vga_win_puts_colored(win, 3, GAME_HEIGHT - 1, "Arrows: Move | P: Pause | ESC: Exit",
        vga_entry_color(VGA_COLOR_DARK_GREY, VGA_COLOR_BLACK));
}

// OPTION 1: If you have keyboard_available() function
// Use the version in /mnt/user-data/outputs/snake.c

// OPTION 2: If keyboard_available() doesn't exist, use this timer-based version
void snake_gh_loop_timer_based(void) {
    if (!gh.gh_running) {
        return;
    }
    
    gh.last_move_time = get_ticks();
    
    while (gh.gh_running) {
        uint32_t current_time = get_ticks();
        
        // Always check for input (assumes keyboard_key returns 0 if no key - common pattern)
        int key = keyboard_key();
        
        if (key != 0) {  // Only process if a key was actually pressed
            if (key == 0x01) { // ESC
                gh.gh_running = false;
                break;
            } else if (key == 0x19) { // P - Pause
                gh.paused = !gh.paused;
                if (gh.paused) {
                    vga_win_clear(&gh.gh_win);
                    draw_border(&gh.gh_win);
                    vga_win_puts_centered(&gh.gh_win, GAME_HEIGHT / 2, "PAUSED");
                    vga_win_puts_centered(&gh.gh_win, GAME_HEIGHT / 2 + 1, "Press P to resume");
                    vga_win_refresh(&gh.gh_win);
                    sleep_ms(200); // Debounce
                }
            } else if (!gh.paused) {
                // Arrow keys for direction
                if (key == 0x48) { // Up
                    gh.snake.next_direction = DIR_UP;
                } else if (key == 0x50) { // Down
                    gh.snake.next_direction = DIR_DOWN;
                } else if (key == 0x4B) { // Left
                    gh.snake.next_direction = DIR_LEFT;
                } else if (key == 0x4D) { // Right
                    gh.snake.next_direction = DIR_RIGHT;
                }
            }
        }
        
        // Skip gh logic if paused
        if (gh.paused) {
            sleep_ms(50);  // Small sleep to not burn CPU
            continue;
        }
        
        // Only move snake if enough time has passed
        if (current_time - gh.last_move_time >= gh.speed) {
            gh.last_move_time = current_time;
            
            // Move snake
            bool ate_food = false;
            Point head = gh.snake.segments[0];
            
            if (gh.food.exists && head.x == gh.food.position.x && 
                head.y == gh.food.position.y) {
                ate_food = true;
                gh.score += 10;
                gh.food.exists = false;
                
                // Increase speed slightly
                if (gh.speed > MIN_SPEED) {
                    gh.speed -= SPEED_INCREMENT;
                }
            }
            
            move_snake(&gh.snake, ate_food);
            
            // Check collision
            if (check_collision(&gh.snake)) {
                gh.snake.alive = false;
                gh.gh_running = false;
                break;
            }
            
            // Spawn new food if needed
            if (!gh.food.exists) {
                spawn_food(&gh.food, &gh.snake);
            }
            
            // Draw everything
            vga_win_clear(&gh.gh_win);
            draw_border(&gh.gh_win);
            draw_score(&gh.gh_win, gh.score);
            draw_snake(&gh.gh_win, &gh.snake);
            draw_food(&gh.gh_win, &gh.food);
            draw_controls(&gh.gh_win);
            vga_win_refresh(&gh.gh_win);
        }
        
        // Small sleep to prevent CPU spinning
        sleep_ms(10);
    }
}

void play_pong(void) { 
    
    // Initialize gh state (static, no malloc needed)
    memset(&gh, 0, sizeof(SnakeGame));
    
    // Create gh window
    gh.gh_win = vga_create_centered_window(GAME_WIDTH, GAME_HEIGHT,
        VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    vga_win_set_title(&gh.gh_win, "SNAKE - Arrow Keys to Move | P: Pause | ESC: Exit");
    
    // Initialize gh state
    init_snake(&gh.snake);
    gh.food.exists = false;
    spawn_food(&gh.food, &gh.snake);
    gh.score = 0;
    gh.speed = INITIAL_SPEED;
    gh.gh_running = true;
    gh.paused = false;
    
    // Seed random number generator with timer
    rand_seed = get_ticks();
    
    // Run gh loop - use timer-based version
    snake_gh_loop_timer_based();
    
    // Game over screen
    vga_win_clear(&gh.gh_win);
    
    if (gh.snake.alive) {
        vga_win_puts_centered(&gh.gh_win, GAME_HEIGHT / 2 - 2, "Game Exited");
    } else {
        vga_win_puts_centered(&gh.gh_win, GAME_HEIGHT / 2 - 2, "GAME OVER!");
    }
    
    char final_score[32];
    strcpy(final_score, "Final Score: ");
    char score_num[16];
    itoa(gh.score, score_num, 10);
    strcat(final_score, score_num);
    vga_win_puts_centered(&gh.gh_win, GAME_HEIGHT / 2, final_score);
    
    char length_text[32];
    strcpy(length_text, "Snake Length: ");
    char length_num[16];
    itoa(gh.snake.length, length_num, 10);
    strcat(length_text, length_num);
    vga_win_puts_centered(&gh.gh_win, GAME_HEIGHT / 2 + 1, length_text);
    
    vga_win_puts_centered(&gh.gh_win, GAME_HEIGHT / 2 + 3, "Press any key to exit...");
    vga_win_refresh(&gh.gh_win);
    keyboard_wait_for_key(0);
    
    // Clean up
    vga_destroy_window(&gh.gh_win);
    terminal_clear();
}