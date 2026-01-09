#include "api.h"
#include "protocol.h"
#include "display.h"
#include "debug.h"
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <pthread.h>
#include <stdbool.h>
#include <unistd.h>
#include <signal.h>

Board board;
bool stop_execution = false;
int tempo;
pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;

static volatile sig_atomic_t sigint_received = 0;
void client_sig_handler(int sig) {
    if (sig == SIGINT) {
        sigint_received = 1;
    }
}

static void *receiver_thread(void *arg) {
    (void)arg;
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGUSR1);
    sigaddset(&mask, SIGINT);
    pthread_sigmask(SIG_BLOCK, &mask, NULL);

    while (true) {

        if (sigint_received) {
            pthread_exit(NULL);
        }
        Board board = receive_board_update();

        if (board.game_over > 1 || !board.data){
            stop_execution = true;
            break;
        }

        pthread_mutex_lock(&mutex);
        tempo = board.tempo;
        pthread_mutex_unlock(&mutex);

        draw_board_client(board);
        refresh_screen();
        free(board.data);
    }

    free(board.data);
    debug("Returning receiver thread...\n");
    return NULL;
}

int main(int argc, char *argv[]) {
    if (argc != 3 && argc != 4) {
        fprintf(stderr,
            "Usage: %s <client_id> <register_pipe> [commands_file]\n",
            argv[0]);
        return 1;
    }

    struct sigaction sa;
    sa.sa_handler = client_sig_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;  // Sem SA_RESTART para interromper read
    if (sigaction(SIGINT, &sa, NULL) == -1) {
        perror("sigaction SIGINT failed");
        return EXIT_FAILURE;
    }

    const char *client_id = argv[1];
    const char *register_pipe = argv[2];
    const char *commands_file = (argc == 4) ? argv[3] : NULL;


    FILE *cmd_fp = NULL;
    if (commands_file) {
        cmd_fp = fopen(commands_file, "r");
        if (!cmd_fp) {
            perror("Failed to open commands file");
            return 1;
        }
    }

    char req_pipe_path[MAX_PIPE_PATH_LENGTH];
    char notif_pipe_path[MAX_PIPE_PATH_LENGTH];

    snprintf(req_pipe_path, MAX_PIPE_PATH_LENGTH,
             "/tmp/%s_request", client_id);

    snprintf(notif_pipe_path, MAX_PIPE_PATH_LENGTH,
             "/tmp/%s_notification", client_id);

    open_debug_file("client-debug.log");

    if (pacman_connect(req_pipe_path, notif_pipe_path, register_pipe) != 0) {
        perror("Failed to connect to server");
        return 1;
    }

    pthread_t receiver_thread_id;
    pthread_create(&receiver_thread_id, NULL, receiver_thread, NULL);

    terminal_init();
    atexit(terminal_cleanup);
    set_timeout(500);
    draw_board_client(board);
    refresh_screen();

    char command;
    int ch;

    while (1) {
        if (sigint_received) {
            stop_execution = true;
            if (pacman_disconnect() == -1) {
                perror("[ERR]: client disconnect error");
                return EXIT_FAILURE;
            }
        }
        pthread_mutex_lock(&mutex);
        if (stop_execution) {
            pthread_mutex_unlock(&mutex);
            break;
        }
        pthread_mutex_unlock(&mutex);

        if (cmd_fp) {
            // Input from file
            ch = fgetc(cmd_fp);

            if (ch == EOF) {
                // Restart at the start of the file
                rewind(cmd_fp);
                continue;
            }

            command = (char)ch;

            if (command == '\n' || command == '\r' || command == '\0')
                continue;

            command = toupper(command);
            
            // Wait for tempo, to not overflow pipe with requests
            pthread_mutex_lock(&mutex);
            int wait_for = tempo;
            pthread_mutex_unlock(&mutex);

            sleep_ms(wait_for);
            
        } else {
            pthread_mutex_lock(&mutex);
            if (stop_execution) {
                pthread_mutex_unlock(&mutex);
                break;
            }
            pthread_mutex_unlock(&mutex);
            // Interactive input
            command = get_input();
            command = toupper(command);
        }

        if (command == '\0')
            continue;

        if (command == 'Q') {
            if (pacman_disconnect() == -1) {
                perror("[ERR]: client disconnect error");
                return EXIT_FAILURE;
            }
            stop_execution = true;
        }
        debug("Command: %c\n", command);

        pthread_mutex_lock(&mutex);
        if (stop_execution) {
            pthread_mutex_unlock(&mutex);
            break;
        }
        pthread_mutex_unlock(&mutex);

        pacman_play(command);

    }
    pthread_join(receiver_thread_id, NULL);

    if (cmd_fp)
        fclose(cmd_fp);

    pthread_mutex_destroy(&mutex);

    // if (pacman_disconnect() == -1) {
    //     perror("[ERR]: client disconnect error");
    //     return EXIT_FAILURE;
    // }
    terminal_cleanup();

    return EXIT_SUCCESS;
}
