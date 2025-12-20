#include "board.h"
#include "parser.h"
#include <stdlib.h>
#include <stdio.h> //snprintf
#include <fcntl.h>
#include <time.h>
#include <unistd.h>
#include <stdarg.h>
#include <pthread.h>

FILE * debugfile;

// Helper private function to find and kill pacman at specific position
static int find_and_kill_pacman(board_t* board, int new_x, int new_y) {
    for (int p = 0; p < board->n_pacmans; p++) {
        pacman_t* pac = &board->pacmans[p];
        if (pac->pos_x == new_x && pac->pos_y == new_y && pac->alive) {
            pac->alive = 0;
            kill_pacman(board, p);
            return DEAD_PACMAN;
        }
    }
    return VALID_MOVE;
}

// Helper private function for getting board position index
static inline int get_board_index(board_t* board, int x, int y) {
    return y * board->width + x;
}

// Helper private function for checking valid position
static inline int is_valid_position(board_t* board, int x, int y) {
    return (x >= 0 && x < board->width) && (y >= 0 && y < board->height); // Inside of the board boundaries
}

void sleep_ms(int milliseconds) {
    struct timespec ts;
    ts.tv_sec = milliseconds / 1000;
    ts.tv_nsec = (milliseconds % 1000) * 1000000;
    nanosleep(&ts, NULL);
}

int move_pacman(board_t* board, int pacman_index, command_t* command) {
    if (pacman_index < 0 || !board->pacmans[pacman_index].alive) {
        return DEAD_PACMAN; // Invalid or dead pacman
    }

    pacman_t* pac = &board->pacmans[pacman_index];
    int new_x = pac->pos_x;
    int new_y = pac->pos_y;

    // check passo
    if (pac->waiting > 0) {
        pac->waiting -= 1;
        return VALID_MOVE;        
    }
    pac->waiting = pac->passo;

    char direction = command->command;

    if (direction == 'R') {
        char directions[] = {'W', 'S', 'A', 'D'};
        direction = directions[rand() % 4];
    }

    // Calculate new position based on direction
    switch (direction) {
        case 'W': // Up
            new_y--;
            break;
        case 'S': // Down
            new_y++;
            break;
        case 'A': // Left
            new_x--;
            break;
        case 'D': // Right
            new_x++;
            break;
        case 'T': // Wait
            if (command->turns_left == 1) {
                pac->current_move += 1; // move on
                command->turns_left = command->turns;
            }
            else command->turns_left -= 1;
            return VALID_MOVE;
        default:
            return INVALID_MOVE; // Invalid direction
    }

    // Logic for the WASD movement
    pac->current_move+=1;

    // Check boundaries
    if (!is_valid_position(board, new_x, new_y)) {
        return INVALID_MOVE;
    }

    int new_index = get_board_index(board, new_x, new_y);
    int old_index = get_board_index(board, pac->pos_x, pac->pos_y);

    // locks
    if (old_index < new_index) {
        pthread_mutex_lock(&board->board[old_index].lock);
        pthread_mutex_lock(&board->board[new_index].lock);
    }
    else {
        pthread_mutex_lock(&board->board[new_index].lock);
        pthread_mutex_lock(&board->board[old_index].lock);
    }

    char target_content = board->board[new_index].content;

    if (board->board[new_index].has_portal) {
        board->board[old_index].content = ' ';
        board->board[new_index].content = 'P';
        return REACHED_PORTAL;
    }

    // Check for walls
    if (target_content == 'W') {
        goto move_pacman_invalid;
    }

    // Check for ghosts
    if (target_content == 'M') {
        kill_pacman(board, pacman_index);
        goto move_pacman_dead;
    }

    // Collect points
    if (board->board[new_index].has_dot) {
        pac->points++;
        board->board[new_index].has_dot = 0;
    }

    board->board[old_index].content = ' ';
    pac->pos_x = new_x;
    pac->pos_y = new_y;
    board->board[new_index].content = 'P';

    if (old_index < new_index) {
        pthread_mutex_unlock(&board->board[old_index].lock);
        pthread_mutex_unlock(&board->board[new_index].lock);
    }
    else {
        pthread_mutex_unlock(&board->board[new_index].lock);
        pthread_mutex_unlock(&board->board[old_index].lock);
    }
    
    return VALID_MOVE;

    move_pacman_invalid:
    if (old_index < new_index) {
        pthread_mutex_unlock(&board->board[old_index].lock);
        pthread_mutex_unlock(&board->board[new_index].lock);
    }
    else {
        pthread_mutex_unlock(&board->board[new_index].lock);
        pthread_mutex_unlock(&board->board[old_index].lock);
    }
    return INVALID_MOVE;

    move_pacman_dead:
    if (old_index < new_index) {
        pthread_mutex_unlock(&board->board[old_index].lock);
        pthread_mutex_unlock(&board->board[new_index].lock);
    }
    else {
        pthread_mutex_unlock(&board->board[new_index].lock);
        pthread_mutex_unlock(&board->board[old_index].lock);
    }
    return DEAD_PACMAN;
}

