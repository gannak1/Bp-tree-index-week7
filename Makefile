CC=gcc
CFLAGS=-std=c11 -Wall -Wextra -pedantic -Isrc
TARGET=sql_processor

SRCS=src/main.c src/lexer.c src/parser.c src/meta.c src/storage.c src/executor.c src/util.c
OBJS=$(SRCS:.c=.o)

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS)

clean:
	del /Q src\*.o $(TARGET).exe 2>nul || exit 0
