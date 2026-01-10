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
#include <signal.h>
#include "sessions.h"

volatile sig_atomic_t sigurs1_received =0;

#define BUFFER_SIZE 10

typedef struct {
    session_t *session;  // Ponteiro, não cópia!
    int client_id;
} session_entry_t;

session_entry_t *active_sessions = NULL; 
int num_active_sessions = 0;
int max_sessions=0;
pthread_mutex_t sessions_mutex = PTHREAD_MUTEX_INITIALIZER;

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

void sigusr1_handler(int sig) {
    (void)sig;
    sigurs1_received = 1;
}

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

int extractIdFromPipe (const char *pipe_path) {
    int id = -1;
      const char *last_underscore = strrchr(pipe_path, '_');
      if (last_underscore) {
          id = atoi(last_underscore + 1);
      }
      // Fallback: If ID is invalid (0 or -1), try to find ANY number in the string
      if (id <= 0) {
          const char *p = pipe_path;
          while (*p) {
              if (*p >= '0' && *p <= '9') {
                  id = atoi(p);
                  break;
              }
              p++;
          }
      }
      return id;
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
        pthread_rwlock_rdlock(&session->board->state_lock);
        if(session->board->thread_shutdown || !session->active){
            pthread_rwlock_unlock(&session->board->state_lock);  // IMPORTANT: unlock before breaking
            break;
        }
        pthread_rwlock_unlock(&session->board->state_lock);
        ServerBoardThread(session, 0, 0);
    }
    return NULL;
}

void add_session(session_t *session, int client_id){
    pthread_mutex_lock(&sessions_mutex);
    if(num_active_sessions < max_sessions){
        active_sessions[num_active_sessions].session = session;  // Guarda ponteiro
        active_sessions[num_active_sessions].client_id = client_id;
        num_active_sessions++;
        printf("Session added. Client ID: %d\n", client_id);
    }
    pthread_mutex_unlock(&sessions_mutex);
}

void remove_session(session_t *session_ptr){
    pthread_mutex_lock(&sessions_mutex);
    for(int i=0; i<num_active_sessions;i++){
        // Compare memory address, not ID, to avoid collisions when ID=0
        if (active_sessions[i].session == session_ptr) {
            active_sessions[i] = active_sessions[num_active_sessions - 1];
            num_active_sessions--;
            break;
        }
    }
    pthread_mutex_unlock(&sessions_mutex);
}

void create_top5(){
    FILE *fp = fopen("Top_5.txt","w");
    if(!fp)return;

    // Header styling
    fprintf(fp, "+------+--------+--------+\n");
    fprintf(fp, "|        TOP  5          |\n");
    fprintf(fp, "+------+--------+--------+\n");
    fprintf(fp, "| Rank |   ID   | Points |\n");
    fprintf(fp, "+------+--------+--------+\n");

    pthread_mutex_lock(&sessions_mutex);
    
    typedef struct { int id; int points; } top_entry_t;
    
    // Safer allocation than VLA for variable sizes
    int count = num_active_sessions;
    top_entry_t *entries = NULL;
    if (count > 0) {
        entries = malloc(count * sizeof(top_entry_t));
    }
    
    if (entries) {
        for (int i = 0; i < count; i++) {
            entries[i].id = active_sessions[i].client_id;
            entries[i].points = 0;
            
            session_t *sess = active_sessions[i].session;
            if (sess && sess->board) {
                // Must lock the board to safely read pacman pointer and points
                // otherwise a worker performing unload_level/load_level causes segfault
                pthread_rwlock_rdlock(&sess->board->state_lock);
                if (sess->board->pacmans != NULL) {
                    entries[i].points = sess->board->pacmans[0].points;
                }
                pthread_rwlock_unlock(&sess->board->state_lock);
            }
        }

        // Bubble sort descending
        for (int i = 0; i < count - 1; i++) {
            for (int j = 0; j < count - i - 1; j++) {
                if (entries[j].points < entries[j+1].points) {
                    top_entry_t temp = entries[j];
                    entries[j] = entries[j+1];
                    entries[j+1] = temp;
                }
            }
        }
    }
    pthread_mutex_unlock(&sessions_mutex);

    // Write Top 5 Table (Always 5 rows)
    for (int i = 0; i < 5; i++) {
        if (entries && i < count) {
            fprintf(fp, "|  #%d  | %-6d | %-6d |\n", i+1, entries[i].id, entries[i].points);
        } else {
            fprintf(fp, "|  #%d  |        |        |\n", i+1);
        }
    }

    fprintf(fp, "+------+--------+--------+\n");

    if (entries) free(entries);
    fclose(fp);
    printf("Top 5 board updated\n");
}

