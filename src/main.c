#include "sql_processor.h"

#include <time.h>
#include <string.h>

#define DEFAULT_REPL_TABLE "users"
#define QUERY_TIMING_LOG_PATH "logs/query_timing.log"
#define MAX_LOG_SQL_TEXT 160

/* 단조 증가에 가까운 현재 시각을 밀리초 단위로 가져온다. */
static double now_ms(void) {
    struct timespec ts;

    timespec_get(&ts, TIME_UTC);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1000000.0;
}

/* 로그 파일에 남길 SQL을 한 줄로 정리하고 너무 길면 잘라낸다. */
static void format_sql_for_log(const char *sql_text, char *buffer, size_t buffer_size) {
    size_t i;
    size_t j = 0;

    if (buffer_size == 0) {
        return;
    }

    for (i = 0; sql_text[i] != '\0' && j + 1 < buffer_size; i++) {
        char ch = sql_text[i];
        if (ch == '\r' || ch == '\n' || ch == '\t') {
            ch = ' ';
        }
        buffer[j++] = ch;
    }
    buffer[j] = '\0';

    while (j > 0 && buffer[j - 1] == ' ') {
        buffer[--j] = '\0';
    }
    if (sql_text[i] != '\0' && buffer_size > 4) {
        buffer[buffer_size - 4] = '.';
        buffer[buffer_size - 3] = '.';
        buffer[buffer_size - 2] = '.';
        buffer[buffer_size - 1] = '\0';
    }
}

/* SQL 한 문장 실행 시간을 별도 로그 파일에 누적 기록한다. */
static void append_query_timing_log(const char *mode, const char *sql_text, double elapsed_ms, ExecutionPath path) {
    Status status;
    FILE *file;
    time_t now;
    struct tm *local_time;
    char timestamp[32];
    char formatted_sql[MAX_LOG_SQL_TEXT];

    if (!ensure_parent_directory(QUERY_TIMING_LOG_PATH, &status)) {
        fprintf(stderr, "%s\n", status.message);
        return;
    }

    file = fopen(QUERY_TIMING_LOG_PATH, "a");
    if (file == NULL) {
        fprintf(stderr, "Execution error: cannot open log file '%s'\n", QUERY_TIMING_LOG_PATH);
        return;
    }

    now = time(NULL);
    local_time = localtime(&now);
    if (local_time == NULL) {
        snprintf(timestamp, sizeof(timestamp), "unknown-time");
    } else {
        strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", local_time);
    }

    format_sql_for_log(sql_text, formatted_sql, sizeof(formatted_sql));
    fprintf(
        file,
        "%s | mode=%s | path=%s | time_ms=%.3f | sql=%s\n",
        timestamp,
        mode,
        execution_path_to_text(path),
        elapsed_ms,
        formatted_sql
    );
    fclose(file);
}

/* lexer -> parser(AST 생성) -> executor 순서로 SQL 한 문장을 실행한다. */
static int run_sql(const char *sql_text, const char *mode) {
    Status status;
    TokenArray tokens;
    ASTNode *root;
    double started_at;
    double ended_at;
    int ok = 0;

    root = NULL;
    started_at = now_ms();
    tokens = lex_sql(sql_text, &status);
    if (!status.ok) {
        fprintf(stderr, "%s\n", status.message);
        append_query_timing_log(mode, sql_text, now_ms() - started_at, EXECUTION_PATH_UNKNOWN);
        return 0;
    }

    if (!parse_statement(&tokens, &root, &status)) {
        fprintf(stderr, "%s\n", status.message);
        free_tokens(&tokens);
        append_query_timing_log(mode, sql_text, now_ms() - started_at, EXECUTION_PATH_UNKNOWN);
        return 0;
    }

    if (!execute_statement(root, &status)) {
        fprintf(stderr, "%s\n", status.message);
        free_ast(root);
        free_tokens(&tokens);
        append_query_timing_log(mode, sql_text, now_ms() - started_at, get_last_execution_path());
        return 0;
    }

    free_ast(root);
    free_tokens(&tokens);
    ok = 1;
    ended_at = now_ms();
    append_query_timing_log(mode, sql_text, ended_at - started_at, get_last_execution_path());
    return ok;
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

        if (!run_sql(trimmed, "FILE")) {
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

        run_sql(input, "REPL");
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
