#include "board.h"
#include "display.h"
#include "debug.h"
#include "protocol.h"
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
#include <signal.h>

#define CONTINUE_PLAY 0
#define NEXT_LEVEL 1
#define QUIT_GAME 2
#define LOAD_BACKUP 3
#define CREATE_BACKUP 4

#define DEFAULT 0
#define VICTORY 1
#define GAMEOVER 2
#define ENDGAME 3


//int array?
#define NOTIF_SIZE 8

typedef struct {
    int notif_tx;
    int req_rx;
    int thread_shutdown;
    int error;
    pthread_mutex_t lock;
} session_t;

typedef struct {
    board_t *board;
    session_t *session;
} pacman_thread_arg_t;

typedef struct {
    board_t *board;
    int ghost_index;
    pthread_t pacman_tid;
    session_t *session;
} ghost_thread_arg_t;

typedef struct {
    board_t *board;
    session_t *session;
} ncurses_thread_arg_t;

typedef struct {
    int *rx;
    char directory_name[MAX_FILENAME];
} host_thread_arg_t;

typedef struct {
    char directory_name[MAX_FILENAME];
    session_t *session;
} session_thread_arg_t;


sem_t semaforo_clientes;
pthread_mutex_t mutex_clientes;


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

void board_to_char(board_t *board, char* char_board) {

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
                    char_board[idx] = '#';
                    break;

                case 'P': // Pacman
                    char_board[idx] = 'C';
                    break;

                case 'M': // Monster/Ghost
                    if (ghost_charged) {
                        char_board[idx] = 'G'; // Charged Monster/Ghost
                    } else {
                        char_board[idx] = 'M';
                    }
                    break;

                case ' ': // Empty space
                    if (board->board[idx].has_portal) {
                        char_board[idx] = '@';
                    }
                    else if (board->board[idx].has_dot) {
                        char_board[idx] = '.';
                    }
                    else
                        char_board[idx] = ' ';
                    break;

                default:
                    break;
            }
        }
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
            //TODO ////////////////////////////////////////////////////////////////////////////////////////////
        }

        written += ret;
    }
}

int update_client(session_t *session, board_t *game_board, int mode) {
    int notif_pipe_fd = session->notif_tx;

    int victory = 0, game_over = 0, op_code = 4;
    char *board_data =NULL;
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
        board_data = malloc((game_board->width * game_board->height));
        board_to_char(game_board, board_data);
    } else{
        msg.width = 0;
        msg.height = 0;
        msg.tempo = 0;
        msg.points = 0;
    }

    pthread_mutex_lock(&session->lock);
    int written = write_msg(notif_pipe_fd, &msg, sizeof(msg_board_update_t));
    if (written < 0) {
        pthread_mutex_unlock(&session->lock);
        fprintf(stderr, "[ERR]: write failed\n");
        if (board_data) free(board_data);
        return -1;
    }
    if (mode!=ENDGAME) {
        send_msg(notif_pipe_fd, board_data, msg.width * msg.height);
    }
    pthread_mutex_unlock(&session->lock);
    if (board_data) free(board_data);
    return 0;
}

void* ncurses_thread(void *arg) {
    ncurses_thread_arg_t *ncurses_thread_arg = (ncurses_thread_arg_t *) arg;
    board_t *board = ncurses_thread_arg->board;
    session_t *session = ncurses_thread_arg->session;

    sleep_ms(board->tempo / 2);
    while (true) {
        sleep_ms(board->tempo);
        pthread_mutex_lock(&session->lock);
        int shutdown = session->thread_shutdown;
        pthread_mutex_unlock(&session->lock);
        if (shutdown) {
            pthread_exit(NULL);
        }
        // screen_refresh(board, DRAW_MENU);
        pthread_rwlock_wrlock(&board->state_lock);
        update_client(session, board, DEFAULT);
        pthread_rwlock_unlock(&board->state_lock);
    }
}

