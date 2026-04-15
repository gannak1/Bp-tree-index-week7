# bptree.c 수도코드

역할:

- 메모리 기반 B+Tree 인덱스를 구현합니다.
- 삽입, 단일 키 검색, leaf 탐색, range scan을 위한 leaf 연결을 제공합니다.

핵심 개념:

```text
internal node:
    key 배열
    child pointer 배열

leaf node:
    key 배열
    record reference list 배열
    next leaf pointer
```

삽입 수도코드:

```text
함수 bplus_tree_insert(tree, key, record, duplicate):
    duplicate = false

    result = bplus_insert_recursive(tree.root, key, record, tree.unique)

    만약 result가 duplicate라면:
        duplicate = true
        false 반환

    만약 result가 split을 발생시키지 않았다면:
        true 반환

    // root가 split된 경우 새 root 생성
    new_root = internal node 생성
    new_root.keys[0] = result.promoted_key
    new_root.children[0] = old_root
    new_root.children[1] = result.right_node

    tree.root = new_root
    tree.height 증가
    true 반환
```

재귀 삽입 수도코드:

```text
함수 bplus_insert_recursive(node, key, record, unique):
    만약 node가 leaf라면:
        key가 들어갈 위치를 lower_bound로 찾음

        같은 key가 이미 있다면:
            만약 unique index라면:
                duplicate 결과 반환
            아니면:
                같은 key의 record list에 record 추가
                split 없음 반환

        key와 record list를 leaf에 삽입

        만약 leaf의 key 수가 최대 이하라면:
            split 없음 반환

        leaf를 반으로 나눔
        오른쪽 leaf 생성
        절반 key/value를 오른쪽 leaf로 이동
        leaf linked list 연결

        오른쪽 leaf의 첫 key를 부모로 올릴 promoted_key로 반환

    만약 node가 internal node라면:
        key가 내려갈 child 위치를 찾음
        child에 재귀 삽입

        child split이 없다면:
            split 없음 반환

        promoted_key와 right_node를 현재 internal node에 삽입

        만약 internal node의 key 수가 최대 이하라면:
            split 없음 반환

        internal node를 반으로 나눔
        가운데 key를 부모로 올림
        오른쪽 internal node 생성
        split 결과 반환
```

검색 수도코드:

```text
함수 bplus_tree_search(tree, key):
    node = tree.root

    node가 leaf가 아닐 동안:
        child index를 찾음
        node = node.children[child index]

    leaf에서 key 위치를 lower_bound로 찾음

    만약 key가 존재하면:
        해당 record list 반환
    아니면:
        NULL 반환
```

range scan 지원:

```text
함수 bplus_tree_find_leaf(tree, key):
    root부터 내려가서 key가 존재할 수 있는 leaf 반환

함수 bplus_tree_leftmost_leaf(tree):
    가장 왼쪽 child를 계속 따라가서 첫 leaf 반환

// query.c는 이 leaf부터 next 포인터를 따라가며 범위 결과를 모음
```

이해 포인트:

- B+Tree의 실제 레코드는 leaf에만 연결됩니다.
- leaf끼리 `next`로 연결되어 있어서 `BETWEEN`, `<`, `>` 같은 범위 검색이 빠릅니다.
- 이 구현은 디스크 페이지 대신 메모리 구조체를 사용하지만, 노드 크기 4KB 제한을 둬서 DBMS 느낌을 살렸습니다.

