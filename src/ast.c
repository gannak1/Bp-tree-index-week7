#include "ast.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * ast.c
 *
 * SQL 문자열을 명령 단위의 실제 AST 노드 트리로 변환합니다.
 *
 * 예:
 *   SELECT * FROM users FORCE INDEX (PRIMARY) WHERE id BETWEEN 1 AND 10;
 *
 * AST_SELECT
 *   AST_SELECT_LIST "*"
 *   AST_TABLE "users"
 *   AST_INDEX_HINT "FORCE PRIMARY"
 *   AST_WHERE "id BETWEEN 1 AND 10"
 *     AST_CONDITION "id BETWEEN 1 AND 10"
 *
 * executor.c는 root kind로 명령을 분기하고, 기존 SQL 실행 함수들은 ast.sql을 사용해
 * 기존 파서를 재사용합니다. 즉, AST는 실제로 구성하되 실행 로직과도 호환되게 유지합니다.
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
    /* REPL 입력은 보통 세미콜론으로 끝나므로 AST 구성 전에 제거합니다. */
    trim_inplace(sql);
    size_t len = strlen(sql);
    if (len > 0 && sql[len - 1] == ';') {
        sql[len - 1] = '\0';
        trim_inplace(sql);
    }
}

static void copy_slice_trimmed(char *dst, size_t dst_size, const char *base, int start, int length) {
    if (dst_size == 0) {
        return;
    }
    if (start < 0) {
        start = 0;
    }
    if (length < 0) {
        length = 0;
    }
    while (length > 0 && isspace((unsigned char)base[start])) {
        start++;
        length--;
    }
    while (length > 0 && isspace((unsigned char)base[start + length - 1])) {
        length--;
    }
    if ((size_t)length >= dst_size) {
        length = (int)dst_size - 1;
    }
    memcpy(dst, base + start, (size_t)length);
    dst[length] = '\0';
}

static AstNode *ast_node_create(AstKind kind, const char *base, int start, int length) {
    AstNode *node = (AstNode *)calloc(1, sizeof(AstNode));
    if (!node) {
        return NULL;
    }
    node->kind = kind;
    node->start = start;
    node->length = length;
    if (base) {
        copy_slice_trimmed(node->text, sizeof(node->text), base, start, length);
    }
    return node;
}

static bool ast_add_child(AstNode *parent, AstNode *child) {
    if (!parent || !child) {
        return false;
    }
    if (!parent->first_child) {
        parent->first_child = child;
        return true;
    }
    AstNode *tail = parent->first_child;
    while (tail->next_sibling) {
        tail = tail->next_sibling;
    }
    tail->next_sibling = child;
    return true;
}

static void ast_node_free(AstNode *node) {
    while (node) {
        AstNode *next = node->next_sibling;
        ast_node_free(node->first_child);
        free(node);
        node = next;
    }
}

void sql_ast_free(SqlAst *ast) {
    if (!ast) {
        return;
    }
    ast_node_free(ast->root);
    ast->root = NULL;
}

static AstKind classify_command(const char *sql) {
    if (sql[0] == '\0') return AST_EMPTY;
    if (str_casecmp_local(sql, "EXIT") == 0 || str_casecmp_local(sql, "QUIT") == 0) return AST_EXIT;
    if (starts_with_ci(sql, "INSERT")) return AST_INSERT;
    if (starts_with_ci(sql, "EXPLAIN")) return AST_EXPLAIN;
    if (starts_with_ci(sql, "SELECT")) return AST_SELECT;
    if (starts_with_ci(sql, "SHOW INDEX")) return AST_SHOW_INDEX;
    if (starts_with_ci(sql, "CREATE UNIQUE INDEX")) return AST_CREATE_UNIQUE_INDEX;
    if (starts_with_ci(sql, "CREATE INDEX")) return AST_CREATE_INDEX;
    if (starts_with_ci(sql, "DROP INDEX")) return AST_DROP_INDEX;
    if (starts_with_ci(sql, "ALTER TABLE") && find_ci(sql, "MODIFY PRIMARY KEY")) return AST_ALTER_PRIMARY_KEY;
    if (str_casecmp_local(sql, "SAVE") == 0) return AST_SAVE;
    if (starts_with_ci(sql, "LOAD SCHEMA")) return AST_LOAD_SCHEMA;
    if (starts_with_ci(sql, "LOAD DATA BINARY")) return AST_LOAD_DATA_BINARY;
    if (starts_with_ci(sql, "BENCHMARK")) return AST_BENCHMARK;
    return AST_UNSUPPORTED;
}

