#include "sql_processor.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

/*
 * strdup 대체 함수다.
 *
 * 입력:
 * - text: 복사할 원본 문자열
 *
 * 반환:
 * - 성공: heap에 새로 할당된 문자열 포인터
 * - 실패: NULL
 *
 * 어디에 쓰나:
 * - lexer가 토큰 text를 저장할 때
 * - parser / AST가 노드 text를 따로 보관할 때
 *
 * 주의:
 * - 반환된 메모리는 호출한 쪽에서 free해야 한다.
 */
char *sp_strdup(const char *text) {
    size_t length; /* 원본 문자열 길이 */
    char *copy;    /* heap에 새로 만들 복사본 포인터 */

    if (text == NULL) {
        return NULL;
    }

    length = strlen(text);
    copy = (char *)malloc(length + 1);
    if (copy == NULL) {
        return NULL;
    }

    memcpy(copy, text, length + 1);
    return copy;
}

/*
 * 문자열 앞뒤 공백을 제자리에서 잘라낸다.
 *
 * 입력:
 * - text: 수정 가능한 문자열 버퍼
 *
 * 반환:
 * - 앞쪽 공백을 건너뛴 시작 위치 포인터
 *
 * 어디에 쓰나:
 * - SQL 파일에서 한 줄을 읽은 뒤 빈칸 정리
 * - CSV 메타 파일에서 column/type/size 값 정리
 */
char *trim_whitespace(char *text) {
    char *end; /* 뒤쪽 공백을 지울 때 사용할 끝 포인터 */

    while (*text != '\0' && isspace((unsigned char)*text)) {
        text++;
    }

    if (*text == '\0') {
        return text;
    }

    end = text + strlen(text) - 1;
    while (end > text && isspace((unsigned char)*end)) {
        *end = '\0';
        end--;
    }

    return text;
}

/*
 * SQL 문자열 literal 양끝의 작은따옴표를 제거한다.
 *
 * 예:
 * - 입력:  'Kim'
 * - 결과:  Kim
 *
 * 어디에 쓰나:
 * - AST VALUE 노드의 text를 실제 CHAR 컬럼 값으로 쓸 때
 * - WHERE 문자열 비교 전에 순수 문자열만 추출할 때
 */
void strip_quotes(char *text) {
    size_t length; /* 현재 문자열 길이 */

    if (text == NULL) {
        return;
    }

    length = strlen(text);
    if (length >= 2 && text[0] == '\'' && text[length - 1] == '\'') {
        memmove(text, text + 1, length - 2);
        text[length - 2] = '\0';
    }
}

/*
 * 대소문자를 무시하고 두 문자열이 같은지 비교한다.
 *
 * 어디에 쓰나:
 * - SQL 키워드 판별
 * - schema/table/column 이름 비교
 * - 메타 타입 문자열(INT, CHAR) 판별
 */
int equals_ignore_case(const char *left, const char *right) {
    while (*left != '\0' && *right != '\0') {
        if (toupper((unsigned char)*left) != toupper((unsigned char)*right)) {
            return 0;
        }
        left++;
        right++;
    }

    return *left == '\0' && *right == '\0';
}

/*
 * AST 노드 하나를 생성한다.
 *
 * 입력:
 * - type: 만들 노드 종류
 * - text: 노드가 보관할 문자열. NULL이면 text 없는 구조 노드
 * - value_type: literal 값 종류
 *
 * 내부 동작:
 * - ASTNode 자체를 malloc
 * - text가 있으면 sp_strdup로 복사본을 만들어 소유권을 노드가 가져감
 * - child/sibling 포인터는 NULL로 초기화
 *
 * 반환:
 * - 성공: 새 ASTNode 포인터
 * - 실패: NULL
 */
ASTNode *create_ast_node(NodeType type, const char *text, ASTValueType value_type) {
    ASTNode *node; /* 새로 만들 AST 노드 */

    node = (ASTNode *)malloc(sizeof(ASTNode));
    if (node == NULL) {
        return NULL;
    }

    node->type = type;
    node->value_type = value_type;
    node->text = text != NULL ? sp_strdup(text) : NULL;
    if (text != NULL && node->text == NULL) {
        free(node);
        return NULL;
    }
    node->first_child = NULL;
    node->next_sibling = NULL;
    return node;
}

/*
 * 부모 노드 아래에 자식 노드를 하나 붙인다.
 *
 * 동작 방식:
 * - 자식이 없으면 first_child에 바로 연결
 * - 이미 자식이 있으면 마지막 sibling 뒤에 연결
 *
 * 어디에 쓰나:
 * - parser가 SELECT / INSERT / WHERE / BETWEEN AST를 조립할 때
 */
void append_child(ASTNode *parent, ASTNode *child) {
    ASTNode *cursor; /* 마지막 형제 노드를 찾기 위한 순회 포인터 */

    if (parent == NULL || child == NULL) {
        return;
    }

    if (parent->first_child == NULL) {
        parent->first_child = child;
        return;
    }

    cursor = parent->first_child;
    while (cursor->next_sibling != NULL) {
        cursor = cursor->next_sibling;
    }
    cursor->next_sibling = child;
}

/*
 * 부모 노드 아래에서 원하는 타입의 첫 번째 자식 노드를 찾는다.
 *
 * 어디에 쓰나:
 * - executor가 TABLE / WHERE / COLUMN_LIST / VALUE_LIST 노드를 찾을 때
 */
ASTNode *find_child(ASTNode *parent, NodeType type) {
    ASTNode *cursor; /* 자식 목록을 순회하는 포인터 */

    if (parent == NULL) {
        return NULL;
    }

    cursor = parent->first_child;
    while (cursor != NULL) {
        if (cursor->type == type) {
            return cursor;
        }
        cursor = cursor->next_sibling;
    }
    return NULL;
}

/*
 * AST 전체를 재귀적으로 해제한다.
 *
 * 해제 순서:
 * - first_child
 * - next_sibling
 * - node->text
 * - node 자신
 *
 * 즉 트리 전체의 소유 메모리를 한 번에 정리하는 함수다.
 */
void free_ast(ASTNode *node) {
    if (node == NULL) {
        return;
    }

    free_ast(node->first_child);
    free_ast(node->next_sibling);
    free(node->text);
    free(node);
}
