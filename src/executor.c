#include "sql_processor.h"

#include <string.h>

/*
 * 전역 실행 컨텍스트다.
 *
 * 왜 전역으로 두나:
 * - 같은 프로세스 안에서 같은 테이블을 여러 번 질의할 때
 *   매번 id 인덱스를 다시 만들지 않기 위해서다.
 * - REPL에서 여러 SQL을 이어서 실행할 때 특히 효과가 있다.
 */
static ExecutionContext g_context;

/*
 * 실행 경로 enum을 사람이 읽을 수 있는 짧은 문자열로 바꾼다.
 * timing log와 디버깅 출력에서 쓴다.
 */
const char *execution_path_to_text(ExecutionPath path) {
    switch (path) {
    case EXECUTION_PATH_INSERT:
        return "insert";
    case EXECUTION_PATH_INDEXED:
        return "indexed";
    case EXECUTION_PATH_FULL_SCAN:
        return "full_scan";
    case EXECUTION_PATH_UNKNOWN:
    default:
        return "unknown";
    }
}

/* 마지막 SQL이 어떤 경로로 실행됐는지 외부에서 조회할 때 쓴다. */
ExecutionPath get_last_execution_path(void) {
    return g_context.last_execution_path;
}

/*
 * TABLE 노드 아래 자식 두 개에서 schema와 table 이름을 꺼낸다.
 *
 * 기대 AST 구조:
 * NODE_TABLE
 * ├── NODE_IDENTIFIER(schema)
 * └── NODE_IDENTIFIER(table)
 */
static int extract_table_names(ASTNode *table_node, char **schema_name, char **table_name) {
    ASTNode *schema_node;      /* 첫 번째 자식: schema 이름 */
    ASTNode *table_name_node;  /* 두 번째 자식: table 이름 */

    if (table_node == NULL || table_node->type != NODE_TABLE) {
        return 0;
    }

    schema_node = table_node->first_child;
    if (schema_node == NULL) {
        return 0;
    }

    table_name_node = schema_node->next_sibling;
    if (table_name_node == NULL) {
        return 0;
    }

    *schema_name = schema_node->text;
    *table_name = table_name_node->text;
    return 1;
}

/*
 * 현재 전역 컨텍스트가 같은 테이블을 가리키는지 확인한다.
 *
 * 같다고 판단하는 기준:
 * - schema 이름
 * - table 이름
 * - data file 경로
 * - row_size
 */
static int is_same_table_context(const ExecutionContext *context, const TableMeta *meta) {
    return context->is_loaded
        && strcmp(context->meta.schema_name, meta->schema_name) == 0
        && strcmp(context->meta.table_name, meta->table_name) == 0
        && strcmp(context->meta.data_file_path, meta->data_file_path) == 0
        && context->meta.row_size == meta->row_size;
}

/*
 * 현재 컨텍스트를 완전히 비운다.
 *
 * 내부에서:
 * - B+ 트리 메모리 해제
 * - 구조체 전체 0 초기화
 */
static void reset_execution_context(ExecutionContext *context) {
    bptree_free(&context->id_index);
    memset(context, 0, sizeof(*context));
}

/*
 * 주어진 메타를 기준으로 실행 컨텍스트를 준비한다.
 *
 * 동작:
 * - 같은 테이블이면 기존 컨텍스트 재사용
 * - 다른 테이블이면 기존 컨텍스트를 버리고 새 메타를 복사
 * - .dat 전체를 읽어 id 인덱스를 재구축
 */
static int ensure_execution_context(const TableMeta *meta, Status *status) {
    if (is_same_table_context(&g_context, meta)) {
        return 1;
    }

    reset_execution_context(&g_context);
    g_context.meta = *meta;
    bptree_init(&g_context.id_index);
    if (!build_id_index_from_data(&g_context, status)) {
        reset_execution_context(&g_context);
        return 0;
    }

    g_context.is_loaded = 1;
    return 1;
}

/*
 * schema/table 이름으로 직접 실행 컨텍스트를 준비한다.
 *
 * 어디에 쓰나:
 * - REPL 시작 시 특정 테이블 id 인덱스를 미리 메모리에 올리고 싶을 때
 */
int prepare_execution_context_for_table(const char *schema_name, const char *table_name, Status *status) {
    TableMeta meta; /* load_table_meta 결과를 임시로 받을 메타 구조체 */

    memset(&meta, 0, sizeof(meta));

    if (!load_table_meta(schema_name, table_name, &meta, status)) {
        return 0;
    }

    return ensure_execution_context(&meta, status);
}

/*
 * AST에서 TABLE 노드를 찾아 현재 실행 컨텍스트를 준비한다.
 *
 * parser가 이미 SQL을 AST로 바꿨으므로, executor는 raw SQL 문자열을 다시 해석하지 않고
 * AST에서 schema/table 정보만 꺼내 메타와 인덱스를 준비한다.
 */
int prepare_execution_context(ASTNode *root, Status *status) {
    ASTNode *table_node;   /* AST에서 찾은 TABLE 노드 */
    char *schema_name;     /* TABLE 노드에서 추출한 schema 문자열 */
    char *table_name;      /* TABLE 노드에서 추출한 table 문자열 */

    if (root == NULL) {
        snprintf(status->message, sizeof(status->message), "Execution error: empty AST root");
        return 0;
    }

    table_node = find_child(root, NODE_TABLE);
    if (!extract_table_names(table_node, &schema_name, &table_name)) {
        snprintf(status->message, sizeof(status->message), "Parse error: table node is missing");
        return 0;
    }

    return prepare_execution_context_for_table(schema_name, table_name, status);
}

/*
 * parser가 만든 AST 루트 노드를 보고 실제 실행기로 분기한다.
 *
 * 흐름:
 * 1. AST에서 대상 테이블 확인
 * 2. 메타/인덱스 준비
 * 3. INSERT면 append_binary_row
 * 4. SELECT면 execute_select
 */
int execute_statement(ASTNode *root, Status *status) {
    if (!prepare_execution_context(root, status)) {
        return 0;
    }

    g_context.last_execution_path = EXECUTION_PATH_UNKNOWN;

    if (root->type == NODE_INSERT) {
        int inserted_id; /* 자동 증가로 생성된 새 id */

        g_context.last_execution_path = EXECUTION_PATH_INSERT;
        if (!append_binary_row(&g_context, root, &inserted_id, status)) {
            return 0;
        }
        printf("1 row inserted.\n");
        printf("Inserted id: %d\n", inserted_id);
        return 1;
    }

    if (root->type == NODE_SELECT) {
        return execute_select(&g_context, root, status);
    }

    snprintf(status->message, sizeof(status->message), "Execution error: unsupported statement");
    return 0;
}
