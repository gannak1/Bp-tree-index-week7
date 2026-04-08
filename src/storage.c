#include "sql_processor.h"

#include <stdlib.h>
#include <string.h>

/* 컬럼 이름으로 TableMeta 내부 컬럼 인덱스를 찾는다. */
static int find_column_index(const TableMeta *meta, const char *name) {
    int i;
    for (i = 0; i < meta->column_count; i++) {
        if (equals_ignore_case(meta->columns[i].name, name)) {
            return i;
        }
    }
    return -1;
}

/* VALUE 노드의 문자열을 실제 int 값으로 바꾼다. */
static int parse_integer_literal(const char *text, int *value) {
    char *end;
    long parsed = strtol(text, &end, 10);
    if (*text == '\0' || *end != '\0') {
        return 0;
    }
    *value = (int)parsed;
    return 1;
}

/* COLUMN_LIST 아래 첫 번째 컬럼이 '*' 인지 확인한다. */
static int is_select_all(ASTNode *column_list) {
    ASTNode *first_column;
    if (column_list == NULL) {
        return 0;
    }
    first_column = column_list->first_child;
    return first_column != NULL && first_column->type == NODE_COLUMN && strcmp(first_column->text, "*") == 0;
}

/* WHERE 노드에서 컬럼, 연산자, 값 노드를 차례대로 꺼낸다. */
static int extract_where_parts(ASTNode *where_node, ASTNode **column_node, ASTNode **operator_node, ASTNode **value_node) {
    if (where_node == NULL || where_node->type != NODE_WHERE) {
        return 0;
    }
    *column_node = where_node->first_child;
    if (*column_node == NULL) {
        return 0;
    }
    *operator_node = (*column_node)->next_sibling;
    if (*operator_node == NULL) {
        return 0;
    }
    *value_node = (*operator_node)->next_sibling;
    return *value_node != NULL;
}

/* 정수 비교 연산을 문자열 연산자에 맞게 수행한다. */
static int compare_ints(int left, int right, const char *op) {
    if (strcmp(op, "=") == 0) return left == right;
    if (strcmp(op, "!=") == 0) return left != right;
    if (strcmp(op, ">") == 0) return left > right;
    if (strcmp(op, ">=") == 0) return left >= right;
    if (strcmp(op, "<") == 0) return left < right;
    if (strcmp(op, "<=") == 0) return left <= right;
    return 0;
}

/* 문자열 비교 연산을 문자열 연산자에 맞게 수행한다. */
static int compare_strings(const char *left, const char *right, const char *op) {
    int cmp = strcmp(left, right);
    if (strcmp(op, "=") == 0) return cmp == 0;
    if (strcmp(op, "!=") == 0) return cmp != 0;
    if (strcmp(op, ">") == 0) return cmp > 0;
    if (strcmp(op, ">=") == 0) return cmp >= 0;
    if (strcmp(op, "<") == 0) return cmp < 0;
    if (strcmp(op, "<=") == 0) return cmp <= 0;
    return 0;
}

/* AST VALUE 노드 하나를 바이너리 row 버퍼의 올바른 위치에 기록한다. */
static int write_value(unsigned char *buffer, const ColumnDef *column, ASTNode *value_node, Status *status) {
    if (column->type == COL_INT) {
        int parsed;
        if (value_node->value_type != AST_VALUE_NUMBER || !parse_integer_literal(value_node->text, &parsed)) {
            snprintf(status->message, sizeof(status->message), "Execution error: column '%s' expects INT", column->name);
            return 0;
        }
        memcpy(buffer + column->offset, &parsed, sizeof(int));
        return 1;
    }

    if (column->type == COL_CHAR) {
        char temp[256];
        size_t length;

        snprintf(temp, sizeof(temp), "%s", value_node->text);
        if (value_node->value_type == AST_VALUE_STRING) {
            strip_quotes(temp);
        }

        length = strlen(temp);
        if ((int)length > column->size) {
            snprintf(status->message, sizeof(status->message), "Execution error: column '%s' exceeds CHAR(%d)", column->name, column->size);
            return 0;
        }
        memset(buffer + column->offset, 0, (size_t)column->size);
        memcpy(buffer + column->offset, temp, length);
        return 1;
    }

    snprintf(status->message, sizeof(status->message), "Schema error: unsupported column type");
    return 0;
}

/* INSERT 루트 노드 아래 VALUE_LIST를 읽어 .dat 파일 끝에 새 row를 추가한다. */
int append_binary_row(const TableMeta *meta, ASTNode *root, Status *status) {
    unsigned char row[MAX_ROW_SIZE];
    FILE *file;
    ASTNode *value_list;
    ASTNode *value_node;
    int i;

    value_list = find_child(root, NODE_VALUE_LIST);
    if (value_list == NULL) {
        snprintf(status->message, sizeof(status->message), "Parse error: value list is missing");
        return 0;
    }

    memset(row, 0, sizeof(row));
    value_node = value_list->first_child;
    for (i = 0; i < meta->column_count; i++) {
        if (value_node == NULL) {
            snprintf(status->message, sizeof(status->message), "Execution error: expected %d values but got fewer", meta->column_count);
            return 0;
        }
        if (!write_value(row, &meta->columns[i], value_node, status)) {
            return 0;
        }
        value_node = value_node->next_sibling;
    }

    if (value_node != NULL) {
        snprintf(status->message, sizeof(status->message), "Execution error: expected %d values but got more", meta->column_count);
        return 0;
    }

    if (!ensure_parent_directory(meta->data_file_path, status)) {
        return 0;
    }

    file = fopen(meta->data_file_path, "ab");
    if (file == NULL) {
        snprintf(status->message, sizeof(status->message), "Execution error: cannot open '%s' for append", meta->data_file_path);
        return 0;
    }

    if (fwrite(row, 1, (size_t)meta->row_size, file) != (size_t)meta->row_size) {
        fclose(file);
        snprintf(status->message, sizeof(status->message), "Execution error: failed to write row");
        return 0;
    }

    fclose(file);
    return 1;
}

