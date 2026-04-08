#include "sql_processor.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

/* strdup 대체 함수다. 호출한 쪽이 반환 메모리를 free해야 한다. */
char *sp_strdup(const char *text) {
    size_t length;
    char *copy;

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

/* 문자열 앞뒤 공백을 제자리에서 잘라낸다. */
char *trim_whitespace(char *text) {
    char *end;

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

/* SQL 문자열 literal 양끝의 작은따옴표를 제거한다. */
void strip_quotes(char *text) {
    size_t length;

    if (text == NULL) {
        return;
    }

    length = strlen(text);
    if (length >= 2 && text[0] == '\'' && text[length - 1] == '\'') {
        memmove(text, text + 1, length - 2);
        text[length - 2] = '\0';
    }
}

/* SQL 키워드 비교를 위해 대소문자를 무시하고 문자열을 비교한다. */
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

/* AST 노드 하나를 생성한다. text가 있으면 heap에 복사해 보관한다. */
ASTNode *create_ast_node(NodeType type, const char *text, ASTValueType value_type) {
    ASTNode *node;

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

/* 부모 노드의 마지막 자식 뒤에 새 자식 노드를 붙인다. */
void append_child(ASTNode *parent, ASTNode *child) {
    ASTNode *cursor;

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

/* 부모 아래에서 원하는 타입의 첫 번째 자식 노드를 찾는다. */
ASTNode *find_child(ASTNode *parent, NodeType type) {
    ASTNode *cursor;

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

/* AST 전체를 자식/형제 순서로 재귀 해제한다. */
void free_ast(ASTNode *node) {
    if (node == NULL) {
        return;
    }

    free_ast(node->first_child);
    free_ast(node->next_sibling);
    free(node->text);
    free(node);
}
