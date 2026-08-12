CC      = cc
CFLAGS  = -std=c11 -Wall -Wextra -O2 -D_POSIX_C_SOURCE=200809L
LDFLAGS = -lpthread

SRC = src/server.c src/sha1.c src/base64.c
BIN = bin/server

.PHONY: all clean run

all: $(BIN)

$(BIN): $(SRC) src/sha1.h src/base64.h
	@mkdir -p bin
	$(CC) $(CFLAGS) -o $(BIN) $(SRC) $(LDFLAGS)

run: $(BIN)
	./$(BIN)

clean:
	rm -rf bin
