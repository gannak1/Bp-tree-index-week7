#include "sql_processor.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    const TokenArray *tokens;
    int index;
} Parser;

/* 현재 parser가 바라보는 토큰을 반환한다. */
static const Token *current_token(Parser *parser) {
    return &parser->tokens->items[parser->index];
}

/* 현재 토큰이 기대한 type이면 소비하고 1을 반환한다. */
static int match(Parser *parser, TokenType type) {
    if (current_token(parser)->type == type) {
        parser->index++;
        return 1;
    }
    return 0;
}

/* 반드시 필요한 토큰을 검사하고, 실패 시 읽기 쉬운 오류를 남긴다. */
static int expect(Parser *parser, TokenType type, Status *status, const char *message) {
    if (!match(parser, type)) {
        snprintf(status->message, sizeof(status->message), "%s", message);
        return 0;
    }
    return 1;
}

/* identifier 하나를 읽어 heap 문자열로 복사해 반환한다. */
static char *parse_identifier_text(Parser *parser, Status *status, const char *message) {
    char *copy;

    if (current_token(parser)->type != TOKEN_IDENTIFIER) {
        snprintf(status->message, sizeof(status->message), "%s", message);
        return NULL;
    }

    copy = sp_strdup(current_token(parser)->text);
    parser->index++;
    return copy;
}

/* 리터럴 하나를 VALUE 노드로 생성한다. */
static ASTNode *parse_value_node(Parser *parser, Status *status) {
    ASTNode *node;
    ASTValueType value_type;

    if (current_token(parser)->type != TOKEN_STRING && current_token(parser)->type != TOKEN_NUMBER) {
        snprintf(status->message, sizeof(status->message), "Parse error: expected literal value");
        return NULL;
    }

    value_type = current_token(parser)->type == TOKEN_STRING ? AST_VALUE_STRING : AST_VALUE_NUMBER;
    node = create_ast_node(NODE_VALUE, current_token(parser)->text, value_type);
    if (node == NULL) {
        snprintf(status->message, sizeof(status->message), "Execution error: out of memory");
        return NULL;
    }
    parser->index++;
    return node;
}

/* 현재 토큰이 비교 연산자면 해당 연산자 문자열을 반환한다. */
static const char *parse_operator_text(Parser *parser, Status *status) {
    const Token *token = current_token(parser);
    switch (token->type) {
    case TOKEN_EQUAL: parser->index++; return "=";
    case TOKEN_NOT_EQUAL: parser->index++; return "!=";
    case TOKEN_GREATER: parser->index++; return ">";
    case TOKEN_GREATER_EQUAL: parser->index++; return ">=";
    case TOKEN_LESS: parser->index++; return "<";
    case TOKEN_LESS_EQUAL: parser->index++; return "<=";
    default:
        snprintf(status->message, sizeof(status->message), "Parse error: expected comparison operator in WHERE");
        return NULL;
    }
}

/* schema.table 또는 table 단독 형식을 TABLE 노드 아래 자식 둘로 만든다. */
static ASTNode *parse_table_node(Parser *parser, Status *status) {
    ASTNode *table_node;
    ASTNode *schema_node;
    ASTNode *table_name_node;
    char *first_identifier;

    first_identifier = parse_identifier_text(parser, status, "Parse error: expected schema or table name");
    if (first_identifier == NULL) {
        return NULL;
    }

    table_node = create_ast_node(NODE_TABLE, NULL, AST_VALUE_NONE);
    if (table_node == NULL) {
        free(first_identifier);
        snprintf(status->message, sizeof(status->message), "Execution error: out of memory");
        return NULL;
    }

    if (match(parser, TOKEN_DOT)) {
        char *table_text = parse_identifier_text(parser, status, "Parse error: expected table name");
        if (table_text == NULL) {
            free(first_identifier);
            free_ast(table_node);
            return NULL;
        }
        schema_node = create_ast_node(NODE_IDENTIFIER, first_identifier, AST_VALUE_NONE);
        table_name_node = create_ast_node(NODE_IDENTIFIER, table_text, AST_VALUE_NONE);
        free(first_identifier);
        free(table_text);
    } else {
        schema_node = create_ast_node(NODE_IDENTIFIER, DEFAULT_SCHEMA_NAME, AST_VALUE_NONE);
        table_name_node = create_ast_node(NODE_IDENTIFIER, first_identifier, AST_VALUE_NONE);
        free(first_identifier);
    }

    if (schema_node == NULL || table_name_node == NULL) {
        free_ast(schema_node);
        free_ast(table_name_node);
        free_ast(table_node);
        snprintf(status->message, sizeof(status->message), "Execution error: out of memory");
        return NULL;
    }

    append_child(table_node, schema_node);
    append_child(table_node, table_name_node);
    return table_node;
}

