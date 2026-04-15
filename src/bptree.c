#include "engine_internal.h"

/*
 * bptree.c
 *
 * 메모리 기반 B+Tree 인덱스 구현 파일입니다.
 *
 * 이 프로젝트의 B+Tree는 실제 DBMS의 디스크 페이지 기반 B+Tree를 단순화한 형태입니다.
 * - internal node는 길 안내용 key와 child pointer만 가집니다.
 * - leaf node는 key와 실제 Record* 목록을 가집니다.
 * - leaf node끼리는 next pointer로 연결되어 있어 BETWEEN, <, > 같은 범위 검색을 빠르게 처리합니다.
 *
 * 파일에는 B+Tree 자체를 저장하지 않습니다.
 * 프로그램 시작 시 storage.c가 바이너리 레코드를 읽고, rebuild_indexes()가 이 파일의 삽입 함수를
 * 호출해서 메모리 B+Tree를 다시 구성합니다.
 */

/* non-unique index에서는 같은 key에 여러 row가 매달릴 수 있으므로 Record* 동적 배열을 사용합니다. */
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

/* 같은 key를 가진 row가 추가될 때 RecordRefList를 2배씩 확장합니다. */
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

/* leaf node가 소유한 RecordRefList 배열만 해제합니다. 실제 Record는 Table이 소유하므로 여기서 해제하지 않습니다. */
static void record_ref_list_free(RecordRefList *list) {
    if (!list) {
        return;
    }
    free(list->items);
    free(list);
}

/* leaf/internal 여부만 다른 빈 B+Tree 노드를 생성합니다. */
static BPlusNode *bplus_node_create(bool is_leaf) {
    BPlusNode *node = (BPlusNode *)calloc(1, sizeof(BPlusNode));
    if (!node) {
        return NULL;
    }
    node->is_leaf = is_leaf ? 1 : 0;
    return node;
}

/* 새 B+Tree는 항상 root 하나짜리 leaf tree로 시작합니다. */
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

/*
 * 노드 재귀 해제.
 *
 * leaf:
 *   key 문자열과 RecordRefList를 해제합니다.
 * internal:
 *   child node를 먼저 재귀적으로 해제한 뒤 key를 해제합니다.
 */
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

/* tree wrapper와 root 이하 모든 노드를 해제합니다. */
void bplus_tree_free(BPlusTree *tree) {
    if (!tree) {
        return;
    }
    bplus_node_free(tree->root);
    free(tree);
}

/*
 * leaf node 안에서 key가 들어갈 첫 위치를 찾습니다.
 *
 * lower_bound 의미:
 *   keys[pos] >= key가 되는 가장 왼쪽 pos를 반환합니다.
 *
 * 검색, 삽입, range scan 시작 위치 계산에 모두 사용됩니다.
 */
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

/*
 * internal node에서 어떤 child로 내려갈지 결정합니다.
 *
 * B+Tree internal key가 [10, 20, 30]이라면:
 *   key < 10        -> child 0
 *   10 <= key < 20  -> child 1
 *   20 <= key < 30  -> child 2
 *   30 <= key       -> child 3
 */
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

/* 재귀 삽입 결과 기본값입니다. split/duplicate/oom 모두 false 상태로 시작합니다. */
static InsertResult insert_result_none(void) {
    InsertResult r;
    memset(&r, 0, sizeof(r));
    return r;
}

/*
 * B+Tree 삽입의 핵심 재귀 함수입니다.
 *
 * 반환값 InsertResult는 다음 상황을 부모에게 알려줍니다.
 * - split=false: 현재 노드에서 삽입이 끝났고 부모는 할 일이 없습니다.
 * - split=true : 현재 노드가 분할되었으므로 promoted key와 right node를 부모에 삽입해야 합니다.
 * - duplicate  : unique index에서 중복 key가 발견되었습니다.
 * - oom        : 메모리 할당 실패입니다.
 */
