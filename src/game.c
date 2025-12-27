#include "board.h"
#include "display.h"
#include "debug.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/types.h>
#include <dirent.h>
#include <errno.h>
#include <unistd.h>
#include <sys/wait.h>
#include <pthread.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <semaphore.h>

#define CONTINUE_PLAY 0
#define NEXT_LEVEL 1
#define QUIT_GAME 2
#define LOAD_BACKUP 3
#define CREATE_BACKUP 4

#define DEFAULT 0
#define VICTORY 1
#define GAMEOVER 2
#define ENDGAME 3

#define CLIENT_REQUEST_SIZE 82
#define REQUEST_RESPONSE 3
#define CLIENT_DC_SIZE 1
#define CLIENT_PLAY_SIZE 2
#define STRING_MAX_SIZE 40
//int array?
#define NOTIF_SIZE 8

typedef struct {
    board_t *board;
    int req_rx;
} pacman_thread_arg_t;

typedef struct {
    board_t *board;
    int ghost_index;
    pthread_t pacman_tid;
} ghost_thread_arg_t;

typedef struct {
    board_t *board;
    int notif_tx;
} ncurses_thread_arg_t;

typedef struct {
    int *rx;
    int max_games;
    char directory_name[MAX_FILENAME];
} host_thread_arg_t;

typedef struct {
    char directory_name[MAX_FILENAME];
    int req_rx;
    int notif_tx;
} session_thread_arg_t;

typedef struct {
    int op_code;
    char req_pipe_path[STRING_MAX_SIZE + 1];
    char notif_pipe_path[STRING_MAX_SIZE + 1];
} msg_registration_t;

typedef struct {
    int op_code;
    int width;
    int height;
    int tempo;
    int victory;
    int game_over;
    int points;
}msg_board_update_t;

sem_t semaforo_clientes;
pthread_mutex_t mutex_clientes;

int thread_shutdown = 0;

void board_to_char(board_t *board, char** char_board) {

    for (int y = 0; y < board->height; y++) {
        for (int x = 0; x < board->width; x++) {
            int idx = y * board->width + x;
            char c = board->board[idx].content;

            int ghost_charged = 0;
            for (int g = 0; g < board->n_ghosts; g++) {
                ghost_t* ghost = &board->ghosts[g];
                if (ghost->pos_x == x && ghost->pos_y == y) {
                    if (ghost->charged)
                        ghost_charged = 1;
                    break;
                }
            }

            switch (c) {
                case 'W': // Wall
                    (*char_board)[idx] = '#';
                    break;

                case 'P': // Pacman
                    (*char_board)[idx] = 'C';
                    break;

                case 'M': // Monster/Ghost
                    if (ghost_charged) {
                        (*char_board)[idx] = 'G'; // Charged Monster/Ghost
                    } else {
                        (*char_board)[idx] = 'M';
                    }
                    break;

                case ' ': // Empty space
                    if (board->board[idx].has_portal) {
                        (*char_board)[idx] = '@';
                    }
                    else if (board->board[idx].has_dot) {
                        (*char_board)[idx] = '.';
                    }
                    else
                        (*char_board)[idx] = ' ';
                    break;

                default:
                    break;
            }
        }
    }
}

int create_backup() {
    // clear the terminal for process transition
    // terminal_cleanup();

    pid_t child = fork();

    if(child != 0) {
        if (child < 0) {
            return -1;
        }

        return child;
    } else {
        debug("[%d] Created\n", getpid());

        return 0;
    }
}

void screen_refresh(board_t * game_board, int mode) {
    debug("REFRESH\n");
    draw_board(game_board, mode);
    refresh_screen();     
}

void send_msg(int fd, char const *str, ssize_t len) {
    ssize_t written = 0;

    while (written < len) {
        ssize_t ret = write(fd, str + written, len - written);
        if (ret < 0) {
            perror("[ERR]: write failed");
            exit(EXIT_FAILURE);
        }

        written += ret;
    }
}

