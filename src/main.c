#include "sql_processor.h"

#include <time.h>
#include <string.h>

/*
 * REPL에서 사용자가 preload 대상을 주지 않았을 때 기본으로 미리 로드할 테이블 이름이다.
 * 현재 프로젝트는 users 테이블 중심으로 동작하므로 users를 기본값으로 둔다.
 */
#define DEFAULT_REPL_TABLE "users"

/*
 * 각 SQL 실행 시간을 누적 기록할 로그 파일 경로다.
 * - mode: FILE / REPL
 * - path: indexed / full_scan / insert
 * - time_ms: 실행 시간
 * - sql: 원본 SQL 일부
 */
#define QUERY_TIMING_LOG_PATH "logs/query_timing.log"

/* 로그 한 줄에 남길 SQL 문자열 최대 길이다. 너무 긴 SQL은 뒤를 ...으로 자른다. */
#define MAX_LOG_SQL_TEXT 160

/*
 * 현재 시간을 밀리초 단위 실수값으로 반환한다.
 *
 * 어디에 쓰나:
 * - SQL 한 문장의 전체 실행 시간을 재기 위해 시작/끝 시각을 기록한다.
 */
static double now_ms(void) {
    struct timespec ts; /* timespec_get이 채워줄 현재 시각 구조체 */

    timespec_get(&ts, TIME_UTC);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1000000.0;
}

/*
 * 로그 파일에 남기기 좋은 형태로 SQL 문자열을 정리한다.
 *
 * 동작:
 * - 줄바꿈, 탭을 공백으로 바꾼다.
 * - 뒤쪽 공백을 제거한다.
 * - 너무 길면 ...으로 잘라낸다.
 *
 * 입력:
 * - sql_text: 원본 SQL
 * - buffer: 정리된 SQL을 써 넣을 출력 버퍼
 * - buffer_size: buffer 크기
 */
static void format_sql_for_log(const char *sql_text, char *buffer, size_t buffer_size) {
    size_t i;      /* 원본 SQL을 읽는 인덱스 */
    size_t j = 0;  /* 출력 버퍼에 쓰는 인덱스 */

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

/*
 * SQL 한 문장의 실행 시간을 로그 파일에 기록한다.
 *
 * 입력:
 * - mode: "FILE" 또는 "REPL"
 * - sql_text: 실행한 SQL 원문
 * - elapsed_ms: 걸린 시간
 * - path: 실제 실행 경로(insert / indexed / full_scan)
 */
static void append_query_timing_log(const char *mode, const char *sql_text, double elapsed_ms, ExecutionPath path) {
    Status status;                      /* 디렉터리 생성 실패 등 메시지 전달용 */
    FILE *file;                         /* timing log 파일 핸들 */
    time_t now;                         /* 현재 시각의 epoch time */
    struct tm *local_time;              /* 사람이 읽기 쉬운 로컬 시각 */
    char timestamp[32];                 /* yyyy-mm-dd hh:mm:ss 문자열 */
    char formatted_sql[MAX_LOG_SQL_TEXT]; /* 로그에 남길 축약 SQL */

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

/*
 * SQL 한 문장을 end-to-end로 실행한다.
 *
 * 단계:
 * 1. lex_sql
 * 2. parse_statement
 * 3. execute_statement
 * 4. 실행 시간 로그 기록
 *
 * 입력:
 * - sql_text: 실행할 SQL 한 문장
 * - mode: "FILE" 또는 "REPL"
 */
static int run_sql(const char *sql_text, const char *mode) {
    Status status;      /* lexer/parser/executor 오류 메시지 */
    TokenArray tokens;  /* lexer 결과 */
    ASTNode *root;      /* parser가 만든 AST 루트 */
    double started_at;  /* 실행 시작 시각 */
    double ended_at;    /* 실행 종료 시각 */
    int ok = 0;         /* 최종 성공 여부 */

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

/*
 * SQL 한 문장을 실제 실행하기 전에, 해당 문장이 사용할 테이블의 실행 컨텍스트를
 * 미리 준비한다.
 *
 * 어디에 쓰나:
 * - SQL 파일 실행 시 첫 문장을 읽자마자 id 인덱스를 미리 메모리에 올려두고 싶을 때
 */
static void preload_sql_context(const char *sql_text) {
    Status status;      /* preload 중 오류 메시지 */
    TokenArray tokens;  /* preload용 토큰 배열 */
    ASTNode *root;      /* preload용 AST 루트 */

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

/*
 * schema.table 이름을 직접 받아 해당 테이블의 실행 컨텍스트를 미리 준비한다.
 *
 * 어디에 쓰나:
 * - REPL 시작 전에 users 테이블의 id 인덱스를 먼저 메모리에 올려두기
 */
static void preload_table_context(const char *schema_name, const char *table_name) {
    Status status; /* preload 실패 메시지 */

    if (!prepare_execution_context_for_table(schema_name, table_name, &status)) {
        fprintf(stderr, "%s\n", status.message);
    }
}

/*
 * [schema.]table 형식의 문자열 인자를 파싱한다.
 *
 * 예:
 * - "users"        -> schema=school, table=users
 * - "verify.users" -> schema=verify, table=users
 *
 * 반환:
 * - 성공: 1
 * - 실패: 0
 */
static int parse_table_argument(const char *arg, char *schema_name, size_t schema_size, char *table_name, size_t table_size) {
    const char *dot;        /* schema.table 의 점 위치 */
    size_t schema_length;   /* 점 앞 schema 길이 */

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

/*
 * SQL 파일을 줄 단위로 읽어 여러 문장을 순서대로 실행한다.
 *
 * 현재 제약:
 * - 한 줄에 한 SQL 문장이라는 가정으로 동작한다.
 *
 * 추가 동작:
 * - 첫 유효 SQL을 먼저 한 번 파싱해 해당 테이블의 id 인덱스를 미리 로드한다.
 */
static int run_sql_file(const char *path) {
    FILE *file;             /* SQL 파일 핸들 */
    char sql_text[MAX_SQL_TEXT]; /* 한 줄씩 읽을 SQL 버퍼 */
    int all_ok = 1;         /* 파일 전체 성공 여부 */

    file = fopen(path, "rb");
    if (file == NULL) {
        fprintf(stderr, "Execution error: cannot open SQL file '%s'\n", path);
        return 0;
    }

    while (fgets(sql_text, sizeof(sql_text), file) != NULL) {
        char *trimmed; /* 앞뒤 공백 제거한 SQL 시작 위치 */

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

/*
 * REPL 모드에서 사용자가 입력한 SQL을 반복 실행한다.
 *
 * 입력:
 * - preload_target: 시작 전에 미리 인덱스를 로드할 [schema.]table 이름
 *   NULL이면 DEFAULT_REPL_TABLE(users)를 사용한다.
 */
static int run_repl(const char *preload_target) {
    char input[MAX_SQL_TEXT];            /* 사용자 입력 버퍼 */
    char schema_name[MAX_NAME_LEN];      /* preload 대상 schema 이름 */
    char table_name[MAX_NAME_LEN];       /* preload 대상 table 이름 */

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

/*
 * 프로그램 진입점이다.
 *
 * 지원 모드:
 * - sql_processor <sql-file>
 * - sql_processor --repl [schema.table|table]
 */
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
