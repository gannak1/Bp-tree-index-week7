#include "engine_internal.h"
#include "engine.h"

/*
 * engine.c
 *
 * 프로그램의 실행 흐름을 담당합니다.
 * DB 파일 로드, REPL 입력 루프, self-test, 종료 시 메모리 정리를 조율합니다.
 * SQL 의미 해석과 실행은 executor.c로 넘깁니다.
 */

static int run_repl(Database *db) {
    /*
     * MySQL처럼 프롬프트를 출력하고 세미콜론이 나올 때까지 입력을 누적합니다.
     * 여러 줄 SQL도 command 버퍼에 이어 붙여 하나의 명령으로 처리합니다.
     */
    char line[SQL_BUF_SIZE];
    char command[SQL_BUF_SIZE];
    command[0] = '\0';
    while (true) {
        printf("mysql-bptree> ");
        fflush(stdout);
        if (!fgets(line, sizeof(line), stdin)) {
            break;
        }
        if (strlen(command) + strlen(line) + 1 >= sizeof(command)) {
            printf("Syntax Error: command too long.\n");
            command[0] = '\0';
            continue;
        }
        strcat(command, line);
        if (strchr(line, ';') == NULL &&
            !starts_with_ci(command, "EXIT") &&
            !starts_with_ci(command, "QUIT")) {
            /* 세미콜론이 없으면 아직 SQL이 끝나지 않은 것으로 보고 다음 줄을 더 읽습니다. */
            continue;
        }
        /* 완성된 SQL 하나를 executor로 전달합니다. */
        ExecStatus status = execute_command(db, command);
        command[0] = '\0';
        if (status == EXEC_EXIT) {
            return 0;
        }
    }
    double begin = now_sec();
    /* EOF로 종료되어도 현재 데이터는 저장합니다. */
    execute_save(db, begin);
    return 0;
}

static int run_self_test(void) {
    /*
     * 회귀 테스트용 self-test.
     * persist=false라 실제 data/ 파일을 건드리지 않고 메모리에서만 기능을 검증합니다.
     */
    Database db;
    db_init(&db);
    db.persist = false;
    char err[512];
    (void)err;
    add_index_meta(&db, "PRIMARY", db.schema.primary_column, true, true, true);
    rebuild_indexes(&db, err, sizeof(err));
    const char *cmds[] = {
        /* 컬럼 순서 변경, 인덱스 사용/무시, 범위 검색까지 주요 기능을 순서대로 확인합니다. */
        "INSERT INTO users (name, age, email) VALUES ('kim', 20, 'kim@test.com');",
        "INSERT INTO users (email, age, name) VALUES ('lee@test.com', 25, 'lee');",
        "SELECT * FROM users FORCE INDEX (PRIMARY) WHERE id = 1;",
        "SELECT id FROM users FORCE INDEX (PRIMARY) WHERE id = 1;",
        "SELECT * FROM users IGNORE INDEX (PRIMARY) WHERE id = 1;",
        "CREATE INDEX idx_users_name_test ON users (name);",
        "SELECT * FROM users FORCE INDEX (idx_users_name_test) WHERE name = 'kim';",
        "EXPLAIN SELECT * FROM users WHERE id = 1;",
        "SHOW INDEX FROM users;",
        "BENCHMARK 5;",
        "SELECT * FROM users WHERE id = 5;",
        "SELECT * FROM users WHERE id > 3;",
        "SELECT * FROM users WHERE id <= 2;",
        "SELECT * FROM users WHERE id BETWEEN 2 AND 4;",
        "SELECT id, name FROM users WHERE id BETWEEN 2 AND 4;",
        "SELECT * FROM users IGNORE INDEX (PRIMARY) WHERE id >= 4;",
        "CREATE INDEX idx_users_age_test ON users (age);",
        "SELECT * FROM users FORCE INDEX (idx_users_age_test) WHERE age BETWEEN 20 AND 22;",
        NULL
    };
    for (int i = 0; cmds[i]; i++) {
        printf("\n-- %s\n", cmds[i]);
        execute_command(&db, cmds[i]);
    }
    db_clear(&db);
    return 0;
}

int db_app_main(int argc, char **argv) {
    /* --self-test 인자가 있으면 REPL 대신 테스트 시나리오를 실행합니다. */
    if (argc > 1 && strcmp(argv[1], "--self-test") == 0) {
        return run_self_test();
    }
    Database db;
    db_init(&db);
    double begin = now_sec();
    char err[512];
    /* 시작 시 data/ 파일을 읽고 메모리 B+Tree 인덱스를 재구성합니다. */
    if (!db_load_startup(&db, err, sizeof(err), begin)) {
        printf("File Error: %s\n", err);
        printf("Execution time: %.6f sec\n", now_sec() - begin);
    }
    /* 사용자가 EXIT할 때까지 SQL 명령을 처리합니다. */
    int rc = run_repl(&db);
    db_clear(&db);
    return rc;
}
