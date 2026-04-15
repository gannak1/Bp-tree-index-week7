#ifndef MYSQL_BPTREE_AST_H
#define MYSQL_BPTREE_AST_H

#include <stdbool.h>
#include <stddef.h>

#define AST_SQL_MAX 8192

typedef enum {
    AST_EMPTY = 0,
    AST_EXIT,
    AST_INSERT,
    AST_SELECT,
    AST_EXPLAIN,
    AST_SHOW_INDEX,
    AST_CREATE_INDEX,
    AST_CREATE_UNIQUE_INDEX,
    AST_DROP_INDEX,
    AST_ALTER_PRIMARY_KEY,
    AST_SAVE,
    AST_LOAD_SCHEMA,
    AST_LOAD_DATA_BINARY,
    AST_BENCHMARK,
    AST_UNSUPPORTED
} AstKind;

typedef struct {
    AstKind kind;
    char sql[AST_SQL_MAX];
} SqlAst;

bool sql_ast_parse(const char *input, SqlAst *ast, char *err, size_t err_size);
const char *sql_ast_kind_name(AstKind kind);

#endif
