# Compiler variables
CC = gcc
CFLAGS = -g -Wall -Wextra -std=c17 -D_POSIX_C_SOURCE=200809L -pthread
LDFLAGS = -lncurses -pthread

#CHANGE: Adicionar flags para Thread Sanitizer
SANITIZE_FLAGS = -fno-omit-frame-pointer -O1

# Directory variables
OBJ_DIR = obj
BIN_DIR = bin
INCLUDE_DIR = include
CLIENT_DIR = src/client

# executable 
#client
CLIENT = client


#Client objects
OBJS_CLIENT = client_main.o debug.o api.o display.o

#Server
SERVER_DIR = src/server

SERVER = Pacmanist
OBJS_SERVER = server_main.o game.o board.o parser.o

# Dependencies
display.o = display.h
board.o = board.h
parser.o = parser.h
api.o = api.h protocol.h


# Object files path
vpath %.c $(CLIENT_DIR) $(SERVER_DIR) $(INCLUDE_DIR)

# Make targets
all: client Pacmanist

#CHANGE: Adicionar target com sanitizer
sanitize: CFLAGS += $(SANITIZE_FLAGS)
sanitize: LDFLAGS += $(SANITIZE_FLAGS)
sanitize: clean all

client: $(BIN_DIR)/$(CLIENT)
Pacmanist: $(BIN_DIR)/$(SERVER)

$(BIN_DIR)/$(CLIENT): $(OBJS_CLIENT) | folders
	$(CC) $(CFLAGS) $(addprefix $(OBJ_DIR)/,$(OBJS_CLIENT)) -o $@ $(LDFLAGS)

$(BIN_DIR)/$(SERVER): $(OBJS_SERVER) | folders
	$(CC) $(CFLAGS) $(addprefix $(OBJ_DIR)/,$(OBJS_SERVER)) -o $@ $(LDFLAGS)

# dont include LDFLAGS in the end, to allow compilation on macos
%.o: %.c | folders
	$(CC) -I $(INCLUDE_DIR) $(CFLAGS) -c $< -o $(OBJ_DIR)/$@


# Create folders
folders:
	mkdir -p $(OBJ_DIR)
	mkdir -p $(BIN_DIR)

# Clean object files and executable
clean:
	rm -f $(OBJ_DIR)/*.o
	rm -f $(BIN_DIR)/$(CLIENT)
	rm -f $(BIN_DIR)/$(SERVER)

#CHANGE: Adicionar help
help:
	@echo "Targets:"
	@echo "  make all       - Compile normal"
	@echo "  make sanitize  - Compile com Thread Sanitizer"
	@echo "  make clean     - Limpar ficheiros compilados"

# indentify targets that do not create files
.PHONY: all clean run folders sanitize help