void* pacman_thread(void *arg) {
    pacman_thread_arg_t *pacman_arg = (pacman_thread_arg_t *) arg;
    board_t *board = pacman_arg->board;
    session_t *session = pacman_arg->session;
    int req_rx = session->req_rx;

    pacman_t* pacman = &board->pacmans[0];

    while (true) {
        if(!pacman->alive) {
            pthread_rwlock_rdlock(&board->state_lock);
            board->state = QUIT_GAME;
            pthread_rwlock_unlock(&board->state_lock);
            pthread_exit(NULL);
        }
        if (board->state != CONTINUE_PLAY) {
            pthread_exit(NULL);
        }

        sleep_ms(board->tempo * (1 + pacman->passo));

        command_t* play;
        command_t c;
        msg_play_t msg_play;

        int ret = read_msg(req_rx, &msg_play, sizeof(msg_play_t));
        //EOF
        if (ret == -1) {
            pthread_rwlock_rdlock(&board->state_lock);
            board->state = QUIT_GAME;
            pthread_rwlock_unlock(&board->state_lock);
            pthread_exit(NULL);
        }

        c.command = msg_play.command;

        if(c.command == '\0') {
            continue;
        }

        c.turns = 1;
        play = &c;

        debug("KEY %c\n", play->command);

        pthread_rwlock_rdlock(&board->state_lock);

        if (play->command == 'Q') {
            board->state = QUIT_GAME;
            pthread_rwlock_unlock(&board->state_lock);
            pthread_exit(NULL);
        }

        int result = move_pacman(board, 0, play);
        if (result == REACHED_PORTAL) {
            // Next level
            board->state = NEXT_LEVEL;
            pthread_rwlock_unlock(&board->state_lock);
            break;
        }

        if(result == DEAD_PACMAN) {
            // Restart from child, wait for child, then quit
            board->state = QUIT_GAME;
            pthread_rwlock_unlock(&board->state_lock);
            break;
        }

        pthread_rwlock_unlock(&board->state_lock);
    }

    pthread_exit(NULL);
}
void* ghost_thread(void *arg) {
    ghost_thread_arg_t *ghost_arg = (ghost_thread_arg_t*) arg;
    board_t *board = ghost_arg->board;
    int ghost_ind = ghost_arg->ghost_index;
    pthread_t pacman_tid = ghost_arg->pacman_tid;
    session_t *session = ghost_arg->session;

    // free(ghost_arg);

    ghost_t* ghost = &board->ghosts[ghost_ind];

    while (true) {
        sleep_ms(board->tempo * (1 + ghost->passo));

        pthread_mutex_lock(&session->lock);
        int shutdown = session->thread_shutdown;
        pthread_mutex_unlock(&session->lock);
        if (shutdown) pthread_exit(NULL);

        pthread_rwlock_rdlock(&board->state_lock);
        if (board->state != CONTINUE_PLAY) {
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
    session_t *session = session_arg->session;
    int req_rx = session->req_rx;
    int notif_tx = session->notif_tx;
    session->error = 0;


    //free(session_arg);
    // Random seed for any random movements
    srand((unsigned int)time(NULL));

    DIR* level_dir = opendir(directory_name);

    if (level_dir == NULL) {
        fprintf(stderr, "Failed to open directory\n");
        pthread_mutex_lock(&session->lock);
        session->error = 1;
        pthread_mutex_unlock(&session->lock);
        pthread_mutex_destroy(&session->lock);
        // free(session);
        sem_post(&semaforo_clientes);
        pthread_exit(NULL);
    }

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
            update_client(session, &game_board, DEFAULT);
            // draw_board(&game_board, DRAW_MENU);
            // refresh_screen();

            while(true) {
                pthread_t ncurses_tid, pacman_tid;
                pthread_t *ghost_tids = malloc(game_board.n_ghosts * sizeof(pthread_t));


                session->thread_shutdown = 0;

                debug("Creating threads\n");

                pacman_thread_arg_t *pac_arg = malloc(sizeof(pacman_thread_arg_t));
                pac_arg->board = &game_board;
                pac_arg->session = session;
                pthread_create(&pacman_tid, NULL, pacman_thread, (void*) pac_arg);
                for (int i = 0; i < game_board.n_ghosts; i++) {
                    ghost_thread_arg_t *ghost_arg = malloc(sizeof(ghost_thread_arg_t));
                    ghost_arg->board = &game_board;
                    ghost_arg->ghost_index = i;
                    ghost_arg->pacman_tid = pacman_tid;
                    ghost_arg->session = session;
                    pthread_create(&ghost_tids[i], NULL, ghost_thread, (void*) ghost_arg);
                }
                ncurses_thread_arg_t *ncurses_arg = malloc(sizeof(ncurses_thread_arg_t));
                ncurses_arg->board = &game_board;
                ncurses_arg->session = session;
                pthread_create(&ncurses_tid, NULL, ncurses_thread, (void*) ncurses_arg);

                pthread_join(pacman_tid, NULL);
                int err;
                pthread_mutex_lock(&session->lock);
                err = session->error;
                pthread_mutex_unlock(&session->lock);
                if (err==1) {
                    sem_post(&semaforo_clientes);
                    pthread_exit(NULL);
                }

                pthread_mutex_lock(&session->lock);
                session->thread_shutdown = 1;
                pthread_mutex_unlock(&session->lock);

                pthread_join(ncurses_tid, NULL);
                // free(ncurses_arg);
                for (int i = 0; i < game_board.n_ghosts; i++) {
                    pthread_join(ghost_tids[i], NULL);
                }

                free(ghost_tids);

                int result = game_board.state;

                if(result == NEXT_LEVEL) {
                    // screen_refresh(&game_board, DRAW_WIN);
                    update_client(session, &game_board, VICTORY);
                    sleep_ms(game_board.tempo);
                    break;
                }

                if(result == QUIT_GAME) {
                    // screen_refresh(&game_board, DRAW_GAME_OVER);
                    update_client(session, &game_board, GAMEOVER);
                    sleep_ms(game_board.tempo);
                    end_game = true;
                    break;
                }

                // screen_refresh(&game_board, DRAW_MENU);
                update_client(session, &game_board, DEFAULT);
                accumulated_points = game_board.pacmans[0].points;
            }
            // print_board(&game_board);
            // if (game_board.pacmans[0].alive) {
            //     update_client(session, &game_board, VICTORY);
            // }

            unload_level(&game_board);

        }
    }

    // Se já não há mais níveis
    board_t end_board;
    memset(&end_board, 0, sizeof(board_t));
    update_client(session, &end_board, ENDGAME);
    //receber disconnect
    while (true) {
        char msg;
        ssize_t bytes_read = read(req_rx, &msg, 1);

        if (bytes_read == 1) {
            if (msg == '2') break;
            continue;
        }
        if (bytes_read == 0) {
            break;
        }
        if (errno == EINTR) continue;
        perror("[ERR]: read disconnect failed");
        break;
    }

    close(req_rx);
    close(notif_tx);
    if (closedir(level_dir) == -1) {
        fprintf(stderr, "Failed to close directory\n");
        session->error = 1;
        pthread_mutex_destroy(&session->lock);
        // free(session);
        sem_post(&semaforo_clientes);
        pthread_exit(NULL);
    }
    pthread_mutex_destroy(&session->lock);
    // free(session);
    sem_post(&semaforo_clientes);
    pthread_exit(NULL);
}

