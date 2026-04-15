#include "sql_processor.h"

#include <string.h>

static ExecutionContext g_context;

/* TABLE 노드 아래 자식 두 개에서 schema와 table 이름을 꺼낸다. */
static int extract_table_names(ASTNode *table_node, char **schema_name, char **table_name) {
    ASTNode *schema_node;
    ASTNode *table_name_node;

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

/* 현재 컨텍스트가 같은 테이블을 가리키는지 확인한다. */
static int is_same_table_context(const ExecutionContext *context, const TableMeta *meta) {
    return context->is_loaded
        && strcmp(context->meta.schema_name, meta->schema_name) == 0
        && strcmp(context->meta.table_name, meta->table_name) == 0
        && strcmp(context->meta.data_file_path, meta->data_file_path) == 0
        && context->meta.row_size == meta->row_size;
}

/* 새 테이블 컨텍스트로 바뀔 때 이전 인덱스와 상태를 비운다. */
static void reset_execution_context(ExecutionContext *context) {
    bptree_free(&context->id_index);
    memset(context, 0, sizeof(*context));
}

/* meta를 기준으로 컨텍스트를 준비하고 id 인덱스를 한 번만 재구축한다. */
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

/* schema/table 이름으로 현재 실행 컨텍스트와 id 인덱스를 미리 준비한다. */
int prepare_execution_context_for_table(const char *schema_name, const char *table_name, Status *status) {
    TableMeta meta;

    memset(&meta, 0, sizeof(meta));

    if (!load_table_meta(schema_name, table_name, &meta, status)) {
        return 0;
    }

    return ensure_execution_context(&meta, status);
}

/* AST에서 대상 테이블을 찾아 현재 실행 컨텍스트와 id 인덱스를 미리 준비한다. */
int prepare_execution_context(ASTNode *root, Status *status) {
    ASTNode *table_node;
    char *schema_name;
    char *table_name;

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

/* parser가 만든 AST 루트 노드를 보고 INSERT 또는 SELECT 실행기로 분기한다. */
int execute_statement(ASTNode *root, Status *status) {
    if (!prepare_execution_context(root, status)) {
        return 0;
    }

    if (root->type == NODE_INSERT) {
        int inserted_id;

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
