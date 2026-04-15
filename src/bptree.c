#include "sql_processor.h"

#include <stdlib.h>
#include <string.h>

/*
 * 삽입 중 split이 발생했을 때 부모 호출자에게 전달할 결과 구조체다.
 *
 * 필드 의미:
 * - did_split: 현재 노드에서 분할이 일어났는지 여부
 * - promoted_key: 부모로 올려보낼 기준 key
 * - right_node: 분할 후 오른쪽 새 노드
 */
typedef struct {
    int did_split;
    int promoted_key;
    BPTreeNode *right_node;
} BPTreeInsertResult;

/*
 * B+ 트리 노드 하나를 생성한다.
 *
 * 입력:
 * - is_leaf: 1이면 leaf node, 0이면 internal node
 *
 * 내부 동작:
 * - 노드를 malloc
 * - 메모리를 0으로 초기화
 * - is_leaf 플래그 설정
 */
static BPTreeNode *create_bptree_node(int is_leaf) {
    BPTreeNode *node; /* 새로 만들 노드 */

    node = (BPTreeNode *)malloc(sizeof(BPTreeNode));
    if (node == NULL) {
        return NULL;
    }

    memset(node, 0, sizeof(*node));
    node->is_leaf = is_leaf;
    return node;
}

/*
 * 노드와 그 하위 child 포인터를 따라가며 B+ 트리 메모리를 해제한다.
 *
 * 주의:
 * - leaf의 next 포인터는 해제 순회를 위해 사용하지 않는다.
 * - child 포인터 계층만 따라가도 전체 트리를 한 번만 해제할 수 있다.
 */
static void free_bptree_node(BPTreeNode *node) {
    int i; /* internal node의 자식 배열 순회 인덱스 */

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

/*
 * leaf 노드에서 key가 들어가야 할 정렬 위치를 찾는다.
 *
 * 예:
 * - keys = [1, 3, 7]
 * - key = 5
 * -> 반환값은 2
 */
static int find_leaf_insert_index(BPTreeNode *node, int key) {
    int index = 0; /* 삽입 위치를 찾기 위해 움직이는 인덱스 */

    while (index < node->key_count && node->keys[index] < key) {
        index++;
    }
    return index;
}

/*
 * internal node에서 어떤 child로 내려가야 하는지 찾는다.
 *
 * 규칙:
 * - 현재 key보다 작은 separator는 지나가고
 * - 처음으로 더 큰 separator를 만나기 전 child로 내려간다.
 */
static int find_child_index(BPTreeNode *node, int key) {
    int index = 0; /* child 선택을 위한 인덱스 */

    while (index < node->key_count && key >= node->keys[index]) {
        index++;
    }
    return index;
}

/*
 * leaf node에 key/offset을 삽입한다.
 *
 * 동작:
 * - 정렬 위치를 찾는다.
 * - 뒤쪽 key/offset을 한 칸씩 민다.
 * - 새 key/offset을 넣는다.
 * - overflow면 leaf split을 수행한다.
 *
 * 반환:
 * - split 없으면 did_split = 0
 * - split 있으면 promoted_key와 right_node를 채워 반환
 */
static BPTreeInsertResult insert_into_leaf(BPTreeNode *node, int key, long offset, Status *status) {
    BPTreeInsertResult result; /* 부모에게 돌려줄 split 결과 */
    int insert_index;          /* 새 key가 들어갈 정렬 위치 */
    int i;                     /* 뒤쪽 배열을 미는 반복 변수 */

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
        int split_index = node->key_count / 2; /* 왼쪽 leaf가 유지할 key 수 */
        int total_keys = node->key_count;      /* split 전 전체 key 수 */
        BPTreeNode *right = create_bptree_node(1); /* 새 오른쪽 leaf */

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

/*
 * internal node 아래 적절한 child에 삽입을 재귀 호출한다.
 *
 * child 쪽 split이 발생하면:
 * - promoted_key를 현재 internal node에 끼워 넣고
 * - 오른쪽 새 child 포인터를 연결한다.
 *
 * 현재 internal node도 overflow하면 다시 split 결과를 부모로 올린다.
 */
static BPTreeInsertResult insert_into_internal(BPTreeNode *node, int key, long offset, Status *status) {
    BPTreeInsertResult child_result; /* child 삽입 결과 */
    BPTreeInsertResult result;       /* 현재 노드 삽입 결과 */
    int child_index;                 /* 내려갈 child 위치 */
    int i;                           /* 배열 이동 반복 변수 */

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
        int mid_index = node->key_count / 2;   /* 부모로 올릴 separator 위치 */
        int total_keys = node->key_count;      /* split 전 전체 key 수 */
        BPTreeNode *right = create_bptree_node(0); /* 새 오른쪽 internal node */

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

/* B+ 트리를 빈 상태로 초기화한다. */
void bptree_init(BPTree *tree) {
    if (tree == NULL) {
        return;
    }
    tree->root = NULL;
}

/* B+ 트리 전체 메모리를 해제한다. */
void bptree_free(BPTree *tree) {
    if (tree == NULL) {
        return;
    }

    free_bptree_node(tree->root);
    tree->root = NULL;
}

/*
 * key 하나를 B+ 트리에 삽입한다.
 *
 * 흐름:
 * - root가 없으면 leaf root를 새로 만든다.
 * - root가 leaf면 바로 leaf 삽입
 * - root가 internal이면 재귀 삽입
 * - root split이 발생하면 새 root를 만든다.
 */
int bptree_insert(BPTree *tree, int key, long offset, Status *status) {
    BPTreeInsertResult result; /* root 삽입 결과 */

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
        BPTreeNode *new_root = create_bptree_node(0); /* split 후 새 root */
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

/*
 * key를 따라가며 leaf에서 row offset을 찾는다.
 *
 * 반환:
 * - 성공: 1, offset 출력
 * - 실패: 0
 */
int bptree_search(const BPTree *tree, int key, long *offset) {
    BPTreeNode *node; /* 현재 탐색 중인 노드 */
    int i;            /* leaf 안에서 key 비교용 인덱스 */

    if (tree == NULL || tree->root == NULL) {
        return 0;
    }

    node = tree->root;
    while (!node->is_leaf) {
        int child_index = find_child_index(node, key); /* 다음에 내려갈 child 위치 */
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
