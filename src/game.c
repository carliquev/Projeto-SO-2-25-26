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
#include <bits/posix2_lim.h>


#define CONTINUE_PLAY 0
#define NEXT_LEVEL 1
#define QUIT_GAME 2
#define LOAD_BACKUP 3
#define CREATE_BACKUP 4

#define DEFAULT 0
#define VICTORY 1
#define GAMEOVER 2
#define ENDGAME 3

#define MAX_SESSIONS 100
#define TOP_SESSIONS 5


#define NOTIF_SIZE 8

typedef struct {
    int notif_tx;
    int req_rx;
    int thread_shutdown;
    int error;
    pthread_mutex_t lock;
} session_t;

typedef struct {
    session_t *session;
    int id;
    int *points;
    bool active;
} session_state_t;

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

// typedef struct {
//     int *rx;
//     char directory_name[MAX_FILENAME];
//     char reg_pipe_path[MAX_PIPE_PATH_LENGTH];
// } host_thread_arg_t;


typedef struct {
    char directory_name[MAX_FILENAME];
    int thread_id;
} session_thread_arg_t;

typedef struct registration_node {
    int req_rx;
    int notif_tx;
    int client_id;
    struct registration_node *next;
} registration_node_t;

registration_node_t *queue_head = NULL;
registration_node_t *queue_tail = NULL;
pthread_mutex_t queue_lock = PTHREAD_MUTEX_INITIALIZER;

session_state_t** sessions;
int max_sessions = 0;
int active = 0;
static volatile sig_atomic_t sigusr1_received = 0;
pthread_mutex_t sessions_lock = PTHREAD_MUTEX_INITIALIZER;


sem_t semaforo_clientes;
pthread_mutex_t mutex_clientes;

void enqueue_registration(int req_rx, int notif_tx, int client_id) {
    registration_node_t *new_node = malloc(sizeof(registration_node_t));
    if (new_node == NULL) {
        fprintf(stderr, "[ERR]: malloc failed for registration node\n");
        return;
    }

    new_node->req_rx = req_rx;
    new_node->notif_tx = notif_tx;
    new_node->client_id = client_id;
    new_node->next = NULL;

    pthread_mutex_lock(&queue_lock);

    if (queue_tail == NULL) {
        // Fila vazia
        queue_head = new_node;
        queue_tail = new_node;
    } else {
        // Adiciona ao fim
        queue_tail->next = new_node;
        queue_tail = new_node;
    }

    pthread_mutex_unlock(&queue_lock);
}