void* session_worker(void* arg) {
    (void)arg;
    sigset_t set;
    sigemptyset(&set);
    sigemptyset(&set);
    sigaddset(&set, SIGUSR1);
    pthread_sigmask(SIG_BLOCK,&set,NULL);

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

        int client_id = extractIdFromPipe(req.req_pipe);

        session.client_id = client_id;

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

        add_session(&session, client_id);

        int accumulated_points = 0;
        bool end_game = false;

        // Use scandir to get levels so each thread iterates independently
        struct dirent **namelist;
        int n_levels = scandir(global_levels_dir, &namelist, NULL, alphasort);
        if (n_levels < 0) {
            perror("scandir");
        } else {
            // Count actual .lvl files first
            int total_levels = 0;
            for (int i = 0; i < n_levels; i++) {
                char *d_name = namelist[i]->d_name;
                if (d_name[0] == '.') continue;
                char *dot = strrchr(d_name, '.');
                if (dot && strcmp(dot, ".lvl") == 0) {
                    total_levels++;
                }
            }
            
            int current_level = 0;
            
            for (int k = 0; k < n_levels && !end_game && session.active; k++) {
                char *d_name = namelist[k]->d_name;
                
                if (d_name[0] == '.') { free(namelist[k]); continue; }
                char *dot = strrchr(d_name, '.');
                if (!dot || strcmp(dot, ".lvl") != 0) { free(namelist[k]); continue; }

                current_level++;
                printf("Loading Level: %s (%d/%d)\n", d_name, current_level, total_levels);
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

                    // Reset shutdown flags
                    pthread_rwlock_wrlock(&game_board.state_lock);
                    game_board.thread_shutdown = 0;
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
                    pthread_rwlock_wrlock(&game_board.state_lock);
                    game_board.thread_shutdown = 1;
                    pthread_rwlock_unlock(&game_board.state_lock);
                    
                    pthread_join(board_thread, NULL); 

                    int result = 0;
                    int current_points = 0;

                    if (retval) {
                        result = *retval;
                        free(retval);
                    }

                    // Save points BEFORE any cleanup
                    if (game_board.pacmans != NULL) {
                        current_points = game_board.pacmans[0].points;
                    }

                    if(result == NEXT_LEVEL) {
                        accumulated_points = current_points;
                        
                        // Only show victory if this is the LAST level
                        if (current_level == total_levels) {
                            ServerBoardThread(&session, 1, 0);  // Victory!
                            sleep_ms(1000);  // Show victory for 1 second
                            end_game = true;  // End the game after final level
                            
                            // Force client disconnect
                            session.active = 0;
                            if (session.notif_fd != -1) {
                                close(session.notif_fd);
                                session.notif_fd = -1;
                            }
                            if (session.req_fd != -1) {
                                close(session.req_fd);
                                session.req_fd = -1;
                            }
                        }
                        
                        break;  // Move to next level or end
                    }

                    if(result == QUIT_GAME) {
                        ServerBoardThread(&session, 0, 1);
                        sleep_ms(3000);
                        end_game = true;
                        break;
                    }

                    if(result == CONTINUE_PLAY) {
                        // If continue play (e.g. lost life but not game over), update points
                        accumulated_points = current_points;
                        
                        // Restart update thread if we are continuing in the same level
                        if (session.active && !end_game) {
                            pthread_create(&board_thread, NULL, board_updates, &session);
                        }
                        continue;
                    }
                }
                unload_level(&game_board);
                free(namelist[k]);
            }
            free(namelist);
        }

        // Cleanup Session
        session.active = 0;

        pthread_join(session_thd, NULL);
        remove_session(&session);

        // Only close if not already closed
        if (session.notif_fd != -1) {
            close(session.notif_fd);
        }
        if (session.req_fd != -1) {
            close(session.req_fd);
        }
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

    active_sessions = malloc(max_games * sizeof(session_entry_t));
    max_sessions=max_games;
    if (!active_sessions) {
        perror("malloc failed");
        return -1;
    }
    
    memset(active_sessions, 0, max_games * sizeof(session_entry_t));


    srand((unsigned int)time(NULL));
    open_debug_file("serverdebug.log");

    struct sigaction sa;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags=0;
    sa.sa_handler = sigusr1_handler;
    sigaction(SIGUSR1, &sa, NULL);

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


    while (1) {
        if (sigurs1_received) {
            sigurs1_received = 0;
            create_top5();
        }

        if (server_id == -1) {
            server_id = open(server_fifo, O_RDONLY);
            if (server_id == -1) continue;
        }
        
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
