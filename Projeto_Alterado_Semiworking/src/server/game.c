#include "board.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/types.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/wait.h>
#include <pthread.h>
#include "sessions.h"

#define CONTINUE_PLAY 0
#define NEXT_LEVEL 1
#define QUIT_GAME 2
#define LOAD_BACKUP 3
#define CREATE_BACKUP 4

typedef struct {
    board_t *board;
    int ghost_index;
} ghost_thread_arg_t;

int thread_shutdown = 0;

void* pacman_thread(void *arg) {
    session_t *session= arg;
    board_t *board = session->board;

    pacman_t* pacman = &board->pacmans[0];

    int *retval = malloc(sizeof(int));

    while (1) {
        if(!pacman->alive) {
            return (void*) retval;
        }

        sleep_ms(board->tempo * (1 + pacman->passo));

    if (pacman->n_moves == 0) {
        continue;   // ou sleep e continua
    }

    command_t *play = &pacman->moves[pacman->current_move % pacman->n_moves];

        pthread_rwlock_wrlock(&board->state_lock);

        int result = move_pacman(board, 0, play);
        pacman->n_moves=0;

        pthread_rwlock_unlock(&board->state_lock);

        if (result == REACHED_PORTAL) {
            *retval=NEXT_LEVEL;
            return retval;
        }

        if(result == DEAD_PACMAN) {
            *retval = QUIT_GAME;
            // Restart from child, wait for child, then quit
            break;
        }

    }
    return (void*) retval;
}

void* ghost_thread(void *arg) {
    ghost_thread_arg_t *ghost_arg = (ghost_thread_arg_t*) arg;
    board_t *board = ghost_arg->board;
    int ghost_ind = ghost_arg->ghost_index;

    free(ghost_arg);

    ghost_t* ghost = &board->ghosts[ghost_ind];

    while (1) {
        sleep_ms(board->tempo * (1 + ghost->passo));

        pthread_rwlock_rdlock(&board->state_lock);
        if (thread_shutdown) {
            pthread_rwlock_unlock(&board->state_lock);
            pthread_exit(NULL);
        }
        
        move_ghost(board, ghost_ind, &ghost->moves[ghost->current_move%ghost->n_moves]);
        pthread_rwlock_unlock(&board->state_lock);
    }
}

