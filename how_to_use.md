- docker로 실행하여 환경 구축(.devcontainer/Dockerfile)

# 빌드
- gcc -std=c11 -Wall -Wextra -pedantic -Isrc -o /tmp/sql_processor_bptree \
  src/main.c src/lexer.c src/parser.c src/meta.c src/storage.c \
  src/executor.c src/util.c src/bptree.c

# CLI 실행
- sql_processor_bptree --repl users

# 명령어 시간
- query_timing.log에 존재