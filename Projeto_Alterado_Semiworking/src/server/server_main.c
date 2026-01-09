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
#include "sessions.h"

//a
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
        pacman_t *p = &session->board->pacmans[0];
            p->moves[0].command= move;
            p->moves[0].turns_left = 1;
            p->moves[0].turns= 1;
            p->n_moves=1;
        pthread_rwlock_unlock(&session->board->state_lock);
        //pacman_play
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

void ServerBoardThread(session_t *session){
        pthread_rwlock_rdlock(&session->board->state_lock);
        char Opcode = 4;
        char content;
        write(session->notif_fd,&Opcode, sizeof(Opcode));
        
        write(session->notif_fd,&session->board->width, sizeof(int));//1
        write(session->notif_fd,&session->board->height, sizeof(int));//2
        write(session->notif_fd,&session->board->tempo, sizeof(int));//3

        int victory= 0 , game_over= 0;
        write(session->notif_fd,&victory, sizeof(int));//4
        write(session->notif_fd,&game_over, sizeof(int));//5

        write(session->notif_fd,&session->board->pacmans->points, sizeof(int));//6


        char *tabuleiro= malloc(session->board->width* session->board->height);
        for(int i=0;i<session->board->width* session->board->height;i++){
            content = TranslateDataToVisual(session, i);
            tabuleiro[i]= content;
        }
        write(session->notif_fd, tabuleiro,session->board->width* session->board->height);//8
        pthread_rwlock_unlock(&session->board->state_lock);
        free(tabuleiro);
        
}

void* board_updates(void *arg){
    session_t *session = arg;
    while(session->active){
        sleep_ms(session->board->tempo);
        if(session->active)
        ServerBoardThread(session);
    }
    return NULL;
}


int main(int argc, char** argv) {

    if (argc != 4) {
        printf("Usage: %s <levels_dir> <max_games> <pipe_name>\n", argv[0]);
        return -1;
    }

    char *server_fifo = argv[3];
    mkfifo(server_fifo,0666);
    int server_id= open(server_fifo,O_RDONLY);
    char req_pipe[41], notif_pipe[41];
    //PIPE DE REGISTO wtv
    while(1){
        char buffer[81];
        ssize_t n=read(server_id, buffer, 81);
        if(n<=0){
            continue;
        }
        if (buffer[0]!=1){
            continue;
        } 
        else{
            memcpy(req_pipe, &buffer[1], 40);
            memcpy(notif_pipe, &buffer[41], 40);  

            break;
        }
    }
    notif_pipe[40] = '\0';
    req_pipe[40] = '\0';

    session_t session;
    memset(&session,0,sizeof(session));
    session.active=1;
    char response[2];
    response[0]=1;
    response[1]=0;
    session.notif_fd=open(notif_pipe, O_WRONLY);
    write(session.notif_fd,response,2);

    session.req_fd=open(req_pipe,O_RDONLY);



    pthread_t session_thd;
    pthread_create(&session_thd,NULL,ClientSessionThread,&session);

    // Random seed for any random movements
    srand((unsigned int)time(NULL));

    DIR* level_dir = opendir(argv[1]);
        
    if (level_dir == NULL) {
        fprintf(stderr, "Failed to open directory: %s\n", argv[1]);
        return 0;
    }

    open_debug_file("serverdebug.log");
    
    int accumulated_points = 0;
    bool end_game = false;
    board_t game_board;

    session.board = &game_board;


    struct dirent* entry;
    while ((entry = readdir(level_dir)) != NULL && !end_game) {
        

        if (entry->d_name[0] == '.') continue;

        char *dot = strrchr(entry->d_name, '.');
        if (!dot) continue;

        if (strcmp(dot, ".lvl") == 0) {
            load_level(&game_board, entry->d_name, argv[1], accumulated_points);
            ServerBoardThread(&session);
            session.active=1;

            pthread_t board_thread;
            pthread_create(&board_thread, NULL, board_updates, &session);

            while(1) {

                pthread_t pacman_tid;
                pthread_t *ghost_tids = malloc(game_board.n_ghosts * sizeof(pthread_t));

                pthread_create(&pacman_tid, NULL, pacman_thread, &session);
                for (int i = 0; i < game_board.n_ghosts; i++) {
                    ghost_thread_arg_t *arg = malloc(sizeof(ghost_thread_arg_t));
                    arg->board = &game_board;
                    arg->ghost_index = i;
                    pthread_create(&ghost_tids[i], NULL, ghost_thread, (void*) arg);
                }
                int *retval;

                pthread_join(pacman_tid, (void**)&retval);

                pthread_rwlock_wrlock(&game_board.state_lock);
                thread_shutdown = 1;
                pthread_rwlock_unlock(&game_board.state_lock);

                for (int i = 0; i < game_board.n_ghosts; i++) {
                    pthread_join(ghost_tids[i], NULL);
                }

                free(ghost_tids);

                int result = *retval;
                free(retval);

                if(result == NEXT_LEVEL) {
                    sleep_ms(game_board.tempo);
                    break;
                }

                if(result == QUIT_GAME) {
                    sleep_ms(game_board.tempo);
                    end_game = true;
                    break;
                }

                accumulated_points = game_board.pacmans[0].points;      
            }
            unload_level(&game_board);
        }
    }    

    close_debug_file();

    if (closedir(level_dir) == -1) {
        fprintf(stderr, "Failed to close directory\n");
        return 0;
    }
    return 0;
}