static const char *find_matching_paren_ast(const char *open) {
    int depth = 0;
    bool in_quote = false;
    char quote = '\0';
    for (const char *p = open; *p; p++) {
        if (*p == '\'' || *p == '"') {
            if (!in_quote) {
                in_quote = true;
                quote = *p;
            } else if (quote == *p && p > open && p[-1] != '\\') {
                in_quote = false;
            }
        }
        if (in_quote) {
            continue;
        }
        if (*p == '(') {
            depth++;
        } else if (*p == ')') {
            depth--;
            if (depth == 0) {
                return p;
            }
        }
    }
    return NULL;
}

static bool add_table_node(AstNode *root, const char *sql, const char *table_pos) {
    return ast_add_child(root, ast_node_create(AST_TABLE, sql, (int)(table_pos - sql), (int)strlen("users")));
}

static bool parse_select_ast(SqlAst *ast, bool explain, char *err, size_t err_size) {
    const char *sql = ast->sql;
    const char *select_sql = explain ? find_ci(sql, "SELECT") : sql;
    if (!select_sql) {
        snprintf(err, err_size, "Expected SELECT after EXPLAIN.");
        return false;
    }
    const char *from = find_ci(select_sql, "FROM");
    if (!from) {
        snprintf(err, err_size, "Expected FROM clause.");
        return false;
    }
    ast_add_child(ast->root, ast_node_create(AST_SELECT_LIST, sql,
                                             (int)(select_sql + strlen("SELECT") - sql),
                                             (int)(from - (select_sql + strlen("SELECT")))));
    const char *table = find_ci(from, "users");
    if (table) {
        add_table_node(ast->root, sql, table);
    }

    const char *where = find_ci(from, "WHERE");
    const char *force = find_ci(from, "FORCE INDEX");
    const char *ignore = find_ci(from, "IGNORE INDEX");
    const char *hint = NULL;
    const char *hint_word = NULL;
    if (force && (!where || force < where)) {
        hint = force;
        hint_word = "FORCE";
    } else if (ignore && (!where || ignore < where)) {
        hint = ignore;
        hint_word = "IGNORE";
    }
    if (hint) {
        const char *open = strchr(hint, '(');
        const char *close = open ? strchr(open, ')') : NULL;
        AstNode *hint_node = ast_node_create(AST_INDEX_HINT, sql, (int)(hint - sql),
                                             close ? (int)(close - hint + 1) : (int)strlen(hint));
        if (hint_node && open && close) {
            char index_name[128];
            copy_slice_trimmed(index_name, sizeof(index_name), sql, (int)(open + 1 - sql), (int)(close - open - 1));
            snprintf(hint_node->text, sizeof(hint_node->text), "%s %s", hint_word, index_name);
        }
        ast_add_child(ast->root, hint_node);
    }
    if (where) {
        AstNode *where_node = ast_node_create(AST_WHERE, sql, (int)(where - sql), (int)strlen(where));
        const char *condition = where + strlen("WHERE");
        AstNode *condition_node = ast_node_create(AST_CONDITION, sql, (int)(condition - sql), (int)strlen(condition));
        ast_add_child(where_node, condition_node);
        ast_add_child(ast->root, where_node);
    }
    return true;
}

static bool parse_insert_ast(SqlAst *ast, char *err, size_t err_size) {
    const char *sql = ast->sql;
    const char *table = find_ci(sql, "users");
    const char *open_cols = table ? strchr(table, '(') : NULL;
    const char *close_cols = open_cols ? find_matching_paren_ast(open_cols) : NULL;
    const char *values = close_cols ? find_ci(close_cols, "VALUES") : NULL;
    const char *open_vals = values ? strchr(values, '(') : NULL;
    const char *close_vals = open_vals ? find_matching_paren_ast(open_vals) : NULL;
    if (!table || !open_cols || !close_cols || !values || !open_vals || !close_vals) {
        snprintf(err, err_size, "Invalid INSERT AST shape.");
        return true;
    }
    ast_add_child(ast->root, ast_node_create(AST_TABLE, sql, (int)(table - sql), (int)strlen("users")));
    ast_add_child(ast->root, ast_node_create(AST_COLUMN_LIST, sql,
                                             (int)(open_cols + 1 - sql), (int)(close_cols - open_cols - 1)));
    ast_add_child(ast->root, ast_node_create(AST_VALUE_LIST, sql,
                                             (int)(open_vals + 1 - sql), (int)(close_vals - open_vals - 1)));
    return true;
}