void update_client(int notif_pipe_fd, board_t *game_board, int mode) {
    int victory = 0, game_over = 0, op_code = 4;
    char *board_data;
    if (mode == VICTORY) {
        victory = 1;
    } else if (mode == GAMEOVER) {
        game_over = 1;
    } else if (mode == ENDGAME) {
        game_over = 2;
    }
    msg_board_update_t msg;
    msg.op_code = op_code;
    msg.victory = victory;
    msg.game_over = game_over;

    if (mode !=ENDGAME){
        msg.width = game_board->width;
        msg.height = game_board->height;
        msg.tempo = game_board->tempo;
        msg.points = game_board->pacmans[0].points;
        board_data = malloc((game_board->width * game_board->height) * sizeof(char));
        board_to_char(game_board, &board_data);
    } else{
        msg.width = 0;
        msg.height = 0;
        msg.tempo = 0;
        msg.points = 0;
        board_data = NULL;
    }
    

    ssize_t bytes = write(notif_pipe_fd, &msg, sizeof(msg_board_update_t));
    if (bytes < 0) {
        fprintf(stderr, "[ERR]: write failed\n");
        exit(EXIT_FAILURE);
    }
    send_msg(notif_pipe_fd, board_data, game_board->width * game_board->height);
}

void* ncurses_thread(void *arg) {
    ncurses_thread_arg_t *ncurses_thread_arg = (ncurses_thread_arg_t *) arg;
    board_t *board = ncurses_thread_arg->board;

    sleep_ms(board->tempo / 2);
    while (true) {
        sleep_ms(board->tempo);
        // pthread_rwlock_wrlock(&board->state_lock);
        if (thread_shutdown) {
            // pthread_rwlock_unlock(&board->state_lock);
            pthread_exit(NULL);
        }
        // screen_refresh(board, DRAW_MENU);
        update_client(ncurses_thread_arg->notif_tx, board, DEFAULT);
        // pthread_rwlock_unlock(&board->state_lock);
    }
}

void* pacman_thread(void *arg) {
    pacman_thread_arg_t *pacman_arg = (pacman_thread_arg_t *) arg;
    board_t *board = pacman_arg->board;
    int req_rx = pacman_arg->req_rx;

    pacman_t* pacman = &board->pacmans[0];

    while (true) {
        if(!pacman->alive) {
            board->state = QUIT_GAME;
            pthread_exit(NULL);
        }
        if (board->state != CONTINUE_PLAY) {
            pthread_exit(NULL);
        }

        sleep_ms(board->tempo * (1 + pacman->passo));

        command_t* play;
        command_t c;
        char client_play_buffer[CLIENT_PLAY_SIZE];

        ssize_t ret = read(req_rx, client_play_buffer, CLIENT_PLAY_SIZE);
        if (ret == -1) {
            // ret == -1 indicates error
            fprintf(stderr, "[ERR]: read failed: %s\n", strerror(errno));
            exit(EXIT_FAILURE);
        }

        c.command = client_play_buffer[1];

        if(c.command == '\0') {
            continue;
        }

        c.turns = 1;
        play = &c;


        debug("KEY %c\n", play->command);

        // QUIT
        if (play->command == 'Q') {
            board->state = QUIT_GAME;
            pthread_exit(NULL);
        }
        // // FORK
        // if (play->command == 'G') {
        //     *retval = CREATE_BACKUP;
        //     return (void*) retval;
        // }

        pthread_rwlock_rdlock(&board->state_lock);

        int result = move_pacman(board, 0, play);
        if (result == REACHED_PORTAL) {
            // Next level
            board->state = NEXT_LEVEL;
            break;
        }

        if(result == DEAD_PACMAN) {
            // Restart from child, wait for child, then quit
            board->state = QUIT_GAME;
            break;
        }

        pthread_rwlock_unlock(&board->state_lock);
    }
    pthread_rwlock_unlock(&board->state_lock);
    pthread_exit(NULL);
}

