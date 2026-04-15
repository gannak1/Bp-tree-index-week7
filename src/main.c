#include "sql_processor.h"

#include <string.h>

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

/* 파일에서 SQL을 한 줄씩 읽어 여러 문장을 순서대로 실행한다. */
static int run_sql_file(const char *path) {
    FILE *file;
    char sql_text[MAX_SQL_TEXT];
    int all_ok = 1;

    file = fopen(path, "rb");
    if (file == NULL) {
        fprintf(stderr, "Execution error: cannot open SQL file '%s'\n", path);
        return 0;
    }

    while (fgets(sql_text, sizeof(sql_text), file) != NULL) {
        char *trimmed;

        sql_text[strcspn(sql_text, "\r\n")] = '\0';
        trimmed = trim_whitespace(sql_text);
        if (trimmed[0] == '\0') {
            continue;
        }

        if (!run_sql(trimmed)) {
            all_ok = 0;
            break;
        }
    }

    fclose(file);
    return all_ok;
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
    if (argc != 2) {
        fprintf(stderr, "Usage: sql_processor <sql-file> | sql_processor --repl\n");
        return 1;
    }

    if (strcmp(argv[1], "--repl") == 0) {
        return run_repl();
    }

    return run_sql_file(argv[1]) ? 0 : 1;
}