static bool parse_index_ast(SqlAst *ast) {
    const char *sql = ast->sql;
    const char *on = find_ci(sql, "ON");
    const char *table = on ? find_ci(on, "users") : NULL;
    const char *open = on ? strchr(on, '(') : NULL;
    const char *close = open ? strchr(open, ')') : NULL;
    if (table) {
        ast_add_child(ast->root, ast_node_create(AST_TABLE, sql, (int)(table - sql), (int)strlen("users")));
    }
    if (open && close) {
        ast_add_child(ast->root, ast_node_create(AST_COLUMN_LIST, sql,
                                                 (int)(open + 1 - sql), (int)(close - open - 1)));
    }
    return true;
}

static bool parse_load_ast(SqlAst *ast) {
    const char *sql = ast->sql;
    const char *q1 = strchr(sql, '\'');
    char quote = '\'';
    if (!q1) {
        q1 = strchr(sql, '"');
        quote = '"';
    }
    const char *q2 = q1 ? strchr(q1 + 1, quote) : NULL;
    if (q1 && q2) {
        ast_add_child(ast->root, ast_node_create(AST_PATH, sql, (int)(q1 + 1 - sql), (int)(q2 - q1 - 1)));
    }
    return true;
}

static bool parse_benchmark_ast(SqlAst *ast) {
    const char *sql = ast->sql;
    const char *index = find_ci(sql, "INDEX");
    if (index) {
        ast_add_child(ast->root, ast_node_create(AST_BENCHMARK_OPTIONS, sql,
                                                 (int)(index - sql), (int)strlen(index)));
    }
    return true;
}

static bool populate_children(SqlAst *ast, char *err, size_t err_size) {
    switch (ast->kind) {
        case AST_SELECT:
            return parse_select_ast(ast, false, err, err_size);
        case AST_EXPLAIN:
            return parse_select_ast(ast, true, err, err_size);
        case AST_INSERT:
            return parse_insert_ast(ast, err, err_size);
        case AST_CREATE_INDEX:
        case AST_CREATE_UNIQUE_INDEX:
            return parse_index_ast(ast);
        case AST_LOAD_SCHEMA:
        case AST_LOAD_DATA_BINARY:
            return parse_load_ast(ast);
        case AST_BENCHMARK:
            return parse_benchmark_ast(ast);
        default:
            return true;
    }
}

bool sql_ast_parse(const char *input, SqlAst *ast, char *err, size_t err_size) {
    /*
     * 입력 SQL을 정리하고 AstNode 트리를 구성합니다.
     * ast->root가 실제 AST의 루트이며, ast->kind는 root kind의 빠른 접근용 복사본입니다.
     */
    memset(ast, 0, sizeof(*ast));
    ast->kind = AST_EMPTY;
    if (!input) {
        ast->root = ast_node_create(AST_EMPTY, "", 0, 0);
        return ast->root != NULL;
    }
    if (strlen(input) >= sizeof(ast->sql)) {
        snprintf(err, err_size, "SQL command is too long.");
        return false;
    }
    snprintf(ast->sql, sizeof(ast->sql), "%s", input);
    strip_optional_semicolon(ast->sql);
    ast->kind = classify_command(ast->sql);
    ast->root = ast_node_create(ast->kind, ast->sql, 0, (int)strlen(ast->sql));
    if (!ast->root) {
        snprintf(err, err_size, "Failed to allocate AST root.");
        return false;
    }
    if (!populate_children(ast, err, err_size)) {
        sql_ast_free(ast);
        return false;
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
        case AST_TABLE: return "TABLE";
        case AST_SELECT_LIST: return "SELECT_LIST";
        case AST_COLUMN_LIST: return "COLUMN_LIST";
        case AST_VALUE_LIST: return "VALUE_LIST";
        case AST_WHERE: return "WHERE";
        case AST_CONDITION: return "CONDITION";
        case AST_INDEX_HINT: return "INDEX_HINT";
        case AST_PATH: return "PATH";
        case AST_BENCHMARK_OPTIONS: return "BENCHMARK_OPTIONS";
        default: return "UNKNOWN";
    }
}
