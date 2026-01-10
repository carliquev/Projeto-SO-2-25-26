# ========== Compiler ==========
CC      := gcc
CFLAGS  := -g -Wall -Wextra -Werror -std=c17 -D_POSIX_C_SOURCE=200809L
LDLIBS  := -lncurses -pthread

# ========== Directories ==========
SRC_DIR     := src
CLIENT_DIR  := src/client
INCLUDE_DIR := include
OBJ_DIR     := obj
BIN_DIR     := bin

# ========== Executables ==========
PACMANIST := PacmanIST
CLIENT   := client

# ========== Object lists ==========
PACMANIST_OBJS := \
	$(OBJ_DIR)/server/game.o \
	$(OBJ_DIR)/server/parser.o \
	$(OBJ_DIR)/server/board.o

CLIENT_OBJS := \
	$(OBJ_DIR)/client/client_main.o \
	$(OBJ_DIR)/client/debug.o \
	$(OBJ_DIR)/client/api.o \
	$(OBJ_DIR)/client/display.o

# ========== Default target ==========
all: $(BIN_DIR)/$(PACMANIST) $(BIN_DIR)/$(CLIENT)

# ========== Link ==========
$(BIN_DIR)/$(PACMANIST): $(PACMANIST_OBJS) | $(BIN_DIR)
	$(CC) $(CFLAGS) $^ -o $@ $(LDLIBS)

$(BIN_DIR)/$(CLIENT): $(CLIENT_OBJS) | $(BIN_DIR)
	$(CC) $(CFLAGS) $^ -o $@ $(LDLIBS)

# ========== Compile rules ==========
$(OBJ_DIR)/server/%.o: $(SRC_DIR)/%.c | $(OBJ_DIR)/server
	$(CC) -I$(INCLUDE_DIR) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/client/%.o: $(CLIENT_DIR)/%.c | $(OBJ_DIR)/client
	$(CC) -I$(INCLUDE_DIR) $(CFLAGS) -c $< -o $@

# ========== Folders ==========
$(BIN_DIR):
	mkdir -p $@

$(OBJ_DIR)/server:
	mkdir -p $@

$(OBJ_DIR)/client:
	mkdir -p $@

# ========== Convenience ==========
pacmanist: $(BIN_DIR)/$(PACMANIST)
client: $(BIN_DIR)/$(CLIENT)

run-pacmanist: pacmanist
	./$(BIN_DIR)/$(PACMANIST) levels 1 reg_fifo

run-client: client
	./$(BIN_DIR)/$(CLIENT) 1 reg_fifo

# ========== Clean ==========
clean:
	rm -rf $(OBJ_DIR) $(BIN_DIR) *.log *.fifo

.PHONY: all clean pacmanist client run-pacmanist run-client
