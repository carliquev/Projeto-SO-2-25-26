#include "api.h"

#include <errno.h>

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
} ;

typedef struct {
  int op_code;
  char req_pipe_path[MAX_PIPE_PATH_LENGTH + 1];
  char notif_pipe_path[MAX_PIPE_PATH_LENGTH + 1];
} msg_registration_t;

typedef struct {
  int op_code;
  int width;
  int height;
  int tempo;
  int victory;
  int game_over;
  int points;
} msg_board_update_t;



static struct Session session = {.id = -1};

int pacman_connect(char const *req_pipe_path, char const *notif_pipe_path, char const *server_pipe_path) {

  msg_registration_t msg_registration;
  msg_registration.op_code = 1;
  strcpy(msg_registration.req_pipe_path, req_pipe_path);
  strcpy(msg_registration.notif_pipe_path, notif_pipe_path);
  int server;

  while (1) {
    server = open(server_pipe_path, O_WRONLY);
    if (server >= 0) break;

    if (errno == ENOENT) {  // não existe ainda, ou não há reader
      sleep_ms(100);
      continue;
    }
    perror("[ERR]: open failed");
    return -1;
  }

  ssize_t server_write = write(server, &msg_registration, sizeof(msg_registration));
  if (server_write < 0) {
    perror("[ERR]: write failed");
    exit(EXIT_FAILURE);
  }

  /* remove pipe if it exists */
  if (unlink(req_pipe_path) != 0 && errno != ENOENT) {
    perror("[ERR]: unlink(%s) failed");
    exit(EXIT_FAILURE);
  }
  if (mkfifo(req_pipe_path, 0640) != 0) {
    perror("[ERR]: mkfifo failed");
    exit(EXIT_FAILURE);
  }

  /* remove pipe if it exists */
  if (unlink(notif_pipe_path) != 0 && errno != ENOENT) {
    perror("[ERR]: unlink(%s) failed");
    exit(EXIT_FAILURE);
  }

  if (mkfifo(notif_pipe_path, 0640) != 0) {
    perror("[ERR]: mkfifo failed");
    exit(EXIT_FAILURE);
  }

  session.req_pipe = open(req_pipe_path, O_WRONLY);
  if (session.req_pipe == -1) {
    perror("[ERR]: open failed");
    exit(EXIT_FAILURE);
  }

  session.notif_pipe = open(notif_pipe_path, O_RDONLY);
  if (session.notif_pipe == -1) {
    perror("[ERR]: open failed");
    exit(EXIT_FAILURE);
  }

  strcpy(session.notif_pipe_path, notif_pipe_path);
  char notif_reader[2];
  ssize_t notif_read = 0;
  notif_read = read(session.notif_pipe, notif_reader, 2);

  if (notif_read == -1) {
    //fprintf(stderr, "[ERR]: read failed: %s\n", strerror(errno));
    perror("[ERR]: read failed");
    exit(EXIT_FAILURE);
  }
  if (notif_reader[0]=='1') {
    if (notif_reader[1]=='1') {
      exit(EXIT_FAILURE);
    }

    strcpy(session.req_pipe_path, req_pipe_path);
  }

  return(0);
}

void pacman_play(char command) {
  ssize_t notif_write;
  char buffer[2];
  buffer[0] = '3'; //op_code
  buffer[1] = command;
  notif_write = write(session.req_pipe, buffer, sizeof(buffer));
  if (notif_write < 0) {
    perror("[ERR]: write failed");
    exit(EXIT_FAILURE);
  }
}

int pacman_disconnect() {
  ssize_t req_write;
  req_write = write(session.req_pipe, "2", 1);
  if (req_write < 0) {
    perror("[ERR]: write failed");
    return -1;
  }
  close(session.req_pipe);
  close(session.notif_pipe);
  return 0;
}

Board receive_board_update() {
  ssize_t notif_read = 0;
  msg_board_update_t msg_board;
  msg_board.op_code = 0;
  Board game_board;

  while (msg_board.op_code !=4) {
    notif_read = read(session.notif_pipe, &msg_board, sizeof(msg_board_update_t));
    if (notif_read == -1) {
      perror("[ERR]: read failed");
      exit(EXIT_FAILURE);
    }

    if (msg_board.op_code!=4) continue;


    game_board.width = msg_board.width;
    game_board.height = msg_board.height;
    int board_dim = game_board.width * game_board.height;
    game_board.tempo = msg_board.tempo;
    game_board.victory = msg_board.victory;
    game_board.game_over = msg_board.game_over;
    game_board.accumulated_points = msg_board.points;
    game_board.data = malloc((board_dim)*sizeof(char));

    notif_read = read(session.notif_pipe, game_board.data, board_dim);
    if (notif_read == -1) {
      //fprintf(stderr, "[ERR]: read failed: %s\n", strerror(errno));
      perror("[ERR]: read failed");
      exit(EXIT_FAILURE);
    }
    return game_board;
  }
  exit(EXIT_FAILURE);
}