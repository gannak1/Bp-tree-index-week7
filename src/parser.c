#include "sql_processor.h"

#include <stdlib.h>
#include <string.h>

/*
 * Parser는 "토큰 배열 + 현재 읽는 위치"만 알면 된다.
 * - tokens: lexer가 만든 전체 토큰 배열
 * - index: 현재 parser가 바라보고 있는 토큰 위치
 */
typedef struct {
    const TokenArray *tokens;
    int index;
} Parser;

/* 현재 parser가 바라보는 토큰을 반환한다. */
static const Token *current_token(Parser *parser) {
    return &parser->tokens->items[parser->index];
}

/*
 * 현재 토큰이 기대한 type이면 소비하고 1을 반환한다.
 * 아니면 index를 움직이지 않고 0을 반환한다.
 *
 * 어디에 쓰나:
 * - 선택적으로 나올 수 있는 토큰(쉼표, 세미콜론, WHERE 등) 처리
 */
static int match(Parser *parser, TokenType type) {
    if (current_token(parser)->type == type) {
        parser->index++;
        return 1;
    }
    return 0;
}

/*
 * 반드시 필요한 토큰을 검사한다.
 *
 * 성공:
 * - match처럼 토큰을 소비하고 1 반환
 *
 * 실패:
 * - status.message에 사람이 읽기 쉬운 오류를 남기고 0 반환
 */
static int expect(Parser *parser, TokenType type, Status *status, const char *message) {
    if (!match(parser, type)) {
        snprintf(status->message, sizeof(status->message), "%s", message);
        return 0;
    }
    return 1;
}

/*
 * IDENTIFIER 토큰 하나를 읽어서 heap 문자열로 복사해 반환한다.
 *
 * 예:
 * - schema 이름
 * - table 이름
 * - column 이름
 */
static char *parse_identifier_text(Parser *parser, Status *status, const char *message) {
    char *copy; /* 토큰 text를 따로 복사한 문자열 */

    if (current_token(parser)->type != TOKEN_IDENTIFIER) {
        snprintf(status->message, sizeof(status->message), "%s", message);
        return NULL;
    }

    copy = sp_strdup(current_token(parser)->text);
    parser->index++;
    return copy;
}

/*
 * STRING 또는 NUMBER 토큰 하나를 읽어서 NODE_VALUE 노드로 만든다.
 *
 * 반환 노드의:
 * - type = NODE_VALUE
 * - text = 원래 literal 문자열
 * - value_type = STRING 또는 NUMBER
 */
static ASTNode *parse_value_node(Parser *parser, Status *status) {
    ASTNode *node;              /* 최종 생성할 VALUE 노드 */
    ASTValueType value_type;    /* 현재 literal이 문자열인지 숫자인지 */

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

/*
 * BETWEEN lower AND upper 구문을 NODE_BETWEEN 노드로 만든다.
 *
 * AST 구조:
 * NODE_BETWEEN
 * ├── NODE_VALUE(lower)
 * └── NODE_VALUE(upper)
 */
static ASTNode *parse_between_node(Parser *parser, Status *status) {
    ASTNode *between_node; /* BETWEEN 부모 노드 */
    ASTNode *lower_node;   /* lower bound 값 노드 */
    ASTNode *upper_node;   /* upper bound 값 노드 */

    if (!expect(parser, TOKEN_KEYWORD_BETWEEN, status, "Parse error: expected BETWEEN")) {
        return NULL;
    }

    lower_node = parse_value_node(parser, status);
    if (lower_node == NULL) {
        snprintf(status->message, sizeof(status->message), "Parse error: expected BETWEEN lower bound");
        return NULL;
    }

    if (!expect(parser, TOKEN_KEYWORD_AND, status, "Parse error: expected AND in BETWEEN clause")) {
        free_ast(lower_node);
        return NULL;
    }

    upper_node = parse_value_node(parser, status);
    if (upper_node == NULL) {
        free_ast(lower_node);
        snprintf(status->message, sizeof(status->message), "Parse error: expected BETWEEN upper bound");
        return NULL;
    }

    between_node = create_ast_node(NODE_BETWEEN, NULL, AST_VALUE_NONE);
    if (between_node == NULL) {
        free_ast(lower_node);
        free_ast(upper_node);
        snprintf(status->message, sizeof(status->message), "Execution error: out of memory");
        return NULL;
    }

    append_child(between_node, lower_node);
    append_child(between_node, upper_node);
    return between_node;
}

/*
 * 현재 토큰이 비교 연산자면 그 연산자 문자열을 반환한다.
 *
 * 예:
 * - TOKEN_GREATER_EQUAL -> ">="
 *
 * 이 함수는 반환 문자열을 새로 할당하지 않고 상수 문자열을 돌려준다.
 */
static const char *parse_operator_text(Parser *parser, Status *status) {
    const Token *token = current_token(parser); /* 현재 비교 연산자 후보 토큰 */

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

/*
 * schema.table 또는 table 단독 형식을 TABLE 노드로 만든다.
 *
 * 지원 문법:
 * - school.users
 * - users   -> schema는 DEFAULT_SCHEMA_NAME("school")로 자동 채움
 *
 * AST 구조:
 * NODE_TABLE
 * ├── NODE_IDENTIFIER(schema)
 * └── NODE_IDENTIFIER(table)
 */
static ASTNode *parse_table_node(Parser *parser, Status *status) {
    ASTNode *table_node;       /* TABLE 부모 노드 */
    ASTNode *schema_node;      /* schema IDENTIFIER 노드 */
    ASTNode *table_name_node;  /* table IDENTIFIER 노드 */
    char *first_identifier;    /* 처음 읽은 identifier 텍스트 */

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

/*
 * SELECT col1, col2 또는 SELECT * 를 COLUMN_LIST 노드로 만든다.
 *
 * AST 구조:
 * NODE_COLUMN_LIST
 * ├── NODE_COLUMN(...)
 * ├── NODE_COLUMN(...)
 * └── ...
 */
static ASTNode *parse_column_list_node(Parser *parser, Status *status) {
    ASTNode *list_node; /* COLUMN_LIST 부모 노드 */

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

/*
 * WHERE 절을 파싱한다.
 *
 * 지원 문법 1:
 * - WHERE column op value
 *
 * 지원 문법 2:
 * - WHERE column BETWEEN low AND high
 *
 * AST 구조:
 * - 비교 연산:
 *   NODE_WHERE -> COLUMN, OPERATOR, VALUE
 * - BETWEEN:
 *   NODE_WHERE -> COLUMN, BETWEEN
 */
static ASTNode *parse_where_node(Parser *parser, Status *status) {
    ASTNode *where_node;       /* WHERE 부모 노드 */
    ASTNode *column_node;      /* 비교 대상 컬럼 노드 */
    ASTNode *operator_node = NULL; /* 비교 연산자 노드 */
    ASTNode *value_node = NULL;    /* 비교할 값 노드 */
    ASTNode *between_node = NULL;  /* BETWEEN 노드 */
    char *column_name;         /* WHERE 앞쪽 컬럼 이름 텍스트 */
    const char *operator_text; /* 연산자 문자열 */
    
    column_name = parse_identifier_text(parser, status, "Parse error: expected WHERE column");
    if (column_name == NULL) {
        return NULL;
    }

    where_node = create_ast_node(NODE_WHERE, NULL, AST_VALUE_NONE);
    column_node = create_ast_node(NODE_COLUMN, column_name, AST_VALUE_NONE);
    free(column_name);
    if (where_node == NULL || column_node == NULL) {
        free_ast(where_node);
        free_ast(column_node);
        snprintf(status->message, sizeof(status->message), "Execution error: out of memory");
        return NULL;
    }

    append_child(where_node, column_node);

    if (current_token(parser)->type == TOKEN_KEYWORD_BETWEEN) {
        between_node = parse_between_node(parser, status);
        if (between_node == NULL) {
            free_ast(where_node);
            return NULL;
        }
        append_child(where_node, between_node);
        return where_node;
    }

    operator_text = parse_operator_text(parser, status);
    if (operator_text == NULL) {
        free_ast(where_node);
        return NULL;
    }

    value_node = parse_value_node(parser, status);
    if (value_node == NULL) {
        free_ast(where_node);
        return NULL;
    }

    operator_node = create_ast_node(NODE_OPERATOR, operator_text, AST_VALUE_NONE);
    if (operator_node == NULL) {
        free_ast(where_node);
        free_ast(value_node);
        snprintf(status->message, sizeof(status->message), "Execution error: out of memory");
        return NULL;
    }

    append_child(where_node, operator_node);
    append_child(where_node, value_node);
    return where_node;
}

/*
 * INSERT의 VALUES (...) 목록을 VALUE_LIST 노드로 만든다.
 *
 * 각 VALUE 노드는 next_sibling으로 이어진다.
 */
static ASTNode *parse_value_list_node(Parser *parser, Status *status) {
    ASTNode *list_node; /* VALUE_LIST 부모 노드 */

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

/*
 * INSERT INTO ... VALUES (...) 문 전체를 INSERT 루트 노드로 만든다.
 */
static ASTNode *parse_insert(Parser *parser, Status *status) {
    ASTNode *root;             /* 최종 INSERT 루트 노드 */
    ASTNode *table_node;       /* 대상 TABLE 노드 */
    ASTNode *value_list_node;  /* VALUES 목록 노드 */

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

/*
 * SELECT ... FROM ... [WHERE ...] 문 전체를 SELECT 루트 노드로 만든다.
 */
static ASTNode *parse_select(Parser *parser, Status *status) {
    ASTNode *root;             /* 최종 SELECT 루트 노드 */
    ASTNode *column_list_node; /* SELECT 대상 컬럼 목록 */
    ASTNode *table_node;       /* FROM 대상 테이블 */
    ASTNode *where_node = NULL;/* 선택적 WHERE 노드 */

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

/*
 * 첫 토큰을 보고 INSERT 또는 SELECT 루트 AST를 만든다.
 *
 * parser 전체 진입점이며,
 * 이후 executor는 여기서 만들어진 AST만 보고 실행을 결정한다.
 */
int parse_statement(const TokenArray *tokens, ASTNode **root, Status *status) {
    Parser parser; /* 현재 토큰 배열과 읽는 위치를 담는 parser 상태 */

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
