#include "engine_internal.h"
#include "engine.h"

static int run_repl(Database *db) {
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
            continue;
        }
        ExecStatus status = execute_command(db, command);
        command[0] = '\0';
        if (status == EXEC_EXIT) {
            return 0;
        }
    }
    double begin = now_sec();
    execute_save(db, begin);
    return 0;
}

static int run_self_test(void) {
    Database db;
    db_init(&db);
    db.persist = false;
    char err[512];
    (void)err;
    add_index_meta(&db, "PRIMARY", db.schema.primary_column, true, true, true);
    rebuild_indexes(&db, err, sizeof(err));
    const char *cmds[] = {
        "INSERT INTO users (name, age, email) VALUES ('kim', 20, 'kim@test.com');",
        "INSERT INTO users (email, age, name) VALUES ('lee@test.com', 25, 'lee');",
        "SELECT * FROM users FORCE INDEX (PRIMARY) WHERE id = 1;",
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
    if (argc > 1 && strcmp(argv[1], "--self-test") == 0) {
        return run_self_test();
    }
    Database db;
    db_init(&db);
    double begin = now_sec();
    char err[512];
    if (!db_load_startup(&db, err, sizeof(err), begin)) {
        printf("File Error: %s\n", err);
        printf("Execution time: %.6f sec\n", now_sec() - begin);
    }
    int rc = run_repl(&db);
    db_clear(&db);
    return rc;
}