int move_ghost_charged(board_t* board, int ghost_index, char direction) {
    ghost_t* ghost = &board->ghosts[ghost_index];
    int x = ghost->pos_x;
    int y = ghost->pos_y;
    int new_x = x;
    int new_y = y;
    int result;

    ghost->charged = 0; //uncharge

    switch (direction) {
        case 'W':
            if (y == 0) return INVALID_MOVE;

            for (int i = 0; i <= y; i++) {
                pthread_mutex_lock(&board->board[i * board->width + x].lock);
            }

            new_y = 0; // In case there is no colision
            for (int i = y - 1; i >= 0; i--) {
                char target_content = board->board[i * board->width + x].content;
                if (target_content == 'W' || target_content == 'M') {
                    new_y = i + 1; // stop before colision
                    result = VALID_MOVE;
                    break;
                }
                else if (target_content == 'P') {
                    new_y = i;
                    result = find_and_kill_pacman(board, new_x, new_y);
                    break;
                }
            }

            for (int i = 0; i <= y; i++) {
                pthread_mutex_unlock(&board->board[i * board->width + x].lock);
            }
            break;
        case 'S':
            if (y == board->height - 1) return INVALID_MOVE;

            for (int i = y; i < board->height; i++) {
                pthread_mutex_lock(&board->board[i * board->width + x].lock);
            }

            new_y = board->height - 1; // In case there is no colision
            for (int i = y + 1; i < board->height; i++) {
                char target_content = board->board[i * board->width + x].content;
                if (target_content == 'W' || target_content == 'M') {
                    new_y = i - 1; // stop before colision
                    result = VALID_MOVE;
                    break;
                }
                else if (target_content == 'P') {
                    new_y = i;
                    result = find_and_kill_pacman(board, new_x, new_y);
                    break;
                }
            }

            for (int i = y; i < board->height; i++) {
                pthread_mutex_unlock(&board->board[i * board->width + x].lock);
            }
            break;
        case 'A':
            if (x == 0) return INVALID_MOVE;

            for (int j = 0; j <= x; j++) {
                pthread_mutex_lock(&board->board[y * board->width + j].lock);
            }

            new_x = 0; // In case there is no colision
            for (int j = x - 1; j >= 0; j--) {
                char target_content = board->board[y * board->width + j].content;
                if (target_content == 'W' || target_content == 'M') {
                    new_x = j + 1; // stop before colision
                    result = VALID_MOVE;
                    break;
                }
                else if (target_content == 'P') {
                    new_x = j;
                    result = find_and_kill_pacman(board, new_x, new_y);
                    break;
                }
            }

            for (int j = 0; j <= x; j++) {
                pthread_mutex_unlock(&board->board[y * board->width + j].lock);
            }
            break;
        case 'D':
            if (x == board->width - 1) return INVALID_MOVE;

            for (int j = x; j < board->width; j++) {
                pthread_mutex_lock(&board->board[y * board->width + j].lock);
            }

            new_x = board->width - 1; // In case there is no colision
            for (int j = x + 1; j < board->width; j++) {
                char target_content = board->board[y * board->width + j].content;
                if (target_content == 'W' || target_content == 'M') {
                    new_x = j - 1; // stop before colision
                    result = VALID_MOVE;
                    break;
                }
                else if (target_content == 'P') {
                    new_x = j;
                    result = find_and_kill_pacman(board, new_x, new_y);
                    break;
                }
            }

            for (int j = x; j < board->width; j++) {
                pthread_mutex_unlock(&board->board[y * board->width + j].lock);
            }
            break;
        default:
            debug("DEFAULT CHARGED MOVE - direction = %c\n", direction);
            return INVALID_MOVE;
    }

    board->board[y * board->width + x].content = ' '; // Or restore the dot if ghost was on one

    // Update ghost position
    ghost->pos_x = new_x;
    ghost->pos_y = new_y;

    // Update board - set new position
    board->board[new_y * board->width + new_x].content = 'M';
    return result;
}

