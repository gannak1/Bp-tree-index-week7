#include "sql_processor.h"

#include <string.h>

/* SQL 파일 전체를 호출자가 준비한 버퍼로 읽어온다. */
int read_file_text(const char *path, char *buffer, size_t buffer_size, Status *status) {
    FILE *file;
    size_t read_bytes;

    file = fopen(path, "rb");
    if (file == NULL) {
        snprintf(status->message, sizeof(status->message), "Execution error: cannot open SQL file '%s'", path);
        return 0;
    }

    read_bytes = fread(buffer, 1, buffer_size - 1, file);
    if (ferror(file)) {
        fclose(file);
        snprintf(status->message, sizeof(status->message), "Execution error: failed to read SQL file '%s'", path);
        return 0;
    }

    buffer[read_bytes] = '\0';
    fclose(file);
    return 1;
}

/* lexer -> parser(AST 생성) -> executor 순서로 SQL 한 문장을 실행한다. */
static int run_sql(const char *sql_text) {
    Status status;
    TokenArray tokens;
    ASTNode *root;

    root = NULL;
    tokens = lex_sql(sql_text, &status);
    if (!status.ok) {
        fprintf(stderr, "%s\n", status.message);
        return 0;
    }

    if (!parse_statement(&tokens, &root, &status)) {
        fprintf(stderr, "%s\n", status.message);
        free_tokens(&tokens);
        return 0;
    }

    if (!execute_statement(root, &status)) {
        fprintf(stderr, "%s\n", status.message);
        free_ast(root);
        free_tokens(&tokens);
        return 0;
    }

    free_ast(root);
    free_tokens(&tokens);
    return 1;
}

/* REPL 모드에서 한 줄씩 SQL을 입력받아 반복 실행한다. */
static int run_repl(void) {
    char input[MAX_SQL_TEXT];

    printf("SQL Processor REPL. Type 'exit' or 'quit' to finish.\n");
    while (1) {
        printf("sql> ");
        if (fgets(input, sizeof(input), stdin) == NULL) {
            break;
        }

        input[strcspn(input, "\r\n")] = '\0';
        if (equals_ignore_case(input, "exit") || equals_ignore_case(input, "quit")) {
            break;
        }
        if (trim_whitespace(input)[0] == '\0') {
            continue;
        }

        run_sql(input);
    }

    return 0;
}

/* 프로그램 진입점이다. 파일 실행 모드와 REPL 모드를 지원한다. */
int main(int argc, char **argv) {
    char sql_text[MAX_SQL_TEXT];
    Status status;

    if (argc != 2) {
        fprintf(stderr, "Usage: sql_processor <sql-file> | sql_processor --repl\n");
        return 1;
    }

    if (strcmp(argv[1], "--repl") == 0) {
        return run_repl();
    }

    if (!read_file_text(argv[1], sql_text, sizeof(sql_text), &status)) {
        fprintf(stderr, "%s\n", status.message);
        return 1;
    }

    return run_sql(sql_text) ? 0 : 1;
}
