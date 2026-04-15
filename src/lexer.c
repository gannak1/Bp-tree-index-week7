#include "sql_processor.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

/*
 * 동적 토큰 배열 뒤에 새 토큰 하나를 추가한다.
 *
 * 입력:
 * - array: 확장할 토큰 배열
 * - type: 새 토큰 종류
 * - start: 원본 SQL 문자열에서 이 토큰이 시작하는 주소
 * - length: start부터 몇 글자를 복사할지
 *
 * 내부 동작:
 * - Token 배열을 realloc으로 한 칸 늘린다.
 * - 토큰 text를 별도 malloc해서 복사한다.
 * - 마지막 칸에 type/text를 채우고 count를 증가시킨다.
 */
static int push_token(TokenArray *array, TokenType type, const char *start, size_t length) {
    Token *resized; /* 크기를 늘린 새 Token 배열 포인터 */
    char *text;     /* 토큰 문자열 복사본 */

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

/*
 * identifier 문자열이 예약어인지 일반 식별자인지 판별한다.
 *
 * 예:
 * - "SELECT" -> TOKEN_KEYWORD_SELECT
 * - "users"  -> TOKEN_IDENTIFIER
 */
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

/*
 * 원본 SQL 문자열을 parser가 읽을 토큰 배열로 분해한다.
 *
 * 주요 규칙:
 * - 공백은 건너뛴다.
 * - *, , . ( ) ; 는 단일 문자 토큰으로 만든다.
 * - !=, >=, <= 같은 비교 연산자를 만든다.
 * - '...' 는 문자열 literal로 만든다.
 * - 숫자는 NUMBER 토큰으로 만든다.
 * - 영문/숫자/_ 조합은 IDENTIFIER 또는 KEYWORD로 만든다.
 *
 * 반환:
 * - 성공 시 TOKEN_EOF까지 포함된 TokenArray
 * - 실패 시 status.message에 오류를 남기고 비운 배열 반환
 */
TokenArray lex_sql(const char *sql, Status *status) {
    TokenArray array;      /* 최종 반환할 토큰 배열 */
    const char *cursor;    /* 현재 읽고 있는 SQL 문자열 위치 */

    array.items = NULL;
    array.count = 0;
    cursor = sql;
    status->ok = 0;
    status->message[0] = '\0';

    while (*cursor != '\0') {
        const char *start; /* 현재 토큰이 시작된 위치 */

        /* 공백은 토큰 의미가 없으므로 그냥 건너뛴다. */
        if (isspace((unsigned char)*cursor)) {
            cursor++;
            continue;
        }

        /*
         * 단일 문자 토큰 처리
         * - *, , . ( ) ;
         */
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

        /*
         * 비교 연산자 처리
         * - =, !=, >, >=, <, <=
         */
        if (*cursor == '!' || *cursor == '>' || *cursor == '<' || *cursor == '=') {
            TokenType type; /* 현재 연산자에 대응하는 토큰 종류 */
            size_t length = 1; /* 두 글자 연산자면 2로 바뀐다 */

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

        /*
         * 문자열 literal 처리
         * 시작과 끝의 작은따옴표까지 포함해서 text에 저장한다.
         */
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

        /*
         * 숫자 literal 처리
         * 부호(+/-)가 붙은 정수도 허용한다.
         */
        if (isdigit((unsigned char)*cursor) || ((*cursor == '-' || *cursor == '+') && isdigit((unsigned char)cursor[1]))) {
            start = cursor++;
            while (isdigit((unsigned char)*cursor)) {
                cursor++;
            }
            if (!push_token(&array, TOKEN_NUMBER, start, (size_t)(cursor - start))) goto oom;
            continue;
        }

        /*
         * identifier / keyword 처리
         * 예:
         * - users
         * - school
         * - SELECT
         */
        if (isalpha((unsigned char)*cursor) || *cursor == '_') {
            TokenType type; /* 예약어인지 일반 식별자인지 판정 결과 */

            start = cursor++;
            while (isalnum((unsigned char)*cursor) || *cursor == '_') {
                cursor++;
            }
            if (!push_token(&array, TOKEN_IDENTIFIER, start, (size_t)(cursor - start))) goto oom;
            type = keyword_type(array.items[array.count - 1].text);
            array.items[array.count - 1].type = type;
            continue;
        }

        /*
         * 위 규칙 어디에도 안 걸리면 지금 문법에서 허용하지 않는 문자다.
         */
        snprintf(status->message, sizeof(status->message), "Parse error: unexpected character '%c'", *cursor);
        free_tokens(&array);
        return array;
    }

    /* parser가 문장 끝을 쉽게 판별하도록 EOF 토큰을 항상 마지막에 붙인다. */
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

/*
 * lexer가 만든 동적 토큰 배열을 전부 해제한다.
 * 각 토큰의 text도 따로 malloc되어 있으므로 하나씩 free해야 한다.
 */
void free_tokens(TokenArray *tokens) {
    int i; /* 토큰 배열 순회용 인덱스 */

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