int move_ghost(board_t* board, int ghost_index, command_t* command) {
    ghost_t* ghost = &board->ghosts[ghost_index];
    int new_x = ghost->pos_x;
    int new_y = ghost->pos_y;

    // check passo
    if (ghost->waiting > 0) {
        ghost->waiting -= 1;
        return VALID_MOVE;
    }
    ghost->waiting = ghost->passo;

    char direction = command->command;

    if (direction == 'R') {
        char directions[] = {'W', 'S', 'A', 'D'};
        direction = directions[rand() % 4];
    }

    // Calculate new position based on direction
    switch (direction) {
        case 'W': // Up
            new_y--;
            break;
        case 'S': // Down
            new_y++;
            break;
        case 'A': // Left
            new_x--;
            break;
        case 'D': // Right
            new_x++;
            break;
        case 'C': // Charge
            ghost->current_move += 1;
            ghost->charged = 1;
            return VALID_MOVE;
        case 'T': // Wait
            if (command->turns_left == 1) {
                ghost->current_move += 1; // move on
                command->turns_left = command->turns;
            }
            else command->turns_left -= 1;
            return VALID_MOVE;
        default:
            return INVALID_MOVE; // Invalid direction
    }

    // Logic for the WASD movement
    ghost->current_move++;
    if (ghost->charged)
        return move_ghost_charged(board, ghost_index, direction);

    // Check boundaries
    if (!is_valid_position(board, new_x, new_y)) {
        return INVALID_MOVE;
    }

    // Check board position
    int new_index = new_y * board->width + new_x;
    int old_index = ghost->pos_y * board->width + ghost->pos_x;

    // locks
    if (old_index < new_index) {
        pthread_mutex_lock(&board->board[old_index].lock);
        pthread_mutex_lock(&board->board[new_index].lock);
    }
    else {
        pthread_mutex_lock(&board->board[new_index].lock);
        pthread_mutex_lock(&board->board[old_index].lock);
    }

    char target_content = board->board[new_index].content;

    // Check for walls
    if (target_content == 'W') {
        goto move_ghost_invalid;
    }

    // Check for ghosts
    if (target_content == 'M') {
        goto move_ghost_invalid;
    }

    int result = VALID_MOVE;
    // Check for pacman
    if (target_content == 'P') {
        for (int i = 0; i < board->n_pacmans; i++) {
            pacman_t* pac = &board->pacmans[i];
            if (pac->pos_x == new_x && pac->pos_y == new_y && pac->alive) {
                pac->alive = 0; // Pacman dies
                kill_pacman(board, i);
                result = DEAD_PACMAN;
                break;
            }
        }
    }

    // Update board - clear old position (restore what was there)
    board->board[old_index].content = ' '; // Or restore the dot if ghost was on one
    // Update ghost position
    ghost->pos_x = new_x;
    ghost->pos_y = new_y;
    // Update board - set new position
    board->board[new_index].content = 'M';

    if (old_index < new_index) {
        pthread_mutex_unlock(&board->board[old_index].lock);
        pthread_mutex_unlock(&board->board[new_index].lock);
    }
    else {
        pthread_mutex_unlock(&board->board[new_index].lock);
        pthread_mutex_unlock(&board->board[old_index].lock);
    }
    
    return result;

    move_ghost_invalid:
    if (old_index < new_index) {
        pthread_mutex_unlock(&board->board[old_index].lock);
        pthread_mutex_unlock(&board->board[new_index].lock);
    }
    else {
        pthread_mutex_unlock(&board->board[new_index].lock);
        pthread_mutex_unlock(&board->board[old_index].lock);
    }
    return INVALID_MOVE;
}

