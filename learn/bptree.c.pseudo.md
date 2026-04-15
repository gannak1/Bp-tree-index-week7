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

## B+Tree 삽입을 더 자세히 보기

### 1. leaf에 공간이 있는 경우

```text
현재 leaf:
    keys = [10, 20, 40]

삽입 key:
    30

처리:
    lower_bound(30)을 수행하면 위치 2를 얻는다.
    뒤의 key들을 한 칸씩 밀어낸다.
    30을 위치 2에 넣는다.

결과:
    keys = [10, 20, 30, 40]
```

이 경우 부모 노드에는 아무 변화가 없습니다.

### 2. leaf에 같은 key가 이미 있는 경우

```text
만약 unique index라면:
    같은 key 삽입은 허용되지 않는다.
    duplicate = true로 표시하고 실패 반환

만약 non-unique index라면:
    key는 하나만 유지한다.
    해당 key의 RecordRefList에 record 포인터를 추가한다.
```

예:

```text
name index에서 name = "kim"인 row가 여러 개 있을 수 있다.

leaf key:
    "kim"

value:
    [record pointer 1, record pointer 7, record pointer 22]
```

### 3. leaf가 가득 차서 split되는 경우

```text
leaf 최대 key 수를 넘으면 split이 필요하다.

삽입 후 임시 상태:
    left leaf keys = [10, 20, 30, 40, 50]

split 기준:
    절반은 기존 leaf에 남긴다.
    나머지 절반은 새 right leaf로 옮긴다.

결과:
    left leaf keys  = [10, 20]
    right leaf keys = [30, 40, 50]

promoted key:
    right leaf의 첫 key인 30을 부모에게 올린다.
```

leaf 연결도 함께 갱신합니다.

```text
분할 전:
    left -> old_next

분할 후:
    left -> right -> old_next
```

이 연결 덕분에 범위 검색은 leaf를 순서대로 훑을 수 있습니다.

### 4. internal node split

```text
child split 결과로 promoted key가 올라오면 internal node에 삽입한다.

internal node도 key가 너무 많아지면:
    가운데 key를 부모로 올린다.
    왼쪽 key들은 기존 node에 남긴다.
    오른쪽 key들은 새 right internal node로 이동한다.

주의:
    leaf split에서는 promoted key가 오른쪽 leaf에도 남는다.
    internal split에서는 promoted key가 부모로 올라가고 현재 노드에서는 빠진다.
```

## 검색 경로 예시

```text
검색 key = 75

root internal keys:
    [30, 60, 90]

child 선택:
    75는 60보다 크고 90보다 작다.
    따라서 children[2]로 내려간다.

다음 internal 또는 leaf에서도 같은 방식 반복

leaf에 도착하면:
    lower_bound(75)
    key가 실제로 75인지 확인
```

복잡도:

```text
단일 검색:
    O(log N)

범위 검색:
    O(log N + K)
    // K는 결과 row 수
```

## 왜 leaf에만 record를 저장하는가

```text
B+Tree:
    internal node는 길 안내용 key만 가진다.
    실제 데이터 pointer는 leaf node에만 둔다.

장점:
    1. 모든 실제 데이터가 leaf에 있으므로 순차 접근이 쉽다.
    2. leaf를 linked list로 연결하면 range scan이 빠르다.
    3. internal node는 더 많은 key를 담아 tree height를 낮출 수 있다.
```

## 이 구현에서 저장하는 값

```text
B+Tree key:
    id, age 같은 int
    name, email 같은 string

B+Tree value:
    Record* pointer

파일에 저장되는 것:
    Record 자체는 data/users.data에 저장
    B+Tree 노드는 저장하지 않음

재시작 시:
    storage.c가 Record를 읽음
    rebuild_indexes()가 모든 Record를 다시 B+Tree에 삽입
```