void* ghost_thread(void *arg) {
    ghost_thread_arg_t *ghost_arg = (ghost_thread_arg_t*) arg;
    board_t *board = ghost_arg->board;
    int ghost_ind = ghost_arg->ghost_index;
    pthread_t pacman_tid = ghost_arg->pacman_tid;

    free(ghost_arg);

    ghost_t* ghost = &board->ghosts[ghost_ind];

    while (true) {
        sleep_ms(board->tempo * (1 + ghost->passo));

        pthread_rwlock_rdlock(&board->state_lock);
        if (thread_shutdown || board->state != CONTINUE_PLAY) {
            pthread_rwlock_unlock(&board->state_lock);
            pthread_exit(NULL);
        }
        
        if (move_ghost(board, ghost_ind, &ghost->moves[ghost->current_move%ghost->n_moves])==DEAD_PACMAN) {
            board->state = QUIT_GAME;
            pthread_cancel(pacman_tid);
            pthread_rwlock_unlock(&board->state_lock);
            pthread_exit(NULL);
        }
        pthread_rwlock_unlock(&board->state_lock);
    }
}

void* session_thread(void *arg) {
    session_thread_arg_t *session_arg = (session_thread_arg_t*) arg;
    char directory_name[MAX_FILENAME];
    strcpy(directory_name, session_arg->directory_name);
    int req_rx = session_arg->req_rx;
    int notif_tx = session_arg->notif_tx;

    // Random seed for any random movements
    srand((unsigned int)time(NULL));

    DIR* level_dir = opendir(directory_name);

    if (level_dir == NULL) {
        fprintf(stderr, "Failed to open directory\n");
        return 0;
    }

    open_debug_file("debug.log");

    // terminal_init();

    board_t game_board;
    int accumulated_points = 0;
    bool end_game = false;


    // pid_t parent_process = getpid(); // Only the parent process can create backups

    struct dirent* entry;
    while ((entry = readdir(level_dir)) != NULL && !end_game) {
        if (entry->d_name[0] == '.') continue;

        char *dot = strrchr(entry->d_name, '.');
        if (!dot) continue;

        if (strcmp(dot, ".lvl") == 0) {
            load_level(&game_board, entry->d_name, directory_name, accumulated_points);
            game_board.state = CONTINUE_PLAY;
            update_client(notif_tx, &game_board, DEFAULT);
            // draw_board(&game_board, DRAW_MENU);
            // refresh_screen();

            while(true) {
                pthread_t ncurses_tid, pacman_tid;
                pthread_t *ghost_tids = malloc(game_board.n_ghosts * sizeof(pthread_t));

                thread_shutdown = 0;

                debug("Creating threads\n");

                pacman_thread_arg_t *pac_arg = malloc(sizeof(pacman_thread_arg_t));
                pac_arg->board = &game_board;
                pac_arg->req_rx = req_rx;
                pthread_create(&pacman_tid, NULL, pacman_thread, (void*) pac_arg);
                for (int i = 0; i < game_board.n_ghosts; i++) {
                    ghost_thread_arg_t *ghost_arg = malloc(sizeof(ghost_thread_arg_t));
                    ghost_arg->board = &game_board;
                    ghost_arg->ghost_index = i;
                    ghost_arg->pacman_tid = pacman_tid;
                    pthread_create(&ghost_tids[i], NULL, ghost_thread, (void*) ghost_arg);
                }
                ncurses_thread_arg_t *ncurses_arg = malloc(sizeof(ncurses_thread_arg_t));
                ncurses_arg->board = &game_board;
                ncurses_arg->notif_tx = notif_tx;
                pthread_create(&ncurses_tid, NULL, ncurses_thread, (void*) ncurses_arg);

                pthread_join(pacman_tid, NULL);

                pthread_rwlock_wrlock(&game_board.state_lock);
                thread_shutdown = 1;
                pthread_rwlock_unlock(&game_board.state_lock);

                pthread_join(ncurses_tid, NULL);
                for (int i = 0; i < game_board.n_ghosts; i++) {
                    pthread_join(ghost_tids[i], NULL);
                }

                free(ghost_tids);

                int result = game_board.state;

                if(result == NEXT_LEVEL) {
                    // screen_refresh(&game_board, DRAW_WIN);
                    update_client(notif_tx, &game_board, VICTORY);
                    sleep_ms(game_board.tempo);
                    break;
                }

                if(result == QUIT_GAME) {
                    // screen_refresh(&game_board, DRAW_GAME_OVER);
                    update_client(notif_tx, &game_board, GAMEOVER);
                    sleep_ms(game_board.tempo);
                    end_game = true;
                    break;
                }

                // screen_refresh(&game_board, DRAW_MENU);
                update_client(notif_tx, &game_board, DEFAULT);
                accumulated_points = game_board.pacmans[0].points;
            }
            // print_board(&game_board);
            if (game_board.pacmans[0].alive) {
                update_client(notif_tx, &game_board, VICTORY);
            }

            unload_level(&game_board);


        }
    }
    close_debug_file();
    // Se já não há mais níveis
    board_t end_board;
    update_client(notif_tx, &end_board, ENDGAME);
    char disconnect_message;

    ssize_t ret = read(req_rx, &disconnect_message, 1);
    if (ret != 1 && disconnect_message != '2') {
        fprintf(stderr, "[ERR]: client not disconnecting correctly\n");
        exit(EXIT_FAILURE);
    }
    close(req_rx);
    close(notif_tx);

    if (closedir(level_dir) == -1) {
        fprintf(stderr, "Failed to close directory\n");
        exit(EXIT_FAILURE);
    }

    return NULL;
}

