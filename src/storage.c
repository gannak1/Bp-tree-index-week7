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

/* 현재 스키마가 자동 증가 id 첫 컬럼 규칙을 만족하는지 검사한다. */
static int validate_id_column(const TableMeta *meta, Status *status) {
    if (meta->column_count <= 0) {
        snprintf(status->message, sizeof(status->message), "Schema error: table has no columns");
        return 0;
    }
    if (!equals_ignore_case(meta->columns[0].name, "id")) {
        snprintf(status->message, sizeof(status->message), "Schema error: first column must be 'id'");
        return 0;
    }
    if (meta->columns[0].type != COL_INT) {
        snprintf(status->message, sizeof(status->message), "Schema error: 'id' column must be INT");
        return 0;
    }
    if (meta->columns[0].size != (int)sizeof(int)) {
        snprintf(status->message, sizeof(status->message), "Schema error: 'id' column size must be %d", (int)sizeof(int));
        return 0;
    }
    return 1;
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

/* WHERE 노드에서 비교 연산자 기반 조건을 꺼낸다. */
static int extract_comparison_where_parts(ASTNode *where_node, ASTNode **column_node, ASTNode **operator_node, ASTNode **value_node) {
    if (where_node == NULL || where_node->type != NODE_WHERE) {
        return 0;
    }

    *column_node = where_node->first_child;
    if (*column_node == NULL) {
        return 0;
    }

    *operator_node = (*column_node)->next_sibling;
    if (*operator_node == NULL || (*operator_node)->type != NODE_OPERATOR) {
        return 0;
    }

    *value_node = (*operator_node)->next_sibling;
    return *value_node != NULL;
}

/* WHERE 노드에서 BETWEEN 조건을 꺼낸다. */
static int extract_between_where_parts(ASTNode *where_node, ASTNode **column_node, ASTNode **between_node, ASTNode **lower_node, ASTNode **upper_node) {
    if (where_node == NULL || where_node->type != NODE_WHERE) {
        return 0;
    }

    *column_node = where_node->first_child;
    if (*column_node == NULL) {
        return 0;
    }

    *between_node = (*column_node)->next_sibling;
    if (*between_node == NULL || (*between_node)->type != NODE_BETWEEN) {
        return 0;
    }

    *lower_node = (*between_node)->first_child;
    if (*lower_node == NULL) {
        return 0;
    }

    *upper_node = (*lower_node)->next_sibling;
    return *upper_node != NULL;
}

/* WHERE가 정확히 id = <number> 인지 판별하고, 맞으면 target id를 꺼낸다. */
static int extract_index_search_id(ASTNode *where_node, int *target_id) {
    ASTNode *column_node;
    ASTNode *operator_node;
    ASTNode *value_node;

    if (!extract_comparison_where_parts(where_node, &column_node, &operator_node, &value_node)) {
        return 0;
    }
    if (!equals_ignore_case(column_node->text, "id")) {
        return 0;
    }
    if (strcmp(operator_node->text, "=") != 0) {
        return 0;
    }
    if (value_node->value_type != AST_VALUE_NUMBER) {
        return 0;
    }

    return parse_integer_literal(value_node->text, target_id);
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

/* row 버퍼에서 특정 컬럼의 문자열 값을 안전하게 복원한다. */
static void extract_char_value(const ColumnDef *column, const unsigned char *row, char *buffer, size_t buffer_size) {
    int copy_size;

    copy_size = column->size < (int)buffer_size - 1 ? column->size : (int)buffer_size - 1;
    memcpy(buffer, row + column->offset, (size_t)copy_size);
    buffer[copy_size] = '\0';
}

/* data 파일 전체를 스캔해 id -> row_offset B+ 트리를 재구축한다. */
int build_id_index_from_data(ExecutionContext *context, Status *status) {
    FILE *file;
    unsigned char row[MAX_ROW_SIZE];
    long offset;
    int id_value;

    if (!validate_id_column(&context->meta, status)) {
        return 0;
    }

    bptree_free(&context->id_index);
    bptree_init(&context->id_index);
    context->record_count = 0;

    file = fopen(context->meta.data_file_path, "rb");
    if (file == NULL) {
        return 1;
    }

    while (1) {
        offset = ftell(file);
        if (fread(row, 1, (size_t)context->meta.row_size, file) != (size_t)context->meta.row_size) {
            if (feof(file)) {
                break;
            }
            fclose(file);
            snprintf(status->message, sizeof(status->message), "Execution error: failed to rebuild id index from '%s'", context->meta.data_file_path);
            return 0;
        }

        memcpy(&id_value, row + context->meta.columns[0].offset, sizeof(int));
        if (!bptree_insert(&context->id_index, id_value, offset, status)) {
            fclose(file);
            return 0;
        }
        context->record_count++;
    }

    fclose(file);
    return 1;
}

/* INSERT 루트 노드 아래 VALUE_LIST를 읽어 .dat 파일 끝에 새 row를 추가한다. */
int append_binary_row(ExecutionContext *context, ASTNode *root, int *inserted_id, Status *status) {
    unsigned char row[MAX_ROW_SIZE];
    FILE *file;
    ASTNode *value_list;
    ASTNode *value_node;
    long row_offset;
    int next_id;
    int i;

    value_list = find_child(root, NODE_VALUE_LIST);
    if (value_list == NULL) {
        snprintf(status->message, sizeof(status->message), "Parse error: value list is missing");
        return 0;
    }
    if (!validate_id_column(&context->meta, status)) {
        return 0;
    }

    memset(row, 0, sizeof(row));
    next_id = context->record_count + 1;
    memcpy(row + context->meta.columns[0].offset, &next_id, sizeof(int));

    value_node = value_list->first_child;
    for (i = 1; i < context->meta.column_count; i++) {
        if (value_node == NULL) {
            snprintf(status->message, sizeof(status->message), "Execution error: expected %d values but got fewer", context->meta.column_count - 1);
            return 0;
        }
        if (!write_value(row, &context->meta.columns[i], value_node, status)) {
            return 0;
        }
        value_node = value_node->next_sibling;
    }

    if (value_node != NULL) {
        snprintf(status->message, sizeof(status->message), "Execution error: expected %d values but got more", context->meta.column_count - 1);
        return 0;
    }

    if (!ensure_parent_directory(context->meta.data_file_path, status)) {
        return 0;
    }

    row_offset = (long)context->record_count * context->meta.row_size;
    file = fopen(context->meta.data_file_path, "ab");
    if (file == NULL) {
        snprintf(status->message, sizeof(status->message), "Execution error: cannot open '%s' for append", context->meta.data_file_path);
        return 0;
    }

    if (fwrite(row, 1, (size_t)context->meta.row_size, file) != (size_t)context->meta.row_size) {
        fclose(file);
        snprintf(status->message, sizeof(status->message), "Execution error: failed to write row");
        return 0;
    }

    fclose(file);

    if (!bptree_insert(&context->id_index, next_id, row_offset, status)) {
        return 0;
    }

    context->record_count++;
    if (inserted_id != NULL) {
        *inserted_id = next_id;
    }
    return 1;
}

/* 현재 row가 WHERE 조건을 만족하는지 검사한다. WHERE가 없으면 항상 참이다. */
static int row_matches_where(const TableMeta *meta, const unsigned char *row, ASTNode *where_node, Status *status) {
    ASTNode *column_node;
    ASTNode *operator_node;
    ASTNode *value_node;
    ASTNode *between_node;
    ASTNode *lower_node;
    ASTNode *upper_node;
    const ColumnDef *column;
    int index;

    if (where_node == NULL) {
        return 1;
    }

    if (extract_between_where_parts(where_node, &column_node, &between_node, &lower_node, &upper_node)) {
        int actual;
        int lower;
        int upper;

        index = find_column_index(meta, column_node->text);
        if (index < 0) {
            snprintf(status->message, sizeof(status->message), "Schema error: column '%s' does not exist", column_node->text);
            return -1;
        }

        column = &meta->columns[index];
        if (column->type != COL_INT) {
            snprintf(status->message, sizeof(status->message), "Execution error: BETWEEN for column '%s' supports INT only", column->name);
            return -1;
        }
        if (lower_node->value_type != AST_VALUE_NUMBER || !parse_integer_literal(lower_node->text, &lower)) {
            snprintf(status->message, sizeof(status->message), "Execution error: BETWEEN lower bound for '%s' must be INT", column->name);
            return -1;
        }
        if (upper_node->value_type != AST_VALUE_NUMBER || !parse_integer_literal(upper_node->text, &upper)) {
            snprintf(status->message, sizeof(status->message), "Execution error: BETWEEN upper bound for '%s' must be INT", column->name);
            return -1;
        }

        memcpy(&actual, row + column->offset, sizeof(int));
        return lower <= actual && actual <= upper;
    }

    if (!extract_comparison_where_parts(where_node, &column_node, &operator_node, &value_node)) {
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

        extract_char_value(column, row, actual, sizeof(actual));
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

        extract_char_value(column, row, value, sizeof(value));
        printf("%s", value);
    }
}

/* 선택 컬럼 헤더를 공통 형식으로 출력한다. */
static void print_selected_header(const TableMeta *meta, const int *selected_indexes, int selected_count) {
    int i;

    for (i = 0; i < selected_count; i++) {
        if (i > 0) {
            printf(" | ");
        }
        printf("%s", meta->columns[selected_indexes[i]].name);
    }
    printf("\n");
}

/* 선택 컬럼 순서대로 row 하나를 출력한다. */
static void print_selected_row(const TableMeta *meta, const unsigned char *row, const int *selected_indexes, int selected_count) {
    int i;

    for (i = 0; i < selected_count; i++) {
        if (i > 0) {
            printf(" | ");
        }
        print_column_value(&meta->columns[selected_indexes[i]], row);
    }
    printf("\n");
}

/* id = ? 조건이면 B+ 트리에서 row offset을 찾아 row 하나만 읽어 출력한다. */
static int select_by_id_index(const ExecutionContext *context, int target_id, const int *selected_indexes, int selected_count, Status *status) {
    FILE *file;
    unsigned char row[MAX_ROW_SIZE];
    long offset;

    if (!bptree_search(&context->id_index, target_id, &offset)) {
        printf("0 rows selected.\n");
        return 1;
    }

    file = fopen(context->meta.data_file_path, "rb");
    if (file == NULL) {
        snprintf(status->message, sizeof(status->message), "Table error: table '%s.%s' data file not found", context->meta.schema_name, context->meta.table_name);
        return 0;
    }

    if (fseek(file, offset, SEEK_SET) != 0) {
        fclose(file);
        snprintf(status->message, sizeof(status->message), "Execution error: failed to seek row offset %ld", offset);
        return 0;
    }
    if (fread(row, 1, (size_t)context->meta.row_size, file) != (size_t)context->meta.row_size) {
        fclose(file);
        snprintf(status->message, sizeof(status->message), "Execution error: failed to read indexed row");
        return 0;
    }

    fclose(file);
    print_selected_row(&context->meta, row, selected_indexes, selected_count);
    printf("1 rows selected.\n");
    return 1;
}

/* SELECT를 실행하면서 id = ? 는 인덱스를 쓰고, 나머지는 순차 탐색한다. */
int execute_select(ExecutionContext *context, ASTNode *root, Status *status) {
    FILE *file;
    unsigned char row[MAX_ROW_SIZE];
    int selected_indexes[MAX_COLUMNS];
    int selected_count = 0;
    int rows_selected = 0;
    int indexed_id = 0;
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
        for (i = 0; i < context->meta.column_count; i++) {
            selected_indexes[selected_count++] = i;
        }
    } else {
        column_node = column_list->first_child;
        while (column_node != NULL) {
            int index = find_column_index(&context->meta, column_node->text);
            if (index < 0) {
                snprintf(status->message, sizeof(status->message), "Schema error: column '%s' does not exist", column_node->text);
                return 0;
            }
            selected_indexes[selected_count++] = index;
            column_node = column_node->next_sibling;
        }
    }

    print_selected_header(&context->meta, selected_indexes, selected_count);

    if (extract_index_search_id(where_node, &indexed_id)) {
        context->last_execution_path = EXECUTION_PATH_INDEXED;
        return select_by_id_index(context, indexed_id, selected_indexes, selected_count, status);
    }

    context->last_execution_path = EXECUTION_PATH_FULL_SCAN;

    file = fopen(context->meta.data_file_path, "rb");
    if (file == NULL) {
        snprintf(status->message, sizeof(status->message), "Table error: table '%s.%s' data file not found", context->meta.schema_name, context->meta.table_name);
        return 0;
    }

    while (fread(row, 1, (size_t)context->meta.row_size, file) == (size_t)context->meta.row_size) {
        int matches = row_matches_where(&context->meta, row, where_node, status);
        if (matches < 0) {
            fclose(file);
            return 0;
        }
        if (!matches) {
            continue;
        }

        print_selected_row(&context->meta, row, selected_indexes, selected_count);
        rows_selected++;
    }

    fclose(file);
    printf("%d rows selected.\n", rows_selected);
    return 1;
}
