#ifndef PROTOCOL_H
#define PROTOCOL_H

#define MAX_PIPE_PATH_LENGTH 40
#define STRING_MAX_SIZE 40

enum {
  OP_CODE_CONNECT = 1,
  OP_CODE_DISCONNECT = 2,
  OP_CODE_PLAY = 3,
  OP_CODE_BOARD = 4,
};

typedef struct {
  int op_code;
  char req_pipe_path[MAX_PIPE_PATH_LENGTH + 1];
  char notif_pipe_path[MAX_PIPE_PATH_LENGTH + 1];
} msg_registration_t;

typedef struct {
  int op_code;
  int result;
}msg_reg_response_t;

typedef struct {
  int op_code;
  char command;
}msg_play_t;

typedef struct {
  int op_code;
  int width;
  int height;
  int tempo;
  int victory;
  int game_over;
  int points;
} msg_board_update_t;

#endif