static InsertResult bplus_insert_recursive(BPlusNode *node, IndexKey key, Record *record, bool unique) {
    InsertResult result = insert_result_none();
    if (node->is_leaf) {
        /* leaf 안에서 key가 들어갈 위치를 찾습니다. */
        int pos = leaf_lower_bound(node, key);

        /*
         * 같은 key가 이미 leaf에 존재하는 경우.
         * unique index면 실패하고, non-unique index면 기존 key의 RecordRefList에 row만 추가합니다.
         */
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

        /* 새 key를 넣기 위해 pos 뒤쪽 key/value를 한 칸씩 오른쪽으로 밀어냅니다. */
        for (int i = node->num_keys; i > pos; i--) {
            node->keys[i] = node->keys[i - 1];
            node->ptrs.leaf.values[i] = node->ptrs.leaf.values[i - 1];
        }
        /*
         * B+Tree 노드는 key를 자체 소유해야 하므로 key_clone()을 사용합니다.
         * 특히 string key는 Record 안 문자열을 빌려 쓰지 않고 별도 복사본을 저장합니다.
         */
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

        /* 최대 key 수를 넘지 않았다면 split 없이 삽입 완료입니다. */
        if (node->num_keys <= BPLUS_TREE_MAX_KEYS) {
            return result;
        }

        /*
         * leaf split.
         *
         * 기존 leaf의 뒤쪽 절반을 새 right leaf로 옮깁니다.
         * B+Tree에서는 leaf split 시 right leaf의 첫 key가 부모로 올라가며,
         * 그 key는 right leaf에도 그대로 남아 있습니다.
         */
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

        /*
         * leaf linked list 갱신.
         *
         * 범위 검색은 leaf->next를 따라가며 순차적으로 row를 모으므로,
         * split 후에도 정렬 순서가 유지되도록 left -> right -> old_next로 연결합니다.
         */
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

    /* internal node에서는 key가 들어갈 child를 찾아 아래로 재귀 삽입합니다. */
    int child_pos = internal_child_index(node, key);
    InsertResult child = bplus_insert_recursive(node->ptrs.internal.children[child_pos], key, record, unique);
    if (child.duplicate || child.oom || !child.split) {
        return child;
    }

    /*
     * child가 split되었으므로 child.promoted key와 child.right node를 현재 internal node에 끼워 넣습니다.
     */
    for (int i = node->num_keys; i > child_pos; i--) {
        node->keys[i] = node->keys[i - 1];
    }
    for (int i = node->num_keys + 1; i > child_pos + 1; i--) {
        node->ptrs.internal.children[i] = node->ptrs.internal.children[i - 1];
    }
    node->keys[child_pos] = child.promoted;
    node->ptrs.internal.children[child_pos + 1] = child.right;
    node->num_keys++;

    /* 현재 internal node에 아직 공간이 있으면 여기서 종료합니다. */
    if (node->num_keys <= BPLUS_TREE_MAX_KEYS) {
        return result;
    }

    /*
     * internal split.
     *
     * 가운데 key는 부모로 올라가고, 오른쪽 key/child들은 새 right internal node로 이동합니다.
     * leaf split과 달리 promoted key는 현재 노드에 남지 않습니다.
     */
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
        /*
         * root가 split된 경우 tree height가 1 증가합니다.
         * 새 root는 promoted key 하나와 기존 root, 새 right node를 child로 가집니다.
         */
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

/*
 * 단일 key 검색.
 *
 * root에서 시작해 internal key를 기준으로 leaf까지 내려간 뒤,
 * leaf 안에서 lower_bound로 정확한 key 위치를 확인합니다.
 */
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

/* upper bound가 없는 range scan(id <= 100 등)에서 시작점으로 사용할 가장 왼쪽 leaf를 찾습니다. */
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

/* lower bound가 있는 range scan(id >= 100, BETWEEN 등)의 시작 leaf를 찾습니다. */
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
