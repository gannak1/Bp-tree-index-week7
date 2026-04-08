#include "sql_processor.h"

#include <string.h>

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

/* parser가 만든 AST 루트 노드를 보고 INSERT 또는 SELECT 실행기로 분기한다. */
int execute_statement(ASTNode *root, Status *status) {
    TableMeta meta;
    ASTNode *table_node;
    char *schema_name;
    char *table_name;

    memset(&meta, 0, sizeof(meta));

    if (root == NULL) {
        snprintf(status->message, sizeof(status->message), "Execution error: empty AST root");
        return 0;
    }

    table_node = find_child(root, NODE_TABLE);
    if (!extract_table_names(table_node, &schema_name, &table_name)) {
        snprintf(status->message, sizeof(status->message), "Parse error: table node is missing");
        return 0;
    }

    if (!load_table_meta(schema_name, table_name, &meta, status)) {
        return 0;
    }

    if (root->type == NODE_INSERT) {
        if (!append_binary_row(&meta, root, status)) {
            return 0;
        }
        printf("1 row inserted.\n");
        return 1;
    }

    if (root->type == NODE_SELECT) {
        return execute_select(&meta, root, status);
    }

    snprintf(status->message, sizeof(status->message), "Execution error: unsupported statement");
    return 0;
}