int dequeue_registration(int *req_rx, int *notif_tx, int *client_id) {
    pthread_mutex_lock(&queue_lock);

    if (queue_head == NULL) {
        // Fila vazia
        pthread_mutex_unlock(&queue_lock);
        return -1;
    }

    registration_node_t *node = queue_head;
    *req_rx = node->req_rx;
    *notif_tx = node->notif_tx;
    *client_id = node->client_id;

    queue_head = node->next;

    if (queue_head == NULL) {
        // Era o último nó
        queue_tail = NULL;
    }

    pthread_mutex_unlock(&queue_lock);

    free(node);
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
        if (ret == -1 || msg_play.op_code == OP_CODE_DISCONNECT) {
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
    //inibição de SIGUSR1 nas sessions
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGUSR1);
    pthread_sigmask(SIG_BLOCK, &mask, NULL);

    session_thread_arg_t *session_arg = (session_thread_arg_t*) arg;
    char directory_name[MAX_FILENAME];
    strcpy(directory_name, session_arg->directory_name);

    //free(session_arg);
    // Random seed for any random movements
    srand((unsigned int)time(NULL));

    while (true) {
        bool next_client = false;
        sem_wait(&semaforo_clientes);
        int req_rx, notif_tx, client_id;
        int result = dequeue_registration(&req_rx, &notif_tx, &client_id);
        if (result == -1) {
            sem_post(&semaforo_clientes);
            sleep_ms(100);
            continue;
        }

        msg_reg_response_t response;
        response.op_code = OP_CODE_CONNECT;
        response.result = result;

        int response_write = write_msg(notif_tx, &response, sizeof(msg_reg_response_t));
        if (response_write < 0) {
            perror("[ERR]: write failed");
            close(req_rx);
            close(notif_tx);
            result = 1;
        }
        if (result == 1) {
            continue;
        }

        session_t *session = malloc(sizeof(session_t));
        pthread_mutex_init(&session->lock, NULL);
        session->req_rx = req_rx;
        session->notif_tx = notif_tx;
        session->thread_shutdown = 0;
        session->error = 0;

        session_state_t *session_state = malloc(sizeof(session_state_t));
        session_state->id = client_id;
        session_state->points = malloc(sizeof(int));
        *session_state->points = 0;
        session_state->active = true;
        session_state->session = session;

        //Encontra slot livre no array sessions
        pthread_mutex_lock(&sessions_lock);
        int slot = -1;
        for (int i = 0; i < max_sessions; i++) {
            if (sessions[i] == NULL || !sessions[i]->active) {
                if (sessions[i] != NULL) {
                    free(sessions[i]);
                }
                sessions[i] = session_state;
                slot = i;
                break;
            }
        }
        ++active;
        pthread_mutex_unlock(&sessions_lock);
        if (slot == -1) {
            fprintf(stderr, "[ERR]: No slot available\n");
            free(session_state);
            pthread_mutex_destroy(&session->lock);
            free(session);
            sem_post(&semaforo_clientes);
            exit(EXIT_FAILURE);
        }

        DIR* level_dir = opendir(directory_name);

        if (level_dir == NULL) {
            fprintf(stderr, "Failed to open directory\n");
            pthread_mutex_lock(&session->lock);
            session->error = 1;
            pthread_mutex_unlock(&session->lock);
            pthread_mutex_destroy(&session->lock);
            pthread_mutex_lock(&sessions_lock);
            --active;
            session_state->active = false;
            pthread_mutex_unlock(&sessions_lock);
            free(session);
            sem_post(&semaforo_clientes);
            continue;
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
                session_state->points = &(game_board.pacmans[0].points);

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

                    pthread_mutex_lock(&session->lock);
                    int* err = &session->error;
                    pthread_mutex_unlock(&session->lock);
                    if (*err==1) {
                        free(ghost_tids);
                        next_client = true;
                        break;
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
                        accumulated_points = game_board.pacmans[0].points;
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

                }

                // print_board(&game_board);
                // if (game_board.pacmans[0].alive) {
                //     update_client(session, &game_board, VICTORY);
                // }

                unload_level(&game_board);
                if (next_client==true) {
                    break;
                }
            }
        }
        if (next_client==true) {
            pthread_mutex_lock(&sessions_lock);
            --active;
            session_state->active = false;
            pthread_mutex_unlock(&sessions_lock);
            closedir(level_dir);
            close(req_rx);
            close(notif_tx);
            pthread_mutex_destroy(&session->lock);
            free(session);
            sem_post(&semaforo_clientes);
            continue;
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
        closedir(level_dir);

        pthread_mutex_destroy(&session->lock);
        pthread_mutex_lock(&sessions_lock);
        --active;
        session_state->active = false;
        pthread_mutex_unlock(&sessions_lock);
        free(session);
        sem_post(&semaforo_clientes);
    }
    pthread_exit(NULL);
}

int compare_sessions(const void *a, const void *b) {
    session_state_t *session_a = *(session_state_t**)a;
    session_state_t *session_b = *(session_state_t**)b;

    if (session_b->active && (!session_a->active)) return 1;
    if (session_a->active && (!session_b->active)) return -1;

    int* b_points_ptr = session_b->points;
    int* a_points_ptr = session_a->points;

    if (*b_points_ptr > *a_points_ptr) return 1;
    if (*a_points_ptr > *b_points_ptr) return -1;

    //se pontos iguais, ordenar por id
    return session_a->id - session_b->id;
}

void top5_generator() {
    // fprintf(stderr, "Caught SIGSUR1\n");
    pthread_mutex_lock(&sessions_lock);

    session_state_t* sessions_copy[max_sessions];
    int count = 0;
    for (int i = 0; i < max_sessions; i++) {
        if (sessions[i] != NULL) {
            sessions_copy[count++] = sessions[i];
        }
    }

    int active_sessions = active;
    pthread_mutex_unlock(&sessions_lock);

    qsort(sessions_copy, count, sizeof(session_state_t*), compare_sessions);

    int fd = open("topPlayers.txt", O_CREAT|O_TRUNC|O_WRONLY, 0644);
    int top_n;
    if (active_sessions < TOP_SESSIONS) {
        top_n = active_sessions;
    } else top_n = TOP_SESSIONS;

    for (int i = 0; i < top_n; i++) {
        session_state_t* session_state = sessions_copy[i];
        int * points_ptr = session_state->points;
        char line[LINE_MAX];
        snprintf(line, LINE_MAX ,"ID: %d, Pontos: %d\n",
                session_state->id, *points_ptr);
        write(fd, line, strlen(line));
    }
    close(fd);
}

