#include "api.h"
#include "protocol.h"
#include "debug.h"
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>
#include <stdlib.h>


struct Session {
  int id;
  int req_pipe;
  int notif_pipe;
  char req_pipe_path[MAX_PIPE_PATH_LENGTH + 1];
  char notif_pipe_path[MAX_PIPE_PATH_LENGTH + 1];
};

static struct Session session = {.id = -1};

/**
 * @brief Extracts the client ID from a pipe path.
 * 
 * Searches for the last underscore in the pipe path and extracts
 * the numeric ID that follows it.
 * 
 * @param pipe_path Path to the pipe (e.g., "/tmp/req_pipe_123")
 * @return int The extracted client ID, or -1 if no ID found
 */
int extractIdFromPipe (const char *pipe_path) {
  const char *last_underscore = strrchr(pipe_path, '_');
  if (last_underscore) {
      return atoi(last_underscore + 1);
  }
  return -1;
}

/**
 * @brief Reads exactly n bytes from a file descriptor.
 * 
 * Continuously reads from the file descriptor until exactly n bytes
 * have been read, handling partial reads.
 * 
 * @param fd File descriptor to read from
 * @param buf Buffer to store the read data
 * @param n Number of bytes to read
 * @return ssize_t Number of bytes read, or <= 0 on error/EOF
 */
ssize_t read_all(int fd, void *buf, size_t n) {
    size_t total = 0;
    char *p = buf;

    while (total < n) {
        ssize_t r = read(fd, p + total, n - total);
        if (r <= 0) return r;
        total += r;
    }
    return total;
}

/**
 * @brief Writes exactly n bytes to a file descriptor.
 * 
 * Continuously writes to the file descriptor until exactly n bytes
 * have been written, handling partial writes.
 * 
 * @param fd File descriptor to write to
 * @param buf Buffer containing data to write
 * @param n Number of bytes to write
 * @return ssize_t Number of bytes written, or <= 0 on error
 */
ssize_t write_all(int fd, const void *buf, size_t n) {
    size_t total = 0;
    const char *p = buf;

    while (total < n) {
        ssize_t w = write(fd, p + total, n - total);
        if (w <= 0)
            return w;
        total += w;
    }
    return total;
}

/**
 * @brief Connects the client to the Pacman server.
 * 
 * Creates named pipes for request and notification channels, sends a
 * connection request to the server, and waits for acknowledgment.
 * 
 * @param req_pipe_path Path for the request pipe (client -> server)
 * @param notif_pipe_path Path for the notification pipe (server -> client)
 * @param server_pipe_path Path to the server's main pipe
 * @return int 0 on success, 1 on failure
 */
int pacman_connect(char const *req_pipe_path, char const *notif_pipe_path, char const *server_pipe_path) {
  mkfifo(req_pipe_path,0666);
  mkfifo(notif_pipe_path,0666);
  int server = open(server_pipe_path, O_WRONLY);
  if(server < 0){
    unlink(req_pipe_path);
    unlink(notif_pipe_path);
    return 1;
  }
  int client_id = extractIdFromPipe(req_pipe_path);
  session.id = client_id;
  strncpy(session.req_pipe_path, req_pipe_path, MAX_PIPE_PATH_LENGTH);
  strncpy(session.notif_pipe_path, notif_pipe_path, MAX_PIPE_PATH_LENGTH);
  

  char connect[81] = {0};
  connect[0] = 1;
  strncpy(&connect[1], req_pipe_path,40);
  strncpy(&connect[41], notif_pipe_path,40);

  debug("Enviando mensagem de conexÃ£o ao servidor...\n");
  debug("  req_pipe: %s\n", req_pipe_path);
  debug("  notif_pipe: %s\n", notif_pipe_path);

  write_all(server, connect,sizeof(connect));

  close(server);

  session.notif_pipe = open(notif_pipe_path, O_RDONLY);
  if (session.notif_pipe < 0) {
    return 1;
  }

  char response[2];
  read_all(session.notif_pipe, response, 2);

  if (response[0] != 1 || response[1] != 0) {
    return 1;
  }
  session.req_pipe = open(req_pipe_path, O_WRONLY);

  return 0;
}

/**
 * @brief Sends a gameplay command to the server.
 * 
 * Transmits a player movement or action command through the
 * request pipe to the server.
 * 
 * @param command Character representing the command (e.g., 'w', 'a', 's', 'd')
 */
void pacman_play(char command) {
  char Opcode = 3;
  char instruction[2];
  instruction[0] = Opcode;
  instruction[1] = command;
  write_all(session.req_pipe, instruction, sizeof(instruction));
}

/**
 * @brief Disconnects the client from the server.
 * 
 * Sends a disconnect message to the server, closes all pipes,
 * and removes the named pipe files from the filesystem.
 * 
 * @return int Always returns 0
 */
int pacman_disconnect() {
  char Opcode = 2;
  write_all(session.req_pipe,&Opcode, sizeof(Opcode));
  close(session.notif_pipe);
  close(session.req_pipe);
  unlink(session.notif_pipe_path);
  unlink(session.req_pipe_path);
  
  return 0;
}

/**
 * @brief Receives a board update from the server.
 * 
 * Reads the current game board state from the notification pipe,
 * including dimensions, game status, points, and board data.
 * The caller is responsible for freeing the allocated board data.
 * 
 * @return Board Structure containing the updated game board.
 *              Returns a zero-initialized board if opcode is invalid.
 * @note The returned Board.data must be freed by the caller.
 */
Board receive_board_update() {
  debug("Entrou no board update \n");

  char Opcode[1];
  debug("AA");
  Board tabuleiro = {0};
  debug("Estou na 110 \n");
  read_all(session.notif_pipe,Opcode,1);
  if (Opcode[0] == (char)4){
    debug("Estou na 113 \n");

    read_all(session.notif_pipe,&tabuleiro.width,sizeof(int));
    read_all(session.notif_pipe,&tabuleiro.height,sizeof(int));
    read_all(session.notif_pipe,&tabuleiro.tempo,sizeof(int));
    read_all(session.notif_pipe,&tabuleiro.victory,sizeof(int));
    read_all(session.notif_pipe,&tabuleiro.game_over,sizeof(int));
    read_all(session.notif_pipe,&tabuleiro.accumulated_points,sizeof(int));
    tabuleiro.data=malloc(tabuleiro.height * tabuleiro.width);
    read_all(session.notif_pipe,tabuleiro.data, tabuleiro.width * tabuleiro.height);
    debug("Saiu no board update \n");
    
    return tabuleiro;
    
  }
  return tabuleiro;
}