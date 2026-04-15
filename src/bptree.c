#include "sql_processor.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    int did_split;
    int promoted_key;
    BPTreeNode *right_node;
} BPTreeInsertResult;

/* 새 B+ 트리 노드 하나를 초기화한다. */
static BPTreeNode *create_bptree_node(int is_leaf) {
    BPTreeNode *node;

    node = (BPTreeNode *)malloc(sizeof(BPTreeNode));
    if (node == NULL) {
        return NULL;
    }

    memset(node, 0, sizeof(*node));
    node->is_leaf = is_leaf;
    return node;
}

/* 하위 child 포인터만 따라가며 트리를 해제한다. leaf next는 따라가지 않는다. */
static void free_bptree_node(BPTreeNode *node) {
    int i;

    if (node == NULL) {
        return;
    }

    if (!node->is_leaf) {
        for (i = 0; i <= node->key_count; i++) {
            free_bptree_node(node->children[i]);
        }
    }

    free(node);
}

/* leaf 안에서 key가 들어갈 정렬 위치를 찾는다. */
static int find_leaf_insert_index(BPTreeNode *node, int key) {
    int index = 0;

    while (index < node->key_count && node->keys[index] < key) {
        index++;
    }
    return index;
}

/* internal node에서 key가 내려갈 child 인덱스를 찾는다. */
static int find_child_index(BPTreeNode *node, int key) {
    int index = 0;

    while (index < node->key_count && key >= node->keys[index]) {
        index++;
    }
    return index;
}

/* leaf node에 key/offset을 삽입하고 overflow 시 split 결과를 반환한다. */
static BPTreeInsertResult insert_into_leaf(BPTreeNode *node, int key, long offset, Status *status) {
    BPTreeInsertResult result;
    int insert_index;
    int i;

    memset(&result, 0, sizeof(result));

    insert_index = find_leaf_insert_index(node, key);
    if (insert_index < node->key_count && node->keys[insert_index] == key) {
        snprintf(status->message, sizeof(status->message), "Execution error: duplicate id key '%d'", key);
        return result;
    }

    for (i = node->key_count; i > insert_index; i--) {
        node->keys[i] = node->keys[i - 1];
        node->offsets[i] = node->offsets[i - 1];
    }

    node->keys[insert_index] = key;
    node->offsets[insert_index] = offset;
    node->key_count++;

    if (node->key_count <= BPTREE_MAX_KEYS) {
        return result;
    }

    {
        int split_index = node->key_count / 2;
        int total_keys = node->key_count;
        BPTreeNode *right = create_bptree_node(1);

        if (right == NULL) {
            snprintf(status->message, sizeof(status->message), "Execution error: out of memory");
            result.promoted_key = 0;
            return result;
        }

        right->key_count = total_keys - split_index;
        for (i = 0; i < right->key_count; i++) {
            right->keys[i] = node->keys[split_index + i];
            right->offsets[i] = node->offsets[split_index + i];
        }

        node->key_count = split_index;
        right->next = node->next;
        node->next = right;

        result.did_split = 1;
        result.promoted_key = right->keys[0];
        result.right_node = right;
        return result;
    }
}

/* internal node에 승격 key와 새 오른쪽 자식을 삽입하고 overflow 시 split한다. */
static BPTreeInsertResult insert_into_internal(BPTreeNode *node, int key, long offset, Status *status) {
    BPTreeInsertResult child_result;
    BPTreeInsertResult result;
    int child_index;
    int i;

    memset(&result, 0, sizeof(result));

    child_index = find_child_index(node, key);
    child_result = node->children[child_index]->is_leaf
        ? insert_into_leaf(node->children[child_index], key, offset, status)
        : insert_into_internal(node->children[child_index], key, offset, status);

    if (status->message[0] != '\0') {
        return result;
    }
    if (!child_result.did_split) {
        return result;
    }

    for (i = node->key_count; i > child_index; i--) {
        node->keys[i] = node->keys[i - 1];
    }
    for (i = node->key_count + 1; i > child_index + 1; i--) {
        node->children[i] = node->children[i - 1];
    }

    node->keys[child_index] = child_result.promoted_key;
    node->children[child_index + 1] = child_result.right_node;
    node->key_count++;

    if (node->key_count <= BPTREE_MAX_KEYS) {
        return result;
    }

    {
        int mid_index = node->key_count / 2;
        int total_keys = node->key_count;
        BPTreeNode *right = create_bptree_node(0);

        if (right == NULL) {
            snprintf(status->message, sizeof(status->message), "Execution error: out of memory");
            return result;
        }

        right->key_count = total_keys - mid_index - 1;
        for (i = 0; i < right->key_count; i++) {
            right->keys[i] = node->keys[mid_index + 1 + i];
        }
        for (i = 0; i <= right->key_count; i++) {
            right->children[i] = node->children[mid_index + 1 + i];
        }

        result.did_split = 1;
        result.promoted_key = node->keys[mid_index];
        result.right_node = right;
        node->key_count = mid_index;
    }

    return result;
}

/* 트리 초기 상태를 만든다. */
void bptree_init(BPTree *tree) {
    if (tree == NULL) {
        return;
    }
    tree->root = NULL;
}

/* 트리 전체를 해제한다. */
void bptree_free(BPTree *tree) {
    if (tree == NULL) {
        return;
    }

    free_bptree_node(tree->root);
    tree->root = NULL;
}

/* key 하나를 B+ 트리에 삽입한다. 필요하면 루트를 분할한다. */
int bptree_insert(BPTree *tree, int key, long offset, Status *status) {
    BPTreeInsertResult result;

    if (tree->root == NULL) {
        tree->root = create_bptree_node(1);
        if (tree->root == NULL) {
            snprintf(status->message, sizeof(status->message), "Execution error: out of memory");
            return 0;
        }
        tree->root->keys[0] = key;
        tree->root->offsets[0] = offset;
        tree->root->key_count = 1;
        return 1;
    }

    status->ok = 1;
    status->message[0] = '\0';

    result = tree->root->is_leaf
        ? insert_into_leaf(tree->root, key, offset, status)
        : insert_into_internal(tree->root, key, offset, status);

    if (status->message[0] != '\0') {
        status->ok = 0;
        return 0;
    }

    if (result.did_split) {
        BPTreeNode *new_root = create_bptree_node(0);
        if (new_root == NULL) {
            snprintf(status->message, sizeof(status->message), "Execution error: out of memory");
            status->ok = 0;
            return 0;
        }

        new_root->keys[0] = result.promoted_key;
        new_root->children[0] = tree->root;
        new_root->children[1] = result.right_node;
        new_root->key_count = 1;
        tree->root = new_root;
    }

    return 1;
}

/* key를 따라 내려가 leaf에서 row offset을 찾는다. */
int bptree_search(const BPTree *tree, int key, long *offset) {
    BPTreeNode *node;
    int i;

    if (tree == NULL || tree->root == NULL) {
        return 0;
    }

    node = tree->root;
    while (!node->is_leaf) {
        int child_index = find_child_index(node, key);
        node = node->children[child_index];
        if (node == NULL) {
            return 0;
        }
    }

    for (i = 0; i < node->key_count; i++) {
        if (node->keys[i] == key) {
            if (offset != NULL) {
                *offset = node->offsets[i];
            }
            return 1;
        }
    }

    return 0;
}
