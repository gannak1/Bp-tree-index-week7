#include "engine_internal.h"

static RecordRefList *record_ref_list_create(Record *record) {
    RecordRefList *list = (RecordRefList *)calloc(1, sizeof(RecordRefList));
    if (!list) {
        return NULL;
    }
    list->capacity = 4;
    list->items = (Record **)calloc((size_t)list->capacity, sizeof(Record *));
    if (!list->items) {
        free(list);
        return NULL;
    }
    list->items[list->count++] = record;
    return list;
}

static bool record_ref_list_add(RecordRefList *list, Record *record) {
    if (list->count == list->capacity) {
        int new_cap = list->capacity * 2;
        Record **new_items = (Record **)realloc(list->items, (size_t)new_cap * sizeof(Record *));
        if (!new_items) {
            return false;
        }
        list->items = new_items;
        list->capacity = new_cap;
    }
    list->items[list->count++] = record;
    return true;
}

static void record_ref_list_free(RecordRefList *list) {
    if (!list) {
        return;
    }
    free(list->items);
    free(list);
}

static BPlusNode *bplus_node_create(bool is_leaf) {
    BPlusNode *node = (BPlusNode *)calloc(1, sizeof(BPlusNode));
    if (!node) {
        return NULL;
    }
    node->is_leaf = is_leaf ? 1 : 0;
    return node;
}

BPlusTree *bplus_tree_create(KeyType type, bool unique) {
    BPlusTree *tree = (BPlusTree *)calloc(1, sizeof(BPlusTree));
    if (!tree) {
        return NULL;
    }
    tree->root = bplus_node_create(true);
    if (!tree->root) {
        free(tree);
        return NULL;
    }
    tree->key_type = type;
    tree->unique = unique;
    tree->height = 1;
    return tree;
}

static void bplus_node_free(BPlusNode *node) {
    if (!node) {
        return;
    }
    if (node->is_leaf) {
        for (int i = 0; i < node->num_keys; i++) {
            key_free(&node->keys[i]);
            record_ref_list_free(node->ptrs.leaf.values[i]);
        }
    } else {
        for (int i = 0; i <= node->num_keys; i++) {
            bplus_node_free(node->ptrs.internal.children[i]);
        }
        for (int i = 0; i < node->num_keys; i++) {
            key_free(&node->keys[i]);
        }
    }
    free(node);
}

void bplus_tree_free(BPlusTree *tree) {
    if (!tree) {
        return;
    }
    bplus_node_free(tree->root);
    free(tree);
}

