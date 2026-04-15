#include "sql_processor.h"

#include <stdlib.h>
#include <string.h>

/*
 * TableMeta 안에서 컬럼 이름으로 컬럼 인덱스를 찾는다.
 *
 * 반환:
 * - 성공: 0 이상 컬럼 인덱스
 * - 실패: -1
 *
 * 어디에 쓰나:
 * - SELECT 컬럼 해석
 * - WHERE 컬럼 해석
 */
static int find_column_index(const TableMeta *meta, const char *name) {
    int i; /* columns 배열 순회 인덱스 */

    for (i = 0; i < meta->column_count; i++) {
        if (equals_ignore_case(meta->columns[i].name, name)) {
            return i;
        }
    }
    return -1;
}

/*
 * 현재 스키마가 자동 증가 id 첫 컬럼 규칙을 만족하는지 검사한다.
 *
 * 규칙:
 * - 컬럼이 최소 1개 있어야 한다.
 * - 첫 컬럼 이름은 id 여야 한다.
 * - 첫 컬럼 타입은 INT 여야 한다.
 * - 첫 컬럼 size는 sizeof(int) 여야 한다.
 */
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

/*
 * 숫자 문자열을 실제 int 값으로 바꾼다.
 *
 * 반환:
 * - 성공: 1, value에 결과 저장
 * - 실패: 0
 *
 * 주의:
 * - 문자열 전체가 숫자여야 성공으로 본다.
 * - "123abc" 같은 값은 실패 처리한다.
 */
static int parse_integer_literal(const char *text, int *value) {
    char *end;   /* strtol이 숫자 해석을 멈춘 위치 */
    long parsed = strtol(text, &end, 10); /* 임시 long 결과 */

    if (*text == '\0' || *end != '\0') {
        return 0;
    }

    *value = (int)parsed;
    return 1;
}

/*
 * COLUMN_LIST 아래 첫 컬럼이 '*' 인지 확인한다.
 * SELECT * 구문인지 판단할 때 사용한다.
 */
static int is_select_all(ASTNode *column_list) {
    ASTNode *first_column; /* COLUMN_LIST의 첫 번째 자식 */

    if (column_list == NULL) {
        return 0;
    }

    first_column = column_list->first_child;
    return first_column != NULL && first_column->type == NODE_COLUMN && strcmp(first_column->text, "*") == 0;
}

/*
 * WHERE 노드에서 일반 비교식 구조를 꺼낸다.
 *
 * 기대 구조:
 * NODE_WHERE
 * ├── NODE_COLUMN
 * ├── NODE_OPERATOR
 * └── NODE_VALUE
 */
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

/*
 * WHERE 노드에서 BETWEEN 구조를 꺼낸다.
 *
 * 기대 구조:
 * NODE_WHERE
 * ├── NODE_COLUMN
 * └── NODE_BETWEEN
 *     ├── NODE_VALUE(lower)
 *     └── NODE_VALUE(upper)
 */
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

/*
 * WHERE가 정확히 "id = <number>"인지 판정한다.
 *
 * 어디에 쓰나:
 * - 이 조건을 만족하면 B+ 트리 인덱스를 사용해 한 번에 row offset을 찾는다.
 */
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

/* 정수 비교 연산자 문자열을 실제 비교식으로 수행한다. */
static int compare_ints(int left, int right, const char *op) {
    if (strcmp(op, "=") == 0) return left == right;
    if (strcmp(op, "!=") == 0) return left != right;
    if (strcmp(op, ">") == 0) return left > right;
    if (strcmp(op, ">=") == 0) return left >= right;
    if (strcmp(op, "<") == 0) return left < right;
    if (strcmp(op, "<=") == 0) return left <= right;
    return 0;
}

/* 문자열 비교 연산자 문자열을 실제 strcmp 결과에 맞게 수행한다. */
static int compare_strings(const char *left, const char *right, const char *op) {
    int cmp = strcmp(left, right); /* strcmp 결과: 음수/0/양수 */

    if (strcmp(op, "=") == 0) return cmp == 0;
    if (strcmp(op, "!=") == 0) return cmp != 0;
    if (strcmp(op, ">") == 0) return cmp > 0;
    if (strcmp(op, ">=") == 0) return cmp >= 0;
    if (strcmp(op, "<") == 0) return cmp < 0;
    if (strcmp(op, "<=") == 0) return cmp <= 0;
    return 0;
}

