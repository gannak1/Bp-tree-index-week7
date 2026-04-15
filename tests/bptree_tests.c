#include "../src/sql_processor.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_bptree_insert_and_search(void) {
    BPTree tree;
    Status status;
    long offset;
    int i;

    bptree_init(&tree);
    status.ok = 1;
    status.message[0] = '\0';

    for (i = 1; i <= 200; i++) {
        assert(bptree_insert(&tree, i, (long)(i * 100), &status) == 1);
    }

    assert(bptree_search(&tree, 1, &offset) == 1);
    assert(offset == 100);
    assert(bptree_search(&tree, 99, &offset) == 1);
    assert(offset == 9900);
    assert(bptree_search(&tree, 200, &offset) == 1);
    assert(offset == 20000);
    assert(bptree_search(&tree, 201, &offset) == 0);

    bptree_free(&tree);
}

static void test_between_parse_shape(void) {
    Status status;
    TokenArray tokens;
    ASTNode *root;
    ASTNode *where_node;
    ASTNode *column_node;
    ASTNode *between_node;
    ASTNode *lower_node;
    ASTNode *upper_node;

    root = NULL;
    tokens = lex_sql("SELECT * FROM users WHERE age BETWEEN 20 AND 30;", &status);
    assert(status.ok == 1);
    assert(parse_statement(&tokens, &root, &status) == 1);
    assert(root != NULL);
    assert(root->type == NODE_SELECT);

    where_node = find_child(root, NODE_WHERE);
    assert(where_node != NULL);
    assert(where_node->type == NODE_WHERE);

    column_node = where_node->first_child;
    assert(column_node != NULL);
    assert(column_node->type == NODE_COLUMN);
    assert(strcmp(column_node->text, "age") == 0);

    between_node = column_node->next_sibling;
    assert(between_node != NULL);
    assert(between_node->type == NODE_BETWEEN);

    lower_node = between_node->first_child;
    upper_node = lower_node->next_sibling;
    assert(lower_node != NULL);
    assert(upper_node != NULL);
    assert(strcmp(lower_node->text, "20") == 0);
    assert(strcmp(upper_node->text, "30") == 0);

    free_ast(root);
    free_tokens(&tokens);
}

int main(void) {
    test_bptree_insert_and_search();
    test_between_parse_shape();
    printf("All tests passed.\n");
    return 0;
}
