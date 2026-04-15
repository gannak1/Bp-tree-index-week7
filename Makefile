CC ?= cc
CFLAGS ?= -std=c11 -Wall -Wextra -Wpedantic -O2
LDFLAGS ?=

BIN := mysql_bptree
IMAGE := mysql-bptree
SRC := src/main.c src/engine.c src/executor.c src/query.c src/storage.c src/bptree.c src/util.c src/ast.c

.PHONY: all clean run test data-dir docker-build docker-run docker-test

all: $(BIN)

$(BIN): $(SRC)
	$(CC) $(CFLAGS) -o $@ $(SRC) $(LDFLAGS)

run: $(BIN)
	./$(BIN)

test: $(BIN)
	./$(BIN) --self-test

data-dir:
	mkdir -p data

docker-build:
	docker build -t $(IMAGE) .

docker-run: data-dir
	docker run --rm -it -v "$(CURDIR)/data:/app/data" $(IMAGE)

docker-test: docker-build data-dir
	docker run --rm -v "$(CURDIR)/data:/app/data" $(IMAGE) ./$(BIN) --self-test

clean:
	rm -f $(BIN)