void sig_handler(int sig) {
    if (sig == SIGUSR1) {
        sigusr1_received = 1;
    }
}

void hosting(int reg_rx,char *reg_pipe_pathname) {
    while (true) {
        int result = 0;
        if (sigusr1_received) {
            sigusr1_received = 0;
            top5_generator();
        }
        msg_registration_t msg_reg;
        ssize_t ret = read(reg_rx, &msg_reg, sizeof(msg_registration_t));
        if (ret == 0) {
            // All writers disconnected → FIFO reached EOF
            close(reg_rx);
            // Reopen FIFO (still read-only)
            do {
                reg_rx = open(reg_pipe_pathname, O_RDONLY | O_NONBLOCK);
            } while (reg_rx == -1 && errno == EINTR);

            continue;
        } if (ret == -1) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // Sem dados disponíveis, espera um pouco
                sleep_ms(100);
                continue;
            }
            fprintf(stderr, "[ERR]: read failed: %s\n", strerror(errno));
            // continue
            result = 1;
        }

        bool valid = true;
        int req_rx = 0;
        while (true) {
            req_rx = open(msg_reg.req_pipe_path, O_RDONLY | O_NONBLOCK);
            if (req_rx == -1 && errno == ENXIO) {
                sleep_ms(100);
            }
            else if (req_rx == -1) {
                perror("[ERR]: req_pipe open failed");
                valid = false;
                break;
            } else {
                break;
            }

        }
        if (valid == false) result = 1;
        // Remove O_NONBLOCK depois de abrir para I/O ser bloqueante normal
        int flags = fcntl(req_rx, F_GETFL, 0);
        fcntl(req_rx, F_SETFL, flags & ~O_NONBLOCK);

        valid = true;
        int notif_tx = 0;
        while (true) {
            notif_tx = open(msg_reg.notif_pipe_path, O_WRONLY | O_NONBLOCK);
            if (notif_tx == -1 && errno == ENXIO) {
                sleep_ms(100);
            }
            else if (notif_tx == -1) {
                perror("[ERR]: req_pipe open failed");
                valid = false;
                break;
            } else {
                break;
            }

        }
        if (valid == false) result = 1;
        // Remove O_NONBLOCK depois de abrir para I/O ser bloqueante normal
        flags = fcntl(notif_tx, F_GETFL, 0);
        fcntl(notif_tx, F_SETFL, flags & ~O_NONBLOCK);

        if (result==1) continue;

        char client_id_char[MAX_PIPE_PATH_LENGTH];
        int parsed = sscanf(msg_reg.req_pipe_path, "/tmp/%s_request", client_id_char);
        if (parsed != 1) {
            perror("[ERR]: req_pipe parse failed");
            pthread_exit(NULL);
        }

        int client_id = atoi(client_id_char);


        enqueue_registration(req_rx, notif_tx, client_id);

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
    struct sigaction sa;
    sa.sa_handler = sig_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    if (sigaction(SIGUSR1, &sa, NULL) == -1) {
        perror("sigaction failed");
        return EXIT_FAILURE;
    }

    int max_games = atoi(argv[2]);
    max_sessions = max_games;
    sessions = malloc(max_games * sizeof(session_state_t*));
    for (int i = 0; i < max_games; i++) {
        sessions[i] = NULL;
    }
    // Inicializa semaforo
    sem_init(&semaforo_clientes, 0, max_games);
    open_debug_file("debug.log");

    char* reg_pipe_pathname = argv[3];

    /* remove pipe if it exists */
    if (unlink(reg_pipe_pathname) != 0 && errno != ENOENT) {
        perror("[ERR]: unlink(%s) failed");
        return EXIT_FAILURE;
        //exit(EXIT_FAILURE);

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
    //torna read non-blocking
    int flags = fcntl(reg_rx, F_GETFL, 0);
    fcntl(reg_rx, F_SETFL, flags | O_NONBLOCK);

    pthread_t *session_tids = malloc(max_games * sizeof(pthread_t));
    for (int i = 0; i < max_games; i++) {
        session_thread_arg_t *s_arg = malloc(sizeof(session_thread_arg_t));
        strcpy(s_arg->directory_name, argv[1]);
        s_arg->thread_id = i;
        pthread_create(&session_tids[i], NULL, session_thread, (void*) s_arg);
    }

    hosting(reg_rx, argv[3]);

    for (int i = 0; i < max_games; i++) {
        pthread_cancel(session_tids[i]);
    }
    free(session_tids);
    free(sessions);

    close_debug_file();
    return 0;
}