int leaf_lower_bound(BPlusNode *leaf, IndexKey key) {
    int lo = 0;
    int hi = leaf->num_keys;
    while (lo < hi) {
        int mid = lo + (hi - lo) / 2;
        if (key_compare(leaf->keys[mid], key) < 0) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return lo;
}

static int internal_child_index(BPlusNode *node, IndexKey key) {
    int i = 0;
    while (i < node->num_keys && key_compare(key, node->keys[i]) >= 0) {
        i++;
    }
    return i;
}

typedef struct {
    bool split;
    IndexKey promoted;
    BPlusNode *right;
    bool duplicate;
    bool oom;
} InsertResult;

static InsertResult insert_result_none(void) {
    InsertResult r;
    memset(&r, 0, sizeof(r));
    return r;
}

static InsertResult bplus_insert_recursive(BPlusNode *node, IndexKey key, Record *record, bool unique) {
    InsertResult result = insert_result_none();
    if (node->is_leaf) {
        int pos = leaf_lower_bound(node, key);
        if (pos < node->num_keys && key_compare(node->keys[pos], key) == 0) {
            if (unique) {
                result.duplicate = true;
                return result;
            }
            if (!record_ref_list_add(node->ptrs.leaf.values[pos], record)) {
                result.oom = true;
            }
            return result;
        }

        for (int i = node->num_keys; i > pos; i--) {
            node->keys[i] = node->keys[i - 1];
            node->ptrs.leaf.values[i] = node->ptrs.leaf.values[i - 1];
        }
        if (!key_clone(key, &node->keys[pos])) {
            result.oom = true;
            return result;
        }
        node->ptrs.leaf.values[pos] = record_ref_list_create(record);
        if (!node->ptrs.leaf.values[pos]) {
            key_free(&node->keys[pos]);
            result.oom = true;
            return result;
        }
        node->num_keys++;

        if (node->num_keys <= BPLUS_TREE_MAX_KEYS) {
            return result;
        }

        int split_at = node->num_keys / 2;
        BPlusNode *right = bplus_node_create(true);
        if (!right) {
            result.oom = true;
            return result;
        }
        int right_count = node->num_keys - split_at;
        for (int i = 0; i < right_count; i++) {
            right->keys[i] = node->keys[split_at + i];
            right->ptrs.leaf.values[i] = node->ptrs.leaf.values[split_at + i];
            node->ptrs.leaf.values[split_at + i] = NULL;
        }
        right->num_keys = right_count;
        node->num_keys = split_at;
        right->ptrs.leaf.next = node->ptrs.leaf.next;
        node->ptrs.leaf.next = right;
        if (!key_clone(right->keys[0], &result.promoted)) {
            bplus_node_free(right);
            result.oom = true;
            return result;
        }
        result.split = true;
        result.right = right;
        return result;
    }

    int child_pos = internal_child_index(node, key);
    InsertResult child = bplus_insert_recursive(node->ptrs.internal.children[child_pos], key, record, unique);
    if (child.duplicate || child.oom || !child.split) {
        return child;
    }

    for (int i = node->num_keys; i > child_pos; i--) {
        node->keys[i] = node->keys[i - 1];
    }
    for (int i = node->num_keys + 1; i > child_pos + 1; i--) {
        node->ptrs.internal.children[i] = node->ptrs.internal.children[i - 1];
    }
    node->keys[child_pos] = child.promoted;
    node->ptrs.internal.children[child_pos + 1] = child.right;
    node->num_keys++;

    if (node->num_keys <= BPLUS_TREE_MAX_KEYS) {
        return result;
    }

    int mid = node->num_keys / 2;
    BPlusNode *right = bplus_node_create(false);
    if (!right) {
        result.oom = true;
        return result;
    }
    result.promoted = node->keys[mid];

    int right_key_count = node->num_keys - mid - 1;
    for (int i = 0; i < right_key_count; i++) {
        right->keys[i] = node->keys[mid + 1 + i];
    }
    for (int i = 0; i <= right_key_count; i++) {
        right->ptrs.internal.children[i] = node->ptrs.internal.children[mid + 1 + i];
        node->ptrs.internal.children[mid + 1 + i] = NULL;
    }
    right->num_keys = right_key_count;
    node->num_keys = mid;
    result.split = true;
    result.right = right;
    return result;
}

bool bplus_tree_insert(BPlusTree *tree, IndexKey key, Record *record, bool *duplicate) {
    if (duplicate) {
        *duplicate = false;
    }
    InsertResult r = bplus_insert_recursive(tree->root, key, record, tree->unique);
    if (r.duplicate) {
        if (duplicate) {
            *duplicate = true;
        }
        return false;
    }
    if (r.oom) {
        return false;
    }
    if (r.split) {
        BPlusNode *new_root = bplus_node_create(false);
        if (!new_root) {
            key_free(&r.promoted);
            bplus_node_free(r.right);
            return false;
        }
        new_root->keys[0] = r.promoted;
        new_root->ptrs.internal.children[0] = tree->root;
        new_root->ptrs.internal.children[1] = r.right;
        new_root->num_keys = 1;
        tree->root = new_root;
        tree->height++;
    }
    tree->key_count++;
    return true;
}

RecordRefList *bplus_tree_search(BPlusTree *tree, IndexKey key) {
    if (!tree || !tree->root) {
        return NULL;
    }
    BPlusNode *node = tree->root;
    while (!node->is_leaf) {
        int child = internal_child_index(node, key);
        node = node->ptrs.internal.children[child];
    }
    int pos = leaf_lower_bound(node, key);
    if (pos < node->num_keys && key_compare(node->keys[pos], key) == 0) {
        return node->ptrs.leaf.values[pos];
    }
    return NULL;
}

BPlusNode *bplus_tree_leftmost_leaf(BPlusTree *tree) {
    if (!tree || !tree->root) {
        return NULL;
    }
    BPlusNode *node = tree->root;
    while (!node->is_leaf) {
        node = node->ptrs.internal.children[0];
    }
    return node;
}

BPlusNode *bplus_tree_find_leaf(BPlusTree *tree, IndexKey key) {
    if (!tree || !tree->root) {
        return NULL;
    }
    BPlusNode *node = tree->root;
    while (!node->is_leaf) {
        int child = internal_child_index(node, key);
        node = node->ptrs.internal.children[child];
    }
    return node;
}
