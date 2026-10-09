#include "board.h"
#include "game.h"
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

/**
 * @brief Thread function that controls the Pacman movement and game logic.
 * 
 * This thread continuously moves the Pacman character based on queued commands,
 * checks for collisions, portal interactions, and death conditions. It runs
 * until the game ends or the session becomes inactive.
 * 
 * @param arg Pointer to session_t containing the game session and board state
 * @return void* Pointer to an allocated integer containing the game result:
 *  - CONTINUE_PLAY: Game continues normally
 *  - NEXT_LEVEL: Pacman reached a portal
 *  - QUIT_GAME: Pacman died or session ended
 */
void* pacman_thread(void *arg) {
    session_t *session= arg;
    board_t *board = session->board;

    pacman_t* pacman = &board->pacmans[0];

    int *retval = malloc(sizeof(int));
    *retval= CONTINUE_PLAY;
    while (session->active) {
        pthread_rwlock_rdlock(&board->state_lock);
        int is_alive = pacman->alive;
        pthread_rwlock_unlock(&board->state_lock);
        
        if(!is_alive) {
            *retval = QUIT_GAME;
            return (void*) retval;
        }
        
        sleep_ms(board->tempo * (1 + pacman->passo));

        if (pacman->n_moves == 0) {
            continue;
        }

        command_t *play = &pacman->moves[pacman->current_move % pacman->n_moves];

        pthread_rwlock_wrlock(&board->state_lock);

        int result = move_pacman(board, 0, play);
        pacman->n_moves=0;

        pthread_rwlock_unlock(&board->state_lock);

        if (result == REACHED_PORTAL) {
            *retval=NEXT_LEVEL;
            return (void*) retval;
        }

        if(result == DEAD_PACMAN) {
            *retval = QUIT_GAME;
            return (void*) retval;
        }
    }
    return (void*) retval;
}

/**
 * @brief Thread function that controls a single ghost's movement.
 * 
 * This thread continuously moves a ghost character according to its
 * predefined movement pattern. It respects the shutdown flag for
 * graceful termination and uses locks for thread-safe board access.
 * 
 * @param arg Pointer to ghost_thread_arg_t structure containing:
 *            - board: Pointer to the game board
 *            - ghost_index: Index of this ghost in the ghosts array
 *            - shutdown_flag: Pointer to shutdown signal (set to 1 to stop)
 * @return NULL
 */
void* ghost_thread(void *arg) {
    ghost_thread_arg_t *ghost_arg = (ghost_thread_arg_t*) arg;
    board_t *board = ghost_arg->board;
    int ghost_ind = ghost_arg->ghost_index;
    int *shutdown = ghost_arg->shutdown_flag;

    free(ghost_arg);

    ghost_t* ghost = &board->ghosts[ghost_ind];

    while (1) {
        sleep_ms(board->tempo * (1 + ghost->passo));

        pthread_rwlock_rdlock(&board->state_lock);
        if (*shutdown) {
            pthread_rwlock_unlock(&board->state_lock);
            pthread_exit(NULL);
        }
        
        move_ghost(board, ghost_ind, &ghost->moves[ghost->current_move%ghost->n_moves]);
        pthread_rwlock_unlock(&board->state_lock);
    }
}