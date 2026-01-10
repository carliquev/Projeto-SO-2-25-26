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

static struct Session session = {.id = -1};

static int read_msg(int fd, void *buf, size_t n) {
  size_t off = 0;
  while (off < n) {
    ssize_t r = read(fd, (char*)buf + off, n - off);
    if (r < 0) {
      if (errno == EINTR) continue;
      return -1;
    }
    if (r == 0) return -1; // EOF
    off += (size_t)r;
  }
  return 0;
}


static int write_msg(int fd, const void *buf, size_t n) {
  size_t off = 0;
  while (off < n) {
    ssize_t w = write(fd, (const char*)buf + off, n - off);
    if (w < 0) {
      if (errno == EINTR) continue;
      return -1;
    }
    if (w == 0) return -1;
    off += (size_t)w;
  }
  return 0;
}

int pacman_connect(char const *req_pipe_path, char const *notif_pipe_path, char const *server_pipe_path) {

  msg_registration_t msg_registration;
  msg_registration.op_code = 1;
  strcpy(msg_registration.req_pipe_path, req_pipe_path);
  strcpy(msg_registration.notif_pipe_path, notif_pipe_path);
  int server;

  while (1) {
    /* remove pipe if it exists */
    if (unlink(req_pipe_path) != 0 && errno != ENOENT) {
      perror("[ERR]: unlink(%s) failed");
      return -1;
      //exit(EXIT_FAILURE);
    }
    if (mkfifo(req_pipe_path, 0640) != 0) {
      perror("[ERR]: mkfifo failed");
      return -1;
      //exit(EXIT_FAILURE);
    }

    /* remove pipe if it exists */
    if (unlink(notif_pipe_path) != 0 && errno != ENOENT) {
      perror("[ERR]: unlink(%s) failed");
      return -1;
      //exit(EXIT_FAILURE);
    }

    if (mkfifo(notif_pipe_path, 0640) != 0) {
      perror("[ERR]: mkfifo failed");
      return -1;
      //exit(EXIT_FAILURE);
    }

    server = open(server_pipe_path, O_WRONLY);
    if (server >= 0) break;

    if (errno == ENOENT) {  // não existe ainda
      sleep_ms(100);
      continue;
    }
    perror("[ERR]: open failed");
    return -1;
  }

  int server_write = write_msg(server, &msg_registration, sizeof(msg_registration));
  if (server_write < 0) {
    perror("[ERR]: write failed");
    return -1;
    //exit(EXIT_FAILURE);
  }

  session.req_pipe = open(req_pipe_path, O_WRONLY);
  if (session.req_pipe == -1) {
    perror("[ERR]: open failed");
    return -1;
    //exit(EXIT_FAILURE);
  }

  session.notif_pipe = open(notif_pipe_path, O_RDONLY);
  if (session.notif_pipe == -1) {
    perror("[ERR]: open failed");
    return -1;
    //exit(EXIT_FAILURE);
  }

  strcpy(session.notif_pipe_path, notif_pipe_path);
  msg_reg_response_t response;
  int notif_read = read_msg(session.notif_pipe, &response, sizeof(msg_reg_response_t));

  if (notif_read == -1) {
    //fprintf(stderr, "[ERR]: read failed: %s\n", strerror(errno));
    perror("[ERR]: read failed");
    return -1;
    //exit(EXIT_FAILURE);
  }
  if (response.op_code==OP_CODE_CONNECT) {
    if (response.result==1) {
      return -1;
      //exit(EXIT_FAILURE);
    }

    strcpy(session.req_pipe_path, req_pipe_path);
  }

  return(0);
}

void pacman_play(char command) {
  msg_play_t msg_play;
  msg_play.op_code = OP_CODE_PLAY;
  msg_play.command = command;

  int notif_write = write_msg(session.req_pipe, &msg_play, sizeof(msg_play_t));
  if (notif_write < 0) {
    perror("[ERR]: write failed");
    //exit(EXIT_FAILURE); TODO
  }
}

int pacman_disconnect() {
  int req_write = write_msg(session.req_pipe, "2", 1);
  if (req_write < 0) {
    perror("[ERR]: write failed");
    return -1;
  }
  close(session.req_pipe);
  close(session.notif_pipe);
  return 0;
}

Board   receive_board_update() {
  msg_board_update_t msg_board;
  msg_board.op_code = 0;
  Board game_board;
  game_board.data = NULL;

  while (msg_board.op_code !=4) {
    int notif_read = read_msg(session.notif_pipe, &msg_board, sizeof(msg_board_update_t));
    if (notif_read == -1) {
      perror("[ERR]: read failed");
      return game_board; //TODO I DONT THINK THIS WORKS /////////////////////////////////////////////////////////////////////////////////////
      //exit(EXIT_FAILURE);
    }

    if (msg_board.op_code!=4) continue;
    int game_over = msg_board.game_over;
    if (game_over==2) {
      memset(&game_board, 0, sizeof(Board));
      game_board.game_over = 2;
      return game_board;
    }
    game_board.width = msg_board.width;
    game_board.height = msg_board.height;
    int board_dim = game_board.width * game_board.height;
    game_board.tempo = msg_board.tempo;
    game_board.victory = msg_board.victory;
    game_board.game_over = msg_board.game_over;
    game_board.accumulated_points = msg_board.points;
    game_board.data = malloc((board_dim)*sizeof(char));
    if (game_board.data == NULL){
      perror("Memory Exceeded");
      exit(EXIT_FAILURE);
    }

    if (game_board.game_over!=2) {
      notif_read = read_msg(session.notif_pipe, game_board.data, (board_dim)*sizeof(char));
      if (notif_read == -1) {
        //fprintf(stderr, "[ERR]: read failed: %s\n", strerror(errno));
        free(game_board.data);
        perror("[ERR]: read failed");
        exit(EXIT_FAILURE);
      }
    } else {
      game_board.data = NULL;
    }
    return game_board;
  }
  free(game_board.data);
  exit(EXIT_FAILURE);
}