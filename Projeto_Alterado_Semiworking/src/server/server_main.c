#define _DEFAULT_SOURCE
#include "board.h"
#include "game.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/types.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <pthread.h>
#include <stdbool.h> 
#include <fcntl.h>
#include <semaphore.h>
#include "sessions.h"

#define BUFFER_SIZE 10

typedef struct {
    char req_pipe[41];
    char notif_pipe[41];
} connection_request_t;

connection_request_t request_buffer[BUFFER_SIZE];
int buf_in = 0;
int buf_out = 0;

pthread_mutex_t buf_mutex = PTHREAD_MUTEX_INITIALIZER;
sem_t buf_slots;
sem_t buf_items;

char *global_levels_dir;

// tem de se fazer uma funçao para o write para ele esperar os bites
ssize_t read_exact(int fd, void *buf, size_t n) {
    size_t total = 0;
    char *p = buf;

    while (total < n) {
        ssize_t r = read(fd, p + total, n - total);
        if (r <= 0) return r;
        total += r;
    }
    return total;
}

void* ClientSessionThread(void *arg){
    session_t* session = (session_t*)arg;
    char msg[2];

    while(session->active==1){
        ssize_t text= read(session->req_fd,msg,2);
        if(text<=0){
            session->active=0;
            break;
        }
        if(msg[0]==2){
            session->active=0;
            break;  
        }
        if(msg[0]==3){
            char move=msg[1];
            pthread_rwlock_wrlock(&session->board->state_lock);
            if (session->board->pacmans != NULL) {
                pacman_t *p = &session->board->pacmans[0];
                p->moves[0].command= move;
                p->moves[0].turns_left = 1;
                p->moves[0].turns= 1;
                p->n_moves=1;
            }
            pthread_rwlock_unlock(&session->board->state_lock);
        }  
    }
    return NULL;
}

char TranslateDataToVisual(session_t *session, int position){
    char data_received= session->board->board[position].content;
    char data_portals=session->board->board[position].has_portal;
    char data_dots=session->board->board[position].has_dot;
    if (data_received=='W'){
        return '#';
    }
    if (data_received=='P'){
        return 'C';
    }
    if (data_received=='M'){
        return 'M';
    }
    if (data_portals==1){
        return '@';
    }
    if (data_dots ==1){
        return '.';
    }
    return ' ';
}

void ServerBoardThread(session_t *session, int victory, int game_over){
    if (session->board->width == 0) return;

    pthread_rwlock_rdlock(&session->board->state_lock);
    char Opcode = 4;
    
    write(session->notif_fd, &Opcode, sizeof(Opcode));
    write(session->notif_fd, &session->board->width, sizeof(int));
    write(session->notif_fd, &session->board->height, sizeof(int));
    write(session->notif_fd, &session->board->tempo, sizeof(int));
    write(session->notif_fd, &victory, sizeof(int));
    write(session->notif_fd, &game_over, sizeof(int));

    int points = 0;
    if(session->board->pacmans != NULL) {
        points = session->board->pacmans->points;
    }
    write(session->notif_fd, &points, sizeof(int));

    char *tabuleiro = malloc(session->board->width * session->board->height);
    for(int i = 0; i < session->board->width * session->board->height; i++){
        tabuleiro[i] = TranslateDataToVisual(session, i);
    }
    write(session->notif_fd, tabuleiro, session->board->width * session->board->height);
    
    pthread_rwlock_unlock(&session->board->state_lock);
    free(tabuleiro);    
}

void* board_updates(void *arg){
    session_t *session = arg;
    while (1) {
        sleep_ms(session->board->tempo);
        if(session->active)
        ServerBoardThread(session, 0, 0);
        else break;
    }
    return NULL;
}

void* session_worker(void* arg) {
    (void)arg;
    while (1) {
        // 1. Consumer: Extract request from buffer
        connection_request_t req;

        sem_wait(&buf_items);
        pthread_mutex_lock(&buf_mutex);
        
        req = request_buffer[buf_out];
        buf_out = (buf_out + 1) % BUFFER_SIZE;

        pthread_mutex_unlock(&buf_mutex);
        sem_post(&buf_slots);

        printf("Worker started session for client.\n");

        // 2. Initialize Session
        session_t session;
        memset(&session, 0, sizeof(session));
        session.active = 1;

        // 3. Connect to Client
        char response[2] = {1, 0};
        session.notif_fd = open(req.notif_pipe, O_WRONLY);
        if (session.notif_fd == -1) {
            continue; 
        }
        write(session.notif_fd, response, 2);
        
        session.req_fd = open(req.req_pipe, O_RDONLY);
        if (session.req_fd == -1) {
            close(session.notif_fd);
            continue;
        }

        // 4. Start Client Input Listener
        pthread_t session_thd;
        pthread_create(&session_thd, NULL, ClientSessionThread, &session);

        // 5. Game Logic
        board_t game_board;
        memset(&game_board, 0, sizeof(board_t));
        // Initialize rwlock for this board instance
        pthread_rwlock_init(&game_board.state_lock, NULL);

        session.board = &game_board;
        int accumulated_points = 0;
        bool end_game = false;

        // Use scandir to get levels so each thread iterates independently
        struct dirent **namelist;
        int n_levels = scandir(global_levels_dir, &namelist, NULL, alphasort);
        if (n_levels < 0) {
            perror("scandir");
        } else {
            for (int k = 0; k < n_levels && !end_game && session.active; k++) {
                char *d_name = namelist[k]->d_name;
                
                if (d_name[0] == '.') { free(namelist[k]); continue; }
                char *dot = strrchr(d_name, '.');
                if (!dot || strcmp(dot, ".lvl") != 0) { free(namelist[k]); continue; }

                printf("Loading Level: %s\n", d_name);
                load_level(&game_board, d_name, global_levels_dir, accumulated_points);
                
                // Send initial board state
                ServerBoardThread(&session, 0, 0);

                // Start periodic updates
                pthread_t board_thread;
                pthread_create(&board_thread, NULL, board_updates, &session);

                // Level Loop
                while(session.active) {
                    pthread_t pacman_tid;
                    pthread_t *ghost_tids = malloc(game_board.n_ghosts * sizeof(pthread_t));
                    
                    int local_thread_shutdown = 0; 
                    
                    // Reset shutdown for safety inside rwlock
                    pthread_rwlock_wrlock(&game_board.state_lock);
                    // (Any necessary resets)
                    pthread_rwlock_unlock(&game_board.state_lock);

                    pthread_create(&pacman_tid, NULL, pacman_thread, &session);
                    
                    for (int i = 0; i < game_board.n_ghosts; i++) {
                        ghost_thread_arg_t *arg = malloc(sizeof(ghost_thread_arg_t));
                        arg->board = &game_board;
                        arg->ghost_index = i;
                        arg->shutdown_flag = &local_thread_shutdown; 
                        pthread_create(&ghost_tids[i], NULL, ghost_thread, (void*) arg);
                    }
                    
                    int *retval;
                    pthread_join(pacman_tid, (void**)&retval);

                    // Signal ghosts to stop
                    pthread_rwlock_wrlock(&game_board.state_lock);
                    local_thread_shutdown = 1;
                    pthread_rwlock_unlock(&game_board.state_lock);

                    for (int i = 0; i < game_board.n_ghosts; i++) {
                        pthread_join(ghost_tids[i], NULL);
                    }
                    free(ghost_tids);

                    // Stop updates before processing result
                    pthread_cancel(board_thread); 
                    pthread_join(board_thread, NULL);

                    int result = 0;
                    if (retval) {
                        result = *retval;
                        free(retval);
                    }

                    if(result == NEXT_LEVEL) {
                        accumulated_points = game_board.pacmans[0].points;
                        ServerBoardThread(&session, 1, 0); // Victory
                        sleep_ms(game_board.tempo);
                        break; // Go to next level file
                    }

                    if(result == QUIT_GAME) {
                        ServerBoardThread(&session, 0, 1); // Game Over
                        sleep_ms(game_board.tempo);
                        end_game = true;
                        break;
                    }
                    if(result == CONTINUE_PLAY)
                        continue;

                    // If continue play (e.g. lost life but not game over), update points
                    accumulated_points = game_board.pacmans[0].points;
                    
                    // Restart update thread if we are continuing in the same level
                    if (session.active && !end_game) {
                         pthread_create(&board_thread, NULL, board_updates, &session);
                    }
                }
                
                unload_level(&game_board);
                free(namelist[k]);
            }
            free(namelist);
        }

        // Cleanup Session
        session.active = 0;
        pthread_cancel(session_thd); 
        pthread_join(session_thd, NULL);

        close(session.notif_fd);
        close(session.req_fd);
        pthread_rwlock_destroy(&game_board.state_lock);
        printf("Session ended.\n");
    }
    return NULL;
}

int main(int argc, char** argv) {

    if (argc != 4) {
        printf("Usage: %s <levels_dir> <max_games> <pipe_name>\n", argv[0]);
        return -1;
    }

    global_levels_dir = argv[1];
    int max_games = atoi(argv[2]);
    char *server_fifo = argv[3];

    srand((unsigned int)time(NULL));
    open_debug_file("serverdebug.log");

    // Initialize Semaphores
    sem_init(&buf_slots, 0, BUFFER_SIZE);
    sem_init(&buf_items, 0, 0);

    // Create a pool of worker threads
    pthread_t *workers = malloc(sizeof(pthread_t) * max_games);
    for(int i=0; i < max_games; i++) {
        pthread_create(&workers[i], NULL, session_worker, NULL);
    }

    mkfifo(server_fifo, 0666);
    int server_id = open(server_fifo, O_RDONLY);
    
    printf("Server listening on %s with %d worker threads.\n", server_fifo, max_games);

    // --- Host Thread Loop (Producer) ---
    while(1){
        char buffer[81];
        ssize_t n = read(server_id, buffer, 81);
        if(n<=0){
            // If writer closes, re-open to block again
            if (n == 0) {
                 close(server_id);
                 server_id = open(server_fifo, O_RDONLY);
            }
            continue;
        }
        if (buffer[0] != 1){
            continue;
        } 
        
        connection_request_t new_req;
        memcpy(new_req.req_pipe, &buffer[1], 40);
        memcpy(new_req.notif_pipe, &buffer[41], 40);
        new_req.req_pipe[40] = '\0';
        new_req.notif_pipe[40] = '\0';

        // Add to buffer
        sem_wait(&buf_slots);
        pthread_mutex_lock(&buf_mutex);
        
        request_buffer[buf_in] = new_req;
        buf_in = (buf_in + 1) % BUFFER_SIZE;

        pthread_mutex_unlock(&buf_mutex);
        sem_post(&buf_items);
    }

    close_debug_file();
    return 0;
}