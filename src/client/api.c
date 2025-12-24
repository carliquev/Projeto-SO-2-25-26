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
  char *data;
} msg_board_update_t;



static struct Session session = {.id = -1};

int pacman_connect(char const *req_pipe_path, char const *notif_pipe_path, char const *server_pipe_path) {

  msg_registration_t msg_registration;
  msg_registration.op_code = 1;
  strcpy(msg_registration.req_pipe_path, req_pipe_path);
  strcpy(msg_registration.notif_pipe_path, notif_pipe_path);

  if (mkfifo(server_pipe_path, 0640) != 0) {
    perror("[ERR]: mkfifo failed");
    exit(EXIT_FAILURE);
  }
  int server = open(server_pipe_path, O_WRONLY);
  if (server == -1) {
    perror("[ERR]: open failed");
    exit(EXIT_FAILURE);
  }
  ssize_t server_write = write(server, &msg_registration, sizeof(msg_registration));
  if (server_write < 0) {
    perror("[ERR]: write failed");
    exit(EXIT_FAILURE);
  }
  close(server);

  if (mkfifo(notif_pipe_path, 0640) != 0) {
    perror("[ERR]: mkfifo failed");
    exit(EXIT_FAILURE);
  }
  session.notif_pipe = open(notif_pipe_path, O_RDONLY);
  if (session.notif_pipe == -1) {
    perror("[ERR]: open failed");
    exit(EXIT_FAILURE);
  }
  strcpy(session.notif_pipe_path, notif_pipe_path);
  char notif_reader[2];
  int notif_read = 0;
  while (notif_read == 0) {
    notif_read = read(session.notif_pipe, notif_reader, 2);
  }
  if (notif_read == -1) {
    //fprintf(stderr, "[ERR]: read failed: %s\n", strerror(errno));
    perror("[ERR]: read failed");
    exit(EXIT_FAILURE);
  }
  if (strcmp(notif_reader, "11") == 0) {
     exit(EXIT_FAILURE);
  }
  if (strcmp(notif_reader, "10") == 0) {
    if (mkfifo(req_pipe_path, 0640) != 0) {
      perror("[ERR]: mkfifo failed");
      exit(EXIT_FAILURE);
    }
    session.req_pipe = open(req_pipe_path, O_RDONLY);
    if (session.req_pipe == -1) {
      perror("[ERR]: open failed");
      exit(EXIT_FAILURE);
    }
    strcpy(session.req_pipe_path, req_pipe_path);
  }
  return(0);

  /*if (unlink(FIFO_PATHNAME) != 0 && errno != ENOENT) {
    perror("[ERR]: unlink(%s) failed");
    exit(EXIT_FAILURE);
  }

  * create pipe *
  if (mkfifo(FIFO_PATHNAME, 0640) != 0) {
    perror("[ERR]: mkfifo failed");
    exit(EXIT_FAILURE);
  }

  **
   * open pipe for writing
   * this waits for someone to open it for reading
   *
  int tx = open(FIFO_PATHNAME, O_WRONLY);
  if (tx == -1) {
    perror("[ERR]: open failed");
    exit(EXIT_FAILURE);
  }*/
  // TODO - implement me
}

void pacman_play(char command) {

  // TODO - implement me

}

int pacman_disconnect() {
  char *notif_reader;
  int notif_read = 0;
  while (notif_read == 0) {
    notif_read = read(session.notif_pipe, notif_reader, 1);
  }
  if (notif_read == -1) {
    //fprintf(stderr, "[ERR]: read failed: %s\n", strerror(errno));
    perror("[ERR]: read failed");
    return -1;
  }
  if (strcmp(notif_reader, "2") == 0) {
    close(session.req_pipe);
    close(session.notif_pipe);
  }
  // TODO - implement me
  return 0;
}

Board receive_board_update(void) {
    // TODO - implement me
}