#include "sql_processor.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

/* 동적 토큰 배열 뒤에 새 토큰 하나를 추가한다. */
static int push_token(TokenArray *array, TokenType type, const char *start, size_t length) {
    Token *resized;
    char *text;

    resized = (Token *)realloc(array->items, sizeof(Token) * (array->count + 1));
    if (resized == NULL) {
        return 0;
    }
    array->items = resized;

    text = (char *)malloc(length + 1);
    if (text == NULL) {
        return 0;
    }

    memcpy(text, start, length);
    text[length] = '\0';
    array->items[array->count].type = type;
    array->items[array->count].text = text;
    array->count++;
    return 1;
}

/* identifier가 예약어인지 일반 이름인지 판별한다. */
static TokenType keyword_type(const char *text) {
    if (equals_ignore_case(text, "INSERT")) return TOKEN_KEYWORD_INSERT;
    if (equals_ignore_case(text, "INTO")) return TOKEN_KEYWORD_INTO;
    if (equals_ignore_case(text, "VALUES")) return TOKEN_KEYWORD_VALUES;
    if (equals_ignore_case(text, "SELECT")) return TOKEN_KEYWORD_SELECT;
    if (equals_ignore_case(text, "FROM")) return TOKEN_KEYWORD_FROM;
    if (equals_ignore_case(text, "WHERE")) return TOKEN_KEYWORD_WHERE;
    if (equals_ignore_case(text, "BETWEEN")) return TOKEN_KEYWORD_BETWEEN;
    if (equals_ignore_case(text, "AND")) return TOKEN_KEYWORD_AND;
    return TOKEN_IDENTIFIER;
}

/* 원본 SQL 문자열을 parser가 읽을 토큰 배열로 분해한다. */
TokenArray lex_sql(const char *sql, Status *status) {
    TokenArray array;
    const char *cursor;
    array.items = NULL;
    array.count = 0;
    cursor = sql;
    status->ok = 0;
    status->message[0] = '\0';

    while (*cursor != '\0') {
        const char *start;

        if (isspace((unsigned char)*cursor)) {
            cursor++;
            continue;
        }

        if (*cursor == '*' || *cursor == ',' || *cursor == '.' || *cursor == '(' || *cursor == ')' || *cursor == ';') {
            TokenType type = TOKEN_STAR;
            switch (*cursor) {
            case '*': type = TOKEN_STAR; break;
            case ',': type = TOKEN_COMMA; break;
            case '.': type = TOKEN_DOT; break;
            case '(': type = TOKEN_LPAREN; break;
            case ')': type = TOKEN_RPAREN; break;
            case ';': type = TOKEN_SEMICOLON; break;
            }
            if (!push_token(&array, type, cursor, 1)) goto oom;
            cursor++;
            continue;
        }

        if (*cursor == '!' || *cursor == '>' || *cursor == '<' || *cursor == '=') {
            TokenType type;
            size_t length = 1;

            if (*cursor == '!' && cursor[1] == '=') {
                type = TOKEN_NOT_EQUAL;
                length = 2;
            } else if (*cursor == '>' && cursor[1] == '=') {
                type = TOKEN_GREATER_EQUAL;
                length = 2;
            } else if (*cursor == '<' && cursor[1] == '=') {
                type = TOKEN_LESS_EQUAL;
                length = 2;
            } else if (*cursor == '=') {
                type = TOKEN_EQUAL;
            } else if (*cursor == '>') {
                type = TOKEN_GREATER;
            } else if (*cursor == '<') {
                type = TOKEN_LESS;
            } else {
                snprintf(status->message, sizeof(status->message), "Parse error: unexpected character '%c'", *cursor);
                free_tokens(&array);
                return array;
            }

            if (!push_token(&array, type, cursor, length)) goto oom;
            cursor += length;
            continue;
        }

        if (*cursor == '\'') {
            start = cursor++;
            while (*cursor != '\0' && *cursor != '\'') {
                cursor++;
            }
            if (*cursor != '\'') {
                snprintf(status->message, sizeof(status->message), "Parse error: unterminated string literal");
                free_tokens(&array);
                return array;
            }
            cursor++;
            if (!push_token(&array, TOKEN_STRING, start, (size_t)(cursor - start))) goto oom;
            continue;
        }

        if (isdigit((unsigned char)*cursor) || ((*cursor == '-' || *cursor == '+') && isdigit((unsigned char)cursor[1]))) {
            start = cursor++;
            while (isdigit((unsigned char)*cursor)) {
                cursor++;
            }
            if (!push_token(&array, TOKEN_NUMBER, start, (size_t)(cursor - start))) goto oom;
            continue;
        }

        if (isalpha((unsigned char)*cursor) || *cursor == '_') {
            TokenType type;
            start = cursor++;
            while (isalnum((unsigned char)*cursor) || *cursor == '_') {
                cursor++;
            }
            if (!push_token(&array, TOKEN_IDENTIFIER, start, (size_t)(cursor - start))) goto oom;
            type = keyword_type(array.items[array.count - 1].text);
            array.items[array.count - 1].type = type;
            continue;
        }

        snprintf(status->message, sizeof(status->message), "Parse error: unexpected character '%c'", *cursor);
        free_tokens(&array);
        return array;
    }

    if (!push_token(&array, TOKEN_EOF, "", 0)) goto oom;
    status->ok = 1;
    return array;

oom:
    snprintf(status->message, sizeof(status->message), "Execution error: out of memory");
    free_tokens(&array);
    array.items = NULL;
    array.count = 0;
    return array;
}

/* lexer가 만든 동적 토큰 메모리를 전부 해제한다. */
void free_tokens(TokenArray *tokens) {
    int i;

    if (tokens == NULL || tokens->items == NULL) {
        return;
    }

    for (i = 0; i < tokens->count; i++) {
        free(tokens->items[i].text);
    }
    free(tokens->items);
    tokens->items = NULL;
    tokens->count = 0;
}