/* 현재 row가 WHERE 조건을 만족하는지 검사한다. WHERE가 없으면 항상 참이다. */
static int row_matches_where(const TableMeta *meta, const unsigned char *row, ASTNode *where_node, Status *status) {
    ASTNode *column_node;
    ASTNode *operator_node;
    ASTNode *value_node;
    const ColumnDef *column;
    int index;

    if (where_node == NULL) {
        return 1;
    }

    if (!extract_where_parts(where_node, &column_node, &operator_node, &value_node)) {
        snprintf(status->message, sizeof(status->message), "Parse error: malformed WHERE clause");
        return -1;
    }

    index = find_column_index(meta, column_node->text);
    if (index < 0) {
        snprintf(status->message, sizeof(status->message), "Schema error: column '%s' does not exist", column_node->text);
        return -1;
    }

    column = &meta->columns[index];
    if (column->type == COL_INT) {
        int actual;
        int expected;
        memcpy(&actual, row + column->offset, sizeof(int));
        if (value_node->value_type != AST_VALUE_NUMBER || !parse_integer_literal(value_node->text, &expected)) {
            snprintf(status->message, sizeof(status->message), "Execution error: WHERE value for '%s' must be INT", column->name);
            return -1;
        }
        return compare_ints(actual, expected, operator_node->text);
    }

    if (column->type == COL_CHAR) {
        char actual[256];
        char expected[256];
        int copy_size = column->size < (int)sizeof(actual) - 1 ? column->size : (int)sizeof(actual) - 1;
        memcpy(actual, row + column->offset, (size_t)copy_size);
        actual[copy_size] = '\0';
        snprintf(expected, sizeof(expected), "%s", value_node->text);
        strip_quotes(expected);
        return compare_strings(actual, expected, operator_node->text);
    }

    snprintf(status->message, sizeof(status->message), "Schema error: unsupported column type");
    return -1;
}

/* 현재 row에서 지정한 컬럼 값을 꺼내 사람이 읽을 수 있게 출력한다. */
static void print_column_value(const ColumnDef *column, const unsigned char *row) {
    if (column->type == COL_INT) {
        int value;
        memcpy(&value, row + column->offset, sizeof(int));
        printf("%d", value);
        return;
    }

    if (column->type == COL_CHAR) {
        char value[256];
        int copy_size = column->size < (int)sizeof(value) - 1 ? column->size : (int)sizeof(value) - 1;
        memcpy(value, row + column->offset, (size_t)copy_size);
        value[copy_size] = '\0';
        printf("%s", value);
    }
}

/* SELECT를 실행하면서 data 파일을 row_size 단위로 순차 탐색한다. */
int execute_select(const TableMeta *meta, ASTNode *root, Status *status) {
    FILE *file;
    unsigned char row[MAX_ROW_SIZE];
    int selected_indexes[MAX_COLUMNS];
    int selected_count = 0;
    int rows_selected = 0;
    ASTNode *column_list;
    ASTNode *where_node;
    ASTNode *column_node;
    int i;

    column_list = find_child(root, NODE_COLUMN_LIST);
    where_node = find_child(root, NODE_WHERE);
    if (column_list == NULL) {
        snprintf(status->message, sizeof(status->message), "Parse error: column list is missing");
        return 0;
    }

    if (is_select_all(column_list)) {
        for (i = 0; i < meta->column_count; i++) {
            selected_indexes[selected_count++] = i;
        }
    } else {
        column_node = column_list->first_child;
        while (column_node != NULL) {
            int index = find_column_index(meta, column_node->text);
            if (index < 0) {
                snprintf(status->message, sizeof(status->message), "Schema error: column '%s' does not exist", column_node->text);
                return 0;
            }
            selected_indexes[selected_count++] = index;
            column_node = column_node->next_sibling;
        }
    }

    file = fopen(meta->data_file_path, "rb");
    if (file == NULL) {
        snprintf(status->message, sizeof(status->message), "Table error: table '%s.%s' data file not found", meta->schema_name, meta->table_name);
        return 0;
    }

    for (i = 0; i < selected_count; i++) {
        if (i > 0) {
            printf(" | ");
        }
        printf("%s", meta->columns[selected_indexes[i]].name);
    }
    printf("\n");

    while (fread(row, 1, (size_t)meta->row_size, file) == (size_t)meta->row_size) {
        int matches = row_matches_where(meta, row, where_node, status);
        if (matches < 0) {
            fclose(file);
            return 0;
        }
        if (!matches) {
            continue;
        }

        for (i = 0; i < selected_count; i++) {
            if (i > 0) {
                printf(" | ");
            }
            print_column_value(&meta->columns[selected_indexes[i]], row);
        }
        printf("\n");
        rows_selected++;
    }

    fclose(file);
    printf("%d rows selected.\n", rows_selected);
    return 1;
}