/* SELECT col1, col2 또는 SELECT * 를 COLUMN_LIST 노드로 만든다. */
static ASTNode *parse_column_list_node(Parser *parser, Status *status) {
    ASTNode *list_node;

    list_node = create_ast_node(NODE_COLUMN_LIST, NULL, AST_VALUE_NONE);
    if (list_node == NULL) {
        snprintf(status->message, sizeof(status->message), "Execution error: out of memory");
        return NULL;
    }

    if (match(parser, TOKEN_STAR)) {
        ASTNode *column_node = create_ast_node(NODE_COLUMN, "*", AST_VALUE_NONE);
        if (column_node == NULL) {
            free_ast(list_node);
            snprintf(status->message, sizeof(status->message), "Execution error: out of memory");
            return NULL;
        }
        append_child(list_node, column_node);
        return list_node;
    }

    while (1) {
        char *column_name = parse_identifier_text(parser, status, "Parse error: expected column name");
        ASTNode *column_node;

        if (column_name == NULL) {
            free_ast(list_node);
            return NULL;
        }

        column_node = create_ast_node(NODE_COLUMN, column_name, AST_VALUE_NONE);
        free(column_name);
        if (column_node == NULL) {
            free_ast(list_node);
            snprintf(status->message, sizeof(status->message), "Execution error: out of memory");
            return NULL;
        }
        append_child(list_node, column_node);

        if (!match(parser, TOKEN_COMMA)) {
            break;
        }
    }

    return list_node;
}

/* WHERE column op value 를 WHERE 노드로 만든다. */
static ASTNode *parse_where_node(Parser *parser, Status *status) {
    ASTNode *where_node;
    ASTNode *column_node;
    ASTNode *operator_node;
    ASTNode *value_node;
    char *column_name;
    const char *operator_text;

    column_name = parse_identifier_text(parser, status, "Parse error: expected WHERE column");
    if (column_name == NULL) {
        return NULL;
    }

    operator_text = parse_operator_text(parser, status);
    if (operator_text == NULL) {
        free(column_name);
        return NULL;
    }

    value_node = parse_value_node(parser, status);
    if (value_node == NULL) {
        free(column_name);
        return NULL;
    }

    where_node = create_ast_node(NODE_WHERE, NULL, AST_VALUE_NONE);
    column_node = create_ast_node(NODE_COLUMN, column_name, AST_VALUE_NONE);
    operator_node = create_ast_node(NODE_OPERATOR, operator_text, AST_VALUE_NONE);
    free(column_name);
    if (where_node == NULL || column_node == NULL || operator_node == NULL) {
        free_ast(where_node);
        free_ast(column_node);
        free_ast(operator_node);
        free_ast(value_node);
        snprintf(status->message, sizeof(status->message), "Execution error: out of memory");
        return NULL;
    }

    append_child(where_node, column_node);
    append_child(where_node, operator_node);
    append_child(where_node, value_node);
    return where_node;
}

/* INSERT의 VALUES (...) 목록을 VALUE_LIST 노드로 만든다. */
static ASTNode *parse_value_list_node(Parser *parser, Status *status) {
    ASTNode *list_node;

    list_node = create_ast_node(NODE_VALUE_LIST, NULL, AST_VALUE_NONE);
    if (list_node == NULL) {
        snprintf(status->message, sizeof(status->message), "Execution error: out of memory");
        return NULL;
    }

    while (1) {
        ASTNode *value_node = parse_value_node(parser, status);
        if (value_node == NULL) {
            free_ast(list_node);
            return NULL;
        }
        append_child(list_node, value_node);

        if (!match(parser, TOKEN_COMMA)) {
            break;
        }
    }

    return list_node;
}

