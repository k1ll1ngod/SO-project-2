#ifndef GAME_H
#define GAME_H

#include "board.h"

// Return codes
#define CONTINUE_PLAY 0
#define NEXT_LEVEL 1
#define QUIT_GAME 2
#define LOAD_BACKUP 3
#define CREATE_BACKUP 4

typedef struct {
    board_t *board;
    int ghost_index;
} ghost_thread_arg_t;

// If you want pacman_thread to work with board_t directly:
void* pacman_thread(void *arg);
void* ghost_thread(void *arg);


#endif
