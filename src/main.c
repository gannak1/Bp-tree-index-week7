#include "sql_processor.h"

#include <string.h>

#define DEFAULT_REPL_TABLE "users"

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

/* SQL 한 문장을 실행하기 전에 대상 테이블의 id 인덱스를 미리 메모리에 올린다. */
static void preload_sql_context(const char *sql_text) {
    Status status;
    TokenArray tokens;
    ASTNode *root;

    root = NULL;
    tokens = lex_sql(sql_text, &status);
    if (!status.ok) {
        return;
    }

    if (!parse_statement(&tokens, &root, &status)) {
        free_tokens(&tokens);
        return;
    }

    prepare_execution_context(root, &status);
    free_ast(root);
    free_tokens(&tokens);
}

/* REPL 시작 전에 지정한 schema.table의 id 인덱스를 미리 메모리에 올린다. */
static void preload_table_context(const char *schema_name, const char *table_name) {
    Status status;

    if (!prepare_execution_context_for_table(schema_name, table_name, &status)) {
        fprintf(stderr, "%s\n", status.message);
    }
}

/* [schema.]table 형식 인자를 파싱한다. schema가 없으면 기본 schema를 사용한다. */
static int parse_table_argument(const char *arg, char *schema_name, size_t schema_size, char *table_name, size_t table_size) {
    const char *dot;
    size_t schema_length;

    if (arg == NULL || arg[0] == '\0') {
        return 0;
    }

    dot = strchr(arg, '.');
    if (dot == NULL) {
        snprintf(schema_name, schema_size, "%s", DEFAULT_SCHEMA_NAME);
        snprintf(table_name, table_size, "%s", arg);
        return 1;
    }

    schema_length = (size_t)(dot - arg);
    if (schema_length == 0 || dot[1] == '\0' || strchr(dot + 1, '.') != NULL) {
        return 0;
    }

    snprintf(schema_name, schema_size, "%.*s", (int)schema_length, arg);
    snprintf(table_name, table_size, "%s", dot + 1);
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

        preload_sql_context(trimmed);
        break;
    }

    rewind(file);

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
static int run_repl(const char *preload_target) {
    char input[MAX_SQL_TEXT];
    char schema_name[MAX_NAME_LEN];
    char table_name[MAX_NAME_LEN];

    if (preload_target == NULL) {
        preload_target = DEFAULT_REPL_TABLE;
    }
    if (parse_table_argument(preload_target, schema_name, sizeof(schema_name), table_name, sizeof(table_name))) {
        preload_table_context(schema_name, table_name);
    } else {
        fprintf(stderr, "Execution error: invalid preload table '%s'\n", preload_target);
    }

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
    if (argc < 2 || argc > 3) {
        fprintf(stderr, "Usage: sql_processor <sql-file> | sql_processor --repl [schema.table|table]\n");
        return 1;
    }

    if (strcmp(argv[1], "--repl") == 0) {
        return run_repl(argc == 3 ? argv[2] : NULL);
    }

    if (argc != 2) {
        fprintf(stderr, "Usage: sql_processor <sql-file> | sql_processor --repl [schema.table|table]\n");
        return 1;
    }

    return run_sql_file(argv[1]) ? 0 : 1;
}