void* host_thread(void *arg) {
    host_thread_arg_t *host_arg = (host_thread_arg_t*) arg;
    int *reg_rx = host_arg->rx;
    // int max_games = host_arg->max_games;

    int result = 0;

    while (true) {
        msg_registration_t msg_reg;
        ssize_t ret = read(*reg_rx, &msg_reg, sizeof(msg_registration_t));
        if (ret == -1) {
            // ret == -1 indicates error
            fprintf(stderr, "[ERR]: read failed: %s\n", strerror(errno));
            result = 1;
        } else if (ret==0) {
            sleep_ms(100);
            continue;
        }

        int req_rx = open(msg_reg.req_pipe_path, O_RDONLY);
        if (req_rx == -1) {
            perror("[ERR]: req_pipe open failed");
            exit(EXIT_FAILURE);
        }

        int notif_tx = open(msg_reg.notif_pipe_path, O_WRONLY);
        if (notif_tx == -1) {
            perror("[ERR]: notif_pipe open failed");
            exit(EXIT_FAILURE);
        }

        pthread_t session_tid;
        session_thread_arg_t *s_arg = malloc(sizeof(session_thread_arg_t));
        strcpy(s_arg->directory_name, host_arg->directory_name);
        s_arg->req_rx = req_rx;
        s_arg->notif_tx = notif_tx;

        char response[REQUEST_RESPONSE];
        snprintf(response, sizeof(response), "%d%d", msg_reg.op_code, result);
        send_msg(notif_tx, response, REQUEST_RESPONSE);
        pthread_create(&session_tid, NULL, session_thread, (void*) s_arg);
        pthread_join(session_tid, NULL);
    }
    return NULL;
}



int main(int argc, char** argv) {
    if (argc != 4) {
        printf("Usage: %s <level_directory> <max_games> <nome_do_FIFO_de_registo>\n", argv[0]);
        return -1;
    }

    int max_games = atoi(argv[2]);
    // Inicializa semaforo
    sem_init(&semaforo_clientes, 0, max_games);

    char* reg_pipe_pathname = argv[3];

    /* remove pipe if it exists */
    if (unlink(reg_pipe_pathname) != 0 && errno != ENOENT) {
        perror("[ERR]: unlink(%s) failed");
        exit(EXIT_FAILURE);
    }

    /* create pipe */
    if (mkfifo(reg_pipe_pathname, 0640) != 0) {
        perror("[ERR]: mkfifo failed");
        exit(EXIT_FAILURE);
    }

    int reg_rx = open(reg_pipe_pathname, O_RDONLY);
    if (reg_rx == -1) {
        perror("[ERR]: open failed");
        exit(EXIT_FAILURE);
    }

    pthread_t host_tid;
    host_thread_arg_t *arg = malloc(sizeof(host_thread_arg_t));
    arg->rx = &reg_rx;
    arg->max_games = max_games;
    strcpy(arg->directory_name, argv[1]);
    pthread_create(&host_tid, NULL, host_thread, (void*) arg);
    pthread_join(host_tid, NULL);

    return 0;
}