/*
 * AST VALUE 노드 하나를 바이너리 row 버퍼 안의 특정 컬럼 위치에 기록한다.
 *
 * 입력:
 * - buffer: 최종 row를 만들 바이트 배열
 * - column: 지금 기록할 컬럼 메타정보
 * - value_node: AST에서 읽은 값 노드
 *
 * 동작:
 * - INT면 숫자 파싱 후 memcpy
 * - CHAR면 문자열 길이 검사 후 zero-fill + memcpy
 */
static int write_value(unsigned char *buffer, const ColumnDef *column, ASTNode *value_node, Status *status) {
    if (column->type == COL_INT) {
        int parsed; /* 문자열 숫자를 실제 int로 변환한 값 */

        if (value_node->value_type != AST_VALUE_NUMBER || !parse_integer_literal(value_node->text, &parsed)) {
            snprintf(status->message, sizeof(status->message), "Execution error: column '%s' expects INT", column->name);
            return 0;
        }

        memcpy(buffer + column->offset, &parsed, sizeof(int));
        return 1;
    }

    if (column->type == COL_CHAR) {
        char temp[256];  /* strip_quotes용 임시 문자열 버퍼 */
        size_t length;   /* 실제 문자열 길이 */

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

/*
 * row 버퍼에서 CHAR 컬럼 값을 안전하게 문자열로 복원한다.
 *
 * 입력:
 * - column: 읽을 CHAR 컬럼 메타
 * - row: 현재 row 바이트 배열
 * - buffer: 결과 문자열을 쓸 출력 버퍼
 * - buffer_size: 출력 버퍼 크기
 */
static void extract_char_value(const ColumnDef *column, const unsigned char *row, char *buffer, size_t buffer_size) {
    int copy_size; /* 버퍼 크기와 컬럼 크기를 고려한 실제 복사 길이 */

    copy_size = column->size < (int)buffer_size - 1 ? column->size : (int)buffer_size - 1;
    memcpy(buffer, row + column->offset, (size_t)copy_size);
    buffer[copy_size] = '\0';
}

/*
 * data 파일 전체를 스캔해서 id -> row_offset B+ 트리를 재구축한다.
 *
 * 실행 시점:
 * - 프로그램 시작 후 첫 테이블 사용 시
 * - 다른 테이블로 컨텍스트가 바뀔 때
 *
 * 결과:
 * - context->id_index 채워짐
 * - context->record_count 갱신
 */
int build_id_index_from_data(ExecutionContext *context, Status *status) {
    FILE *file;                  /* data 파일 핸들 */
    unsigned char row[MAX_ROW_SIZE]; /* 파일에서 읽어올 row 버퍼 */
    long offset;                 /* 현재 row의 파일 시작 위치 */
    int id_value;                /* 현재 row에서 읽어낸 id 값 */

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

/*
 * INSERT 루트 노드 아래 VALUE_LIST를 읽어 .dat 파일 끝에 새 row를 추가한다.
 *
 * 규칙:
 * - id는 사용자가 직접 넣지 않는다.
 * - next_id = record_count + 1
 * - 새 row를 파일에 append한 뒤 B+ 트리에도 같은 id를 삽입한다.
 */
int append_binary_row(ExecutionContext *context, ASTNode *root, int *inserted_id, Status *status) {
    unsigned char row[MAX_ROW_SIZE]; /* 최종 작성할 row 바이트 버퍼 */
    FILE *file;                      /* append용 data 파일 핸들 */
    ASTNode *value_list;             /* INSERT 아래 VALUE_LIST 노드 */
    ASTNode *value_node;             /* 현재 쓰고 있는 VALUE 노드 */
    long row_offset;                 /* 새 row가 저장될 파일 위치 */
    int next_id;                     /* 자동 증가로 부여할 새 id */
    int i;                           /* 컬럼 순회 인덱스 */

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

/*
 * 현재 row가 WHERE 조건을 만족하는지 검사한다.
 *
 * 반환:
 * - 1: 조건 만족
 * - 0: 조건 불만족
 * - -1: 실행 중 오류
 *
 * 지원:
 * - 일반 비교 연산
 * - BETWEEN (INT 전용)
 */
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
        int actual; /* row에서 읽은 실제 컬럼 값 */
        int lower;  /* BETWEEN 하한 */
        int upper;  /* BETWEEN 상한 */

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
        int actual;   /* row에서 읽은 실제 정수 값 */
        int expected; /* WHERE 오른쪽의 기대 정수 값 */

        memcpy(&actual, row + column->offset, sizeof(int));
        if (value_node->value_type != AST_VALUE_NUMBER || !parse_integer_literal(value_node->text, &expected)) {
            snprintf(status->message, sizeof(status->message), "Execution error: WHERE value for '%s' must be INT", column->name);
            return -1;
        }
        return compare_ints(actual, expected, operator_node->text);
    }

    if (column->type == COL_CHAR) {
        char actual[256];   /* row에서 복원한 실제 문자열 */
        char expected[256]; /* WHERE 오른쪽 문자열 */

        extract_char_value(column, row, actual, sizeof(actual));
        snprintf(expected, sizeof(expected), "%s", value_node->text);
        strip_quotes(expected);
        return compare_strings(actual, expected, operator_node->text);
    }

    snprintf(status->message, sizeof(status->message), "Schema error: unsupported column type");
    return -1;
}

/*
 * 현재 row에서 특정 컬럼 값을 사람 읽기 좋은 형태로 출력한다.
 * INT면 숫자로, CHAR면 문자열로 출력한다.
 */
static void print_column_value(const ColumnDef *column, const unsigned char *row) {
    if (column->type == COL_INT) {
        int value; /* row에서 읽어낸 정수 값 */

        memcpy(&value, row + column->offset, sizeof(int));
        printf("%d", value);
        return;
    }

    if (column->type == COL_CHAR) {
        char value[256]; /* row에서 복원한 문자열 값 */

        extract_char_value(column, row, value, sizeof(value));
        printf("%s", value);
    }
}

/* SELECT 결과 헤더를 "col1 | col2 | ..." 형식으로 출력한다. */
static void print_selected_header(const TableMeta *meta, const int *selected_indexes, int selected_count) {
    int i; /* 선택된 컬럼 배열 순회 인덱스 */

    for (i = 0; i < selected_count; i++) {
        if (i > 0) {
            printf(" | ");
        }
        printf("%s", meta->columns[selected_indexes[i]].name);
    }
    printf("\n");
}

/* SELECT 결과 row 하나를 "value1 | value2 | ..." 형식으로 출력한다. */
static void print_selected_row(const TableMeta *meta, const unsigned char *row, const int *selected_indexes, int selected_count) {
    int i; /* 선택된 컬럼 배열 순회 인덱스 */

    for (i = 0; i < selected_count; i++) {
        if (i > 0) {
            printf(" | ");
        }
        print_column_value(&meta->columns[selected_indexes[i]], row);
    }
    printf("\n");
}

/*
 * id = ? 조건일 때 B+ 트리에서 row offset을 찾아 row 하나만 읽어 출력한다.
 *
 * 이 함수가 바로 "인덱스 경로"의 실제 본체다.
 */
static int select_by_id_index(const ExecutionContext *context, int target_id, const int *selected_indexes, int selected_count, Status *status) {
    FILE *file;                  /* data 파일 핸들 */
    unsigned char row[MAX_ROW_SIZE]; /* 인덱스로 찾은 row 하나를 읽을 버퍼 */
    long offset;                 /* B+ 트리가 돌려준 row 시작 위치 */

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

/*
 * SELECT를 실행한다.
 *
 * 흐름:
 * 1. COLUMN_LIST를 해석해 어떤 컬럼을 출력할지 결정
 * 2. WHERE가 id = number 형태면 B+ 트리 인덱스 경로 사용
 * 3. 아니면 data 파일 전체를 row_size 단위로 선형 탐색
 */
int execute_select(ExecutionContext *context, ASTNode *root, Status *status) {
    FILE *file;                        /* 선형 탐색용 data 파일 핸들 */
    unsigned char row[MAX_ROW_SIZE];  /* 파일에서 읽은 현재 row 버퍼 */
    int selected_indexes[MAX_COLUMNS];/* 출력 대상 컬럼 인덱스 목록 */
    int selected_count = 0;            /* 실제 선택된 컬럼 수 */
    int rows_selected = 0;             /* 조건을 만족한 row 수 */
    int indexed_id = 0;                /* id 인덱스 경로에서 찾을 대상 id */
    ASTNode *column_list;              /* SELECT 아래 COLUMN_LIST 노드 */
    ASTNode *where_node;               /* SELECT 아래 WHERE 노드 */
    ASTNode *column_node;              /* COLUMN_LIST 순회용 포인터 */
    int i;                             /* 컬럼 배열 순회 인덱스 */

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
