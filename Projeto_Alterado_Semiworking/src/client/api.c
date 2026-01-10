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

int extractIdFromPipe (const char *pipe_path) {
  const char *last_underscore = strrchr(pipe_path, '_');
  if (last_underscore) {
      return atoi(last_underscore + 1);
  }
  return -1;
}

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

int pacman_connect(char const *req_pipe_path, char const *notif_pipe_path, char const *server_pipe_path) {
  mkfifo(req_pipe_path,0666);
  mkfifo(notif_pipe_path,0666);
  int server = open(server_pipe_path, O_WRONLY);
  if(server<0){
    unlink(req_pipe_path);
    unlink(notif_pipe_path);
    return 1;
  }
  int client_id= extractIdFromPipe(req_pipe_path);
  session.id=client_id;
  strncpy(session.req_pipe_path, req_pipe_path, MAX_PIPE_PATH_LENGTH);
  strncpy(session.notif_pipe_path, notif_pipe_path, MAX_PIPE_PATH_LENGTH);
  

  char connect[81]={0};
  connect[0]=1;
  strncpy(&connect[1],req_pipe_path,40);
  strncpy(&connect[41],notif_pipe_path,40);

  debug("Enviando mensagem de conexão ao servidor...\n");
  debug("  req_pipe: %s\n", req_pipe_path);
  debug("  notif_pipe: %s\n", notif_pipe_path);

  write(server, connect,sizeof(connect));

  close(server);

  session.notif_pipe = open(notif_pipe_path, O_RDONLY);
  if(session.notif_pipe<0){
    return 1;
  }

  char response[2];
  read(session.notif_pipe, response, 2);

  if (response[0] != 1 || response[1] != 0) {
    return 1;
  }
  session.req_pipe = open(req_pipe_path, O_WRONLY);

  return 0;
}

void pacman_play(char command) {
  char Opcode = 3;
  char instruction[2];
  instruction[0]=Opcode;
  instruction[1]= command;
  write(session.req_pipe,instruction, sizeof(instruction));
}

int pacman_disconnect() {
  char Opcode = 2;
  write(session.req_pipe,&Opcode, sizeof(Opcode));
  close(session.notif_pipe);
  close(session.req_pipe);
  unlink(session.notif_pipe_path);
  unlink(session.req_pipe_path);
  
  return 0;
}

Board receive_board_update() {
  debug("Entrou no board update \n");

  char Opcode[1];
  debug("AA");
  Board tabuleiro={0};
    debug("Estou na 110 \n");
  read(session.notif_pipe,Opcode,1);
  if (Opcode[0]==(char)4){
    debug("Estou na 113 \n");

    read_all(session.notif_pipe,&tabuleiro.width,sizeof(int));
    read_all(session.notif_pipe,&tabuleiro.height,sizeof(int));
    read_all(session.notif_pipe,&tabuleiro.tempo,sizeof(int));
    read_all(session.notif_pipe,&tabuleiro.victory,sizeof(int));
    read_all(session.notif_pipe,&tabuleiro.game_over,sizeof(int));
    read_all(session.notif_pipe,&tabuleiro.accumulated_points,sizeof(int));
    tabuleiro.data=malloc(tabuleiro.height * tabuleiro.width);
    read_all(session.notif_pipe,tabuleiro.data, tabuleiro.width * tabuleiro.height);//tabuleiro data
    debug("Saiu no board update \n");
    
    return tabuleiro;
    
  }
  return tabuleiro;
}