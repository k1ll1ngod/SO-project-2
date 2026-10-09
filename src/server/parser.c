#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "parser.h"
#include "board.h"
#include <fcntl.h>

int read_level(board_t* board, char* filename, char* dirname) {

    char fullname[MAX_FILENAME];
    strcpy(fullname, dirname);
    strcat(fullname, "/");
    strcat(fullname, filename);

    int fd = open(fullname, O_RDONLY);
    if (fd == -1) {
        debug("Error opening file %s\n", fullname);
        return -1;
    }
    
    char command[MAX_COMMAND_LENGTH];

    // Pacman is optional
    board->pacman_file[0] = '\0';
    board->n_pacmans = 1;

    strcpy(board->level_name, filename);
    *strrchr(board->level_name, '.') = '\0';

    int read;
    while ((read = read_line(fd, command)) > 0) {

        // comment
        if (command[0] == '#' || command[0] == '\0') continue;

        char *word = strtok(command, " \t\n");
        if (!word) continue;  // skip empty line

        if (strcmp(word, "DIM") == 0) {
            char *arg1 = strtok(NULL, " \t\n");
            char *arg2 = strtok(NULL, " \t\n");
            if (arg1 && arg2) {
                board->width = atoi(arg1);
                board->height = atoi(arg2);
                debug("DIM = %d x %d\n", board->width, board->height);
            }
        }

        else if (strcmp(word, "TEMPO") == 0) {
            char *arg = strtok(NULL, " \t\n");
            if (arg) {
                board->tempo = atoi(arg);
                debug("TEMPO = %d\n", board->tempo);
            }
        }

        else if (strcmp(word, "PAC") == 0) {
            char *arg = strtok(NULL, " \t\n");
            if (arg) {
                snprintf(board->pacman_file, sizeof(board->pacman_file), "%s/%s", dirname, arg);
                debug("PAC = %s\n", board->pacman_file);
            }
        }

        else if (strcmp(word, "MON") == 0) {
            char *arg;
            int i = 0;
            while ((arg = strtok(NULL, " \t\n")) != NULL) {
                snprintf(board->ghosts_files[i], sizeof(board->ghosts_files[0]), "%s/%s", dirname, arg);
                debug("MON file: %s\n", board->ghosts_files[i]);
                i+= 1;
                if (i == MAX_GHOSTS-1) break;
            }
            board->n_ghosts = i;
        }

        else {
            break;
        }
    }

    if (!board->width || !board->height) {
        debug("Missing dimensions in level file\n");
        close(fd);
        return -1;
    }
    
    // the end of the file contains the grid
    board->board = calloc(board->width * board->height, sizeof(board_pos_t));
    board->pacmans = calloc(board->n_pacmans, sizeof(pacman_t));
    board->ghosts = calloc(board->n_ghosts, sizeof(ghost_t));

    int row = 0;
    // command here still holds the previous line
    while (read > 0) {
        if (command[0]== '#' || command[0] == '\0') continue;
        if (row >= board->height) break;

        debug("Line: %s\n", command);

        for (int col = 0; col < board -> width; col++){
            int idx = row * board->width + col;
            char content = command[col];

            switch (content) {
                case 'X': // wall
                    board->board[idx].content = 'W';
                    break;
                case '@': // portal
                    board->board[idx].content = ' ';
                    board->board[idx].has_portal = 1;
                    break;
                default:
                    board->board[idx].content = ' ';
                    board->board[idx].has_dot = 1;
                    break;
            }
        }

        row++;
        read = read_line(fd, command);
    }

    if (read == -1) {
      debug("Failed parsing line");
      close(fd);
      return read;
    }

    close(fd);
    return 0;
}

