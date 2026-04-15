#include "ast.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

/*
 * ast.c
 *
 * SQL 전체를 완전한 문법 트리로 만들지는 않고, 현재 과제 범위에 맞춰
 * "명령 종류"를 구분하는 얇은 AST 계층을 제공합니다.
 *
 * 예:
 *   SELECT ...       -> AST_SELECT
 *   CREATE INDEX ... -> AST_CREATE_INDEX
 *   BENCHMARK ...    -> AST_BENCHMARK
 *
 * 세부 WHERE 조건은 query.c의 QueryCondition이 담당합니다.
 */

static int ci_char(int c) {
    return tolower((unsigned char)c);
}

static int str_casecmp_local(const char *a, const char *b) {
    while (*a && *b) {
        int ca = ci_char(*a);
        int cb = ci_char(*b);
        if (ca != cb) {
            return ca - cb;
        }
        a++;
        b++;
    }
    return ci_char(*a) - ci_char(*b);
}

static bool starts_with_ci(const char *s, const char *prefix) {
    while (*prefix) {
        if (ci_char(*s) != ci_char(*prefix)) {
            return false;
        }
        s++;
        prefix++;
    }
    return true;
}

static const char *find_ci(const char *haystack, const char *needle) {
    size_t n = strlen(needle);
    if (n == 0) {
        return haystack;
    }
    for (const char *p = haystack; *p; p++) {
        size_t i = 0;
        while (i < n && p[i] && ci_char(p[i]) == ci_char(needle[i])) {
            i++;
        }
        if (i == n) {
            return p;
        }
    }
    return NULL;
}

static void trim_inplace(char *s) {
    char *start = s;
    while (isspace((unsigned char)*start)) {
        start++;
    }
    if (start != s) {
        memmove(s, start, strlen(start) + 1);
    }
    size_t len = strlen(s);
    while (len > 0 && isspace((unsigned char)s[len - 1])) {
        s[--len] = '\0';
    }
}

static void strip_optional_semicolon(char *sql) {
    /* REPL 입력은 보통 세미콜론으로 끝나므로 실행 전에 제거합니다. */
    trim_inplace(sql);
    size_t len = strlen(sql);
    if (len > 0 && sql[len - 1] == ';') {
        sql[len - 1] = '\0';
        trim_inplace(sql);
    }
}

bool sql_ast_parse(const char *input, SqlAst *ast, char *err, size_t err_size) {
    /*
     * 입력 SQL을 정리하고 AstKind로 분류합니다.
     * ast.sql에는 앞뒤 공백과 마지막 세미콜론이 제거된 SQL이 저장됩니다.
     */
    memset(ast, 0, sizeof(*ast));
    ast->kind = AST_EMPTY;
    if (!input) {
        return true;
    }
    if (strlen(input) >= sizeof(ast->sql)) {
        snprintf(err, err_size, "SQL command is too long.");
        return false;
    }
    snprintf(ast->sql, sizeof(ast->sql), "%s", input);
    strip_optional_semicolon(ast->sql);
    if (ast->sql[0] == '\0') {
        ast->kind = AST_EMPTY;
        return true;
    }
    /*
     * 긴 키워드를 짧은 키워드보다 먼저 검사해야 합니다.
     * 예를 들어 CREATE UNIQUE INDEX는 CREATE INDEX보다 먼저 확인해야 unique 정보를 잃지 않습니다.
     */
    if (str_casecmp_local(ast->sql, "EXIT") == 0 || str_casecmp_local(ast->sql, "QUIT") == 0) {
        ast->kind = AST_EXIT;
    } else if (starts_with_ci(ast->sql, "INSERT")) {
        ast->kind = AST_INSERT;
    } else if (starts_with_ci(ast->sql, "EXPLAIN")) {
        ast->kind = AST_EXPLAIN;
    } else if (starts_with_ci(ast->sql, "SELECT")) {
        ast->kind = AST_SELECT;
    } else if (starts_with_ci(ast->sql, "SHOW INDEX")) {
        ast->kind = AST_SHOW_INDEX;
    } else if (starts_with_ci(ast->sql, "CREATE UNIQUE INDEX")) {
        ast->kind = AST_CREATE_UNIQUE_INDEX;
    } else if (starts_with_ci(ast->sql, "CREATE INDEX")) {
        ast->kind = AST_CREATE_INDEX;
    } else if (starts_with_ci(ast->sql, "DROP INDEX")) {
        ast->kind = AST_DROP_INDEX;
    } else if (starts_with_ci(ast->sql, "ALTER TABLE") && find_ci(ast->sql, "MODIFY PRIMARY KEY")) {
        ast->kind = AST_ALTER_PRIMARY_KEY;
    } else if (str_casecmp_local(ast->sql, "SAVE") == 0) {
        ast->kind = AST_SAVE;
    } else if (starts_with_ci(ast->sql, "LOAD SCHEMA")) {
        ast->kind = AST_LOAD_SCHEMA;
    } else if (starts_with_ci(ast->sql, "LOAD DATA BINARY")) {
        ast->kind = AST_LOAD_DATA_BINARY;
    } else if (starts_with_ci(ast->sql, "BENCHMARK")) {
        ast->kind = AST_BENCHMARK;
    } else {
        ast->kind = AST_UNSUPPORTED;
    }
    return true;
}

const char *sql_ast_kind_name(AstKind kind) {
    switch (kind) {
        case AST_EMPTY: return "EMPTY";
        case AST_EXIT: return "EXIT";
        case AST_INSERT: return "INSERT";
        case AST_SELECT: return "SELECT";
        case AST_EXPLAIN: return "EXPLAIN";
        case AST_SHOW_INDEX: return "SHOW_INDEX";
        case AST_CREATE_INDEX: return "CREATE_INDEX";
        case AST_CREATE_UNIQUE_INDEX: return "CREATE_UNIQUE_INDEX";
        case AST_DROP_INDEX: return "DROP_INDEX";
        case AST_ALTER_PRIMARY_KEY: return "ALTER_PRIMARY_KEY";
        case AST_SAVE: return "SAVE";
        case AST_LOAD_SCHEMA: return "LOAD_SCHEMA";
        case AST_LOAD_DATA_BINARY: return "LOAD_DATA_BINARY";
        case AST_BENCHMARK: return "BENCHMARK";
        case AST_UNSUPPORTED: return "UNSUPPORTED";
        default: return "UNKNOWN";
    }
}