void* host_thread(void *arg) {
    host_thread_arg_t *host_arg = (host_thread_arg_t*) arg;
    int *reg_rx = host_arg->rx;
    char directory_name[MAX_FILENAME];
    strcpy(directory_name, host_arg->directory_name);
    int result = 0;
    // free(host_arg);

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

        sem_wait(&semaforo_clientes);
        int req_rx = open(msg_reg.req_pipe_path, O_RDONLY);
        if (req_rx == -1) {
            perror("[ERR]: req_pipe open failed");
            result = 1;
        }

        int notif_tx = open(msg_reg.notif_pipe_path, O_WRONLY);
        if (notif_tx == -1) {
            perror("[ERR]: notif_pipe open failed");
            continue;
        }

        pthread_t session_tid;
        session_t *session = malloc(sizeof(session_t));
        pthread_mutex_init(&session->lock, NULL);
        session_thread_arg_t *s_arg = malloc(sizeof(session_thread_arg_t));
        strcpy(s_arg->directory_name, directory_name);
        session->req_rx = req_rx;
        session->notif_tx = notif_tx;
        session->thread_shutdown = 0;
        session->error = 0;
        s_arg->session = session;

        msg_reg_response_t response;
        response.op_code = msg_reg.op_code;
        response.result = result;

        int response_write = write_msg(notif_tx, &response, sizeof(msg_reg_response_t));
        if (response_write < 0) {
            free(s_arg);
            free(session);
            perror("[ERR]: write failed");
            exit(EXIT_FAILURE);
        }
        if (result == 1) {
            continue;
        }
        pthread_create(&session_tid, NULL, session_thread, (void*) s_arg);
        pthread_detach(session_tid);
        // pthread_join(session_tid, NULL);
    }
    pthread_exit(NULL);
}



int main(int argc, char** argv) {
    if (argc != 4) {
        printf("Usage: %s <level_directory> <max_games> <nome_do_FIFO_de_registo>\n", argv[0]);
        return -1;
    }
    //SIGPIPE não fecha o programa
    // signal(SIGPIPE, SIG_IGN);

    int max_games = atoi(argv[2]);
    // Inicializa semaforo
    sem_init(&semaforo_clientes, 0, max_games);
    open_debug_file("debug.log");

    char* reg_pipe_pathname = argv[3];

    /* remove pipe if it exists */
    if (unlink(reg_pipe_pathname) != 0 && errno != ENOENT) {
        perror("[ERR]: unlink(%s) failed");
        return EXIT_FAILURE;
        exit(EXIT_FAILURE);

    }

    /* create pipe */
    if (mkfifo(reg_pipe_pathname, 0640) != 0) {
        perror("[ERR]: mkfifo failed");
        return EXIT_FAILURE;
        //exit(EXIT_FAILURE);

    }

    int reg_rx = open(reg_pipe_pathname, O_RDONLY);
    if (reg_rx == -1) {
        perror("[ERR]: open failed");
        return EXIT_FAILURE;
        //exit(EXIT_FAILURE);

    }

    pthread_t host_tid;
    host_thread_arg_t *arg = malloc(sizeof(host_thread_arg_t));
    arg->rx = &reg_rx;
    strcpy(arg->directory_name, argv[1]);
    pthread_create(&host_tid, NULL, host_thread, (void*) arg);
    pthread_join(host_tid, NULL);
    close_debug_file();
    free(arg);
    return 0;
}