int read_pacman(board_t* board, int points) {
    pacman_t* pacman = &board->pacmans[0];
    pacman->alive = 1;
    pacman->points = points;

    // no file was provided -> defaults 
    if (board->pacman_file[0] == '\0') {
        pacman->passo = 0;
        pacman->waiting = 0;
        pacman->n_moves = 0; // user controlled
        // default position -> find first non occupied cell
        for (int i = 0; i < board->height; i++) {
            for (int j = 0; j < board->width; j++) {
                int idx = i * board->width + j;
                if (board->board[idx].content == ' ') {
                    pacman->pos_x = j;
                    pacman->pos_y = i;
                    board->board[idx].content = 'P';
                    goto pacman_inserted;
                }
            }
        }

        pacman_inserted:
        return 0;
    }

    int fd = open(board->pacman_file, O_RDONLY);

    int read;
    char command[MAX_COMMAND_LENGTH];
    while ((read = read_line(fd, command)) > 0) {
        // comment
        if (command[0] == '#' || command[0] == '\0') continue;

        char *word = strtok(command, " \t\n");
        if (!word) continue;  // skip empty line

        if (strcmp(word, "PASSO") == 0) {
            char *arg = strtok(NULL, " \t\n");
            if (arg) {
                pacman->passo = atoi(arg);
                pacman->waiting = pacman->passo;
                debug("Pacman passo: %d\n", pacman->passo);
            }
        }
        else if (strcmp(word, "POS") == 0) {
            char *arg1 = strtok(NULL, " \t\n");
            char *arg2 = strtok(NULL, " \t\n");
            if (arg1 && arg2) {
                pacman->pos_x = atoi(arg1);
                pacman->pos_y = atoi(arg2);
                int idx = pacman->pos_y * board->width + pacman->pos_x;
                board->board[idx].content = 'P';
                debug("Pacman Pos = %d x %d\n", pacman->pos_x, pacman->pos_y);
            }
        }
        else {
            break;
        }
    }

    // end of the file contains the moves
    pacman->current_move = 0;
    
    // command here still holds the previous line
    int move = 0;
    while (read > 0 && move < MAX_MOVES) {
        if (command[0]== '#' || command[0] == '\0') continue;
        if (command[0] == 'A' ||
            command[0] == 'D' ||
            command[0] == 'W' ||
            command[0] == 'S' ||
            command[0] == 'R' ||
            command[0] == 'G' ||  // FIXME: so para testar
            command[0] == 'Q') {  // FIXME: so para testar
                pacman->moves[move].command = command[0];
                pacman->moves[move].turns = 1;
                move += 1;
        }
        else if (command[0] == 'T' && command[1] == ' ') { 
            int t = atoi(command+2);
            if (t > 0) {
                pacman->moves[move].command = command[0];
                pacman->moves[move].turns = t;
                pacman->moves[move].turns_left = t;
                move += 1;
            }
        }

        read = read_line(fd, command);
    }
    pacman->n_moves = move;

    if (read == -1) {
        debug("Failed reading line\n");
        close(fd);
        return -1;
    }

    close(fd);
    return 0;
}


int read_ghosts(board_t* board) {
    for (int i = 0; i < board->n_ghosts; i++) {
        int fd = open(board->ghosts_files[i], O_RDONLY);
        ghost_t* ghost = &board->ghosts[i];

        int read;
        char command[MAX_COMMAND_LENGTH];
        while ((read = read_line(fd, command)) > 0) {
            // comment
            if (command[0] == '#' || command[0] == '\0') continue;

            char *word = strtok(command, " \t\n");
            if (!word) continue;  // skip empty line

            if (strcmp(word, "PASSO") == 0) {
                char *arg = strtok(NULL, " \t\n");
                if (arg) {
                    ghost->passo = atoi(arg);
                    ghost->waiting = ghost->passo;
                    debug("Ghost passo: %d\n", ghost->passo);
                }
            }
            else if (strcmp(word, "POS") == 0) {
                char *arg1 = strtok(NULL, " \t\n");
                char *arg2 = strtok(NULL, " \t\n");
                if (arg1 && arg2) {
                    ghost->pos_x = atoi(arg1);
                    ghost->pos_y = atoi(arg2);
                    int idx = ghost->pos_y * board->width + ghost->pos_x;
                    board->board[idx].content = 'M';
                    debug("Ghost Pos = %d x %d\n", ghost->pos_x, ghost->pos_y);
                }
            }
            else {
                break;
            }
        }

        // end of the file contains the moves
        ghost->current_move = 0;

        // command here still holds the previous line
        int move = 0;
        while (read > 0 && move < MAX_MOVES) {
            if (command[0]== '#' || command[0] == '\0') continue;
            if (command[0] == 'A' ||
                command[0] == 'D' ||
                command[0] == 'W' ||
                command[0] == 'S' ||
                command[0] == 'R' ||
                command[0] == 'C') {
                    ghost->moves[move].command = command[0];
                    ghost->moves[move].turns = 1; 
                    move += 1;
            }
            else if (command[0] == 'T' && command[1] == ' ') {
                int t = atoi(command+2);
                if (t > 0) {
                    ghost->moves[move].command = command[0];
                    ghost->moves[move].turns = t;
                    ghost->moves[move].turns_left = t;
                    move += 1;
                }
            }
            read = read_line(fd, command);
        }
        ghost->n_moves = move;

        if (read == -1) {
            debug("Failed reading line\n");
            close(fd);
            return -1;
        }
        close(fd);
    }

    return 0;
}

int read_line(int fd, char *buf) {
    int i = 0;
    char c;
    ssize_t n;

    while ((n = read(fd, &c, 1)) == 1) {
        if (c == '\r') continue;
        if (c == '\n') break;
        buf[i++] = c;
        if (i == MAX_COMMAND_LENGTH - 1) break;
    }

    buf[i] = '\0';
    if (n == -1) return -1;
    if (n == 0 && i == 0) return 0;
    return i;                         
}