/* INSERT INTO ... VALUES (...) 문 전체를 INSERT 루트 노드로 만든다. */
static ASTNode *parse_insert(Parser *parser, Status *status) {
    ASTNode *root;
    ASTNode *table_node;
    ASTNode *value_list_node;

    if (!expect(parser, TOKEN_KEYWORD_INSERT, status, "Parse error: expected INSERT")) return NULL;
    if (!expect(parser, TOKEN_KEYWORD_INTO, status, "Parse error: expected INTO")) return NULL;

    table_node = parse_table_node(parser, status);
    if (table_node == NULL) return NULL;

    if (!expect(parser, TOKEN_KEYWORD_VALUES, status, "Parse error: expected VALUES")) {
        free_ast(table_node);
        return NULL;
    }
    if (!expect(parser, TOKEN_LPAREN, status, "Parse error: expected '(' after VALUES")) {
        free_ast(table_node);
        return NULL;
    }

    value_list_node = parse_value_list_node(parser, status);
    if (value_list_node == NULL) {
        free_ast(table_node);
        return NULL;
    }

    if (!expect(parser, TOKEN_RPAREN, status, "Parse error: expected ')' after values")) {
        free_ast(table_node);
        free_ast(value_list_node);
        return NULL;
    }
    match(parser, TOKEN_SEMICOLON);
    if (!expect(parser, TOKEN_EOF, status, "Parse error: unexpected tokens after statement")) {
        free_ast(table_node);
        free_ast(value_list_node);
        return NULL;
    }

    root = create_ast_node(NODE_INSERT, NULL, AST_VALUE_NONE);
    if (root == NULL) {
        free_ast(table_node);
        free_ast(value_list_node);
        snprintf(status->message, sizeof(status->message), "Execution error: out of memory");
        return NULL;
    }

    append_child(root, table_node);
    append_child(root, value_list_node);
    return root;
}

/* SELECT ... FROM ... [WHERE ...] 문 전체를 SELECT 루트 노드로 만든다. */
static ASTNode *parse_select(Parser *parser, Status *status) {
    ASTNode *root;
    ASTNode *column_list_node;
    ASTNode *table_node;
    ASTNode *where_node = NULL;

    if (!expect(parser, TOKEN_KEYWORD_SELECT, status, "Parse error: expected SELECT")) return NULL;

    column_list_node = parse_column_list_node(parser, status);
    if (column_list_node == NULL) return NULL;

    if (!expect(parser, TOKEN_KEYWORD_FROM, status, "Parse error: expected FROM")) {
        free_ast(column_list_node);
        return NULL;
    }

    table_node = parse_table_node(parser, status);
    if (table_node == NULL) {
        free_ast(column_list_node);
        return NULL;
    }

    if (match(parser, TOKEN_KEYWORD_WHERE)) {
        where_node = parse_where_node(parser, status);
        if (where_node == NULL) {
            free_ast(column_list_node);
            free_ast(table_node);
            return NULL;
        }
    }

    match(parser, TOKEN_SEMICOLON);
    if (!expect(parser, TOKEN_EOF, status, "Parse error: unexpected tokens after statement")) {
        free_ast(column_list_node);
        free_ast(table_node);
        free_ast(where_node);
        return NULL;
    }

    root = create_ast_node(NODE_SELECT, NULL, AST_VALUE_NONE);
    if (root == NULL) {
        free_ast(column_list_node);
        free_ast(table_node);
        free_ast(where_node);
        snprintf(status->message, sizeof(status->message), "Execution error: out of memory");
        return NULL;
    }

    append_child(root, column_list_node);
    append_child(root, table_node);
    if (where_node != NULL) {
        append_child(root, where_node);
    }
    return root;
}

/* 첫 토큰을 보고 INSERT 또는 SELECT 루트 AST를 만든다. */
int parse_statement(const TokenArray *tokens, ASTNode **root, Status *status) {
    Parser parser;

    *root = NULL;
    parser.tokens = tokens;
    parser.index = 0;
    status->ok = 0;
    status->message[0] = '\0';

    if (tokens->count == 0) {
        snprintf(status->message, sizeof(status->message), "Parse error: empty input");
        return 0;
    }

    if (current_token(&parser)->type == TOKEN_KEYWORD_INSERT) {
        *root = parse_insert(&parser, status);
    } else if (current_token(&parser)->type == TOKEN_KEYWORD_SELECT) {
        *root = parse_select(&parser, status);
    } else {
        snprintf(status->message, sizeof(status->message), "Parse error: only INSERT and SELECT are supported");
        return 0;
    }

    if (*root == NULL) {
        return 0;
    }

    status->ok = 1;
    return 1;
}