void kill_pacman(board_t* board, int pacman_index) {
    debug("Killing %d pacman\n\n", pacman_index);
    pacman_t* pac = &board->pacmans[pacman_index];
    int index = pac->pos_y * board->width + pac->pos_x;

    // Remove pacman from the board
    board->board[index].content = ' ';

    // Mark pacman as dead
    pac->alive = 0;
}

// Static Loading
int load_pacman(board_t* board) {
    board->board[1 * board->width + 1].content = 'P'; // Pacman
    board->pacmans[0].pos_x = 1;
    board->pacmans[0].pos_y = 1;
    board->pacmans[0].alive = 1;
    board->pacmans[0].points = 0;
    return 0;
}

// Static Loading
int load_ghost(board_t* board) {
    board->board[4 * board->width + 8].content = 'M'; // Monster
    board->ghosts[0].pos_x = 8;
    board->ghosts[0].pos_y = 4;
    board->board[0 * board->width + 5].content = 'M'; // Monster
    board->ghosts[1].pos_x = 5;
    board->ghosts[1].pos_y = 0;
    return 0;
}

int load_level(board_t *board, char *filename, char* dirname, int points) {

    if (read_level(board, filename, dirname) < 0) {
        printf("Failed to load level\n");
        return -1;
    }

    if (read_pacman(board, points) < 0) {
        printf("Failed to load the pacman\n");
    }

    if (read_ghosts(board) < 0) {
        printf("Failed to read ghosts\n");
    }

    pthread_rwlock_init(&board->state_lock, NULL);

    for (int i = 0; i < board->height * board->width; i++) {
        pthread_mutex_init(&board->board[i].lock, NULL);
    }

    //print_board(board);
    return 0;
}

void unload_level(board_t * board) {
    pthread_rwlock_destroy(&board->state_lock);
    for (int i = 0; i < board->height * board->width; i++) {
        pthread_mutex_destroy(&board->board[i].lock);
    }
    free(board->board);
    free(board->pacmans);
    free(board->ghosts);
}

void open_debug_file(char *filename) {
    debugfile = fopen(filename, "w");
}

void close_debug_file() {
    fclose(debugfile);
}

void debug(const char * format, ...) {
    va_list args;
    va_start(args, format);
    vfprintf(debugfile, format, args);
    va_end(args);

    fflush(debugfile);
}

void print_board(board_t *board) {
    if (!board || !board->board) {
        debug("[%d] Board is empty or not initialized.\n", getpid());
        return;
    }

    // Large buffer to accumulate the whole output
    char buffer[8192];
    size_t offset = 0;

    offset += snprintf(buffer + offset, sizeof(buffer) - offset,
                       "=== [%d] LEVEL INFO ===\n"
                       "Dimensions: %d x %d\n"
                       "Tempo: %d\n"
                       "Pacman file: %s\n",
                       getpid(), board->height, board->width, board->tempo, board->pacman_file);

    offset += snprintf(buffer + offset, sizeof(buffer) - offset,
                       "Monster files (%d):\n", board->n_ghosts);

    for (int i = 0; i < board->n_ghosts; i++) {
        offset += snprintf(buffer + offset, sizeof(buffer) - offset,
                           "  - %s\n", board->ghosts_files[i]);
    }

    offset += snprintf(buffer + offset, sizeof(buffer) - offset, "\n=== BOARD ===\n");

    for (int y = 0; y < board->height; y++) {
        for (int x = 0; x < board->width; x++) {
            int idx = y * board->width + x;
            if (offset < sizeof(buffer) - 2) {
                buffer[offset++] = board->board[idx].content;
            }
        }
        if (offset < sizeof(buffer) - 2) {
            buffer[offset++] = '\n';
        }
    }

    offset += snprintf(buffer + offset, sizeof(buffer) - offset, "==================\n");

    buffer[offset] = '\0';

    debug("%s", buffer);
}
