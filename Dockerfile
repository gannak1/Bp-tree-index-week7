FROM ubuntu:24.04 AS builder

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y \
    build-essential \
    gcc \
    make \
    python3 \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /build
COPY . .

RUN gcc -std=c11 -Wall -Wextra -pedantic -Isrc -o /usr/local/bin/sql_processor_bptree \
    src/main.c \
    src/lexer.c \
    src/parser.c \
    src/meta.c \
    src/storage.c \
    src/executor.c \
    src/util.c \
    src/bptree.c

FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive
ENV LANG=C.UTF-8
ENV LC_ALL=C.UTF-8

RUN apt-get update && apt-get install -y \
    python3 \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /workspace

COPY --from=builder /usr/local/bin/sql_processor_bptree /usr/local/bin/sql_processor_bptree
COPY . /workspace/

ENTRYPOINT ["/usr/local/bin/sql_processor_bptree"]
CMD ["--repl"]
