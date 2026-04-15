# engine_internal.h 수도코드

역할:

- 여러 `.c` 파일이 공유해야 하는 내부 타입과 함수 선언을 모아둔 헤더입니다.
- 외부 사용자용이 아니라 프로젝트 내부 모듈 연결용입니다.

핵심 상수:

```text
DATA_DIR = "data"
SCHEMA_FILE = "data/users.schema.csv"
INDEX_FILE = "data/users.indexes.csv"
DATA_FILE = "data/users.data"
META_FILE = "data/users.meta"

TABLE_NAME = "users"
MAX_NAME_LEN = 64
MAX_EMAIL_LEN = 128
MAX_COLUMNS = 4
MAX_INDEXES = 32
SQL_BUF_SIZE = 8192

BPLUS_TREE_NODE_SIZE_LIMIT = 4096
BPLUS_TREE_TARGET_ORDER = 340
BPLUS_TREE_ORDER = 160
```

자료구조 수도코드:

```text
enum ColumnId:
    ID, NAME, AGE, EMAIL, UNKNOWN

enum ColumnType:
    INT, VARCHAR

enum KeyType:
    INT_KEY, STRING_KEY

enum ConditionOp:
    =, <, <=, >, >=, BETWEEN
```

```text
구조체 Record:
    id
    name
    age
    email

구조체 IndexKey:
    type
    int_value 또는 string_value

구조체 QueryCondition:
    column
    operator
    lower, upper
    lower/upper 존재 여부
    lower/upper inclusive 여부
```

```text
구조체 BPlusNode:
    is_leaf
    num_keys
    keys[]

    만약 internal node라면:
        children[]

    만약 leaf node라면:
        values[]
        next leaf pointer

컴파일 시 검사:
    sizeof(BPlusNode) <= 4096
    // 한 노드가 4KB를 넘지 않도록 보장
```

```text
구조체 Database:
    schema
    table
    indexes[]
    index_count
    persist
```

이해 포인트:

- 이 파일은 프로젝트 내부의 “공통 계약서”입니다.
- C에서는 모듈을 나누면 타입과 함수 선언을 헤더로 공유해야 합니다.
- B+Tree 노드 크기 제한도 여기서 정적으로 확인합니다.

## 자료구조 관계도

```text
Database
    ├── Schema
    │       ├── ColumnMeta[id]
    │       ├── ColumnMeta[name]
    │       ├── ColumnMeta[age]
    │       └── ColumnMeta[email]
    │
    ├── Table
    │       └── Record* rows[]
    │              ├── Record{id=1, name="user1", ...}
    │              ├── Record{id=2, name="user2", ...}
    │              └── ...
    │
    └── IndexMetaRuntime indexes[]
            ├── PRIMARY -> BPlusTree(id)
            ├── idx_users_name -> BPlusTree(name)
            └── idx_users_age -> BPlusTree(age)
```

## Record와 RecordDisk를 같은 타입으로 둔 이유

```text
RecordDisk:
    파일에 저장할 고정 크기 구조체

Record:
    메모리에서 사용할 레코드 타입

현재 구현:
    typedef RecordDisk Record

이유:
    - 과제 범위에서는 users 테이블 구조가 고정되어 있다.
    - 파일에 쓸 구조와 메모리 구조를 같게 두면 바이너리 저장/로드가 단순하다.
    - fread/fwrite로 RecordDisk 크기만큼 바로 읽고 쓸 수 있다.

주의:
    실제 DBMS처럼 가변 길이 VARCHAR를 페이지에 압축 저장하는 방식은 아니다.
    대신 name/email을 고정 길이 char 배열로 둬서 구현 난이도를 낮췄다.
```

## BPlusNode가 4KB를 넘지 않도록 하는 방식

```text
BPLUS_TREE_NODE_SIZE_LIMIT = 4096
BPLUS_TREE_ORDER = 160

BPlusNode 내부:
    keys[BPLUS_TREE_MAX_KEYS + 1]
    internal children 배열 또는 leaf values 배열

컴파일 시:
    _Static_assert(sizeof(BPlusNode) <= 4096)

만약 노드 크기가 4KB를 넘으면:
    컴파일 자체가 실패한다.

의미:
    실제 디스크 페이지 기반 B+Tree처럼 한 노드를 페이지 크기 안에 맞추는 제약을 둔다.
```

## QueryCondition으로 범위 조건을 표현하는 법

```text
id = 10:
    has_lower = true
    lower = 10
    lower_inclusive = true
    has_upper = true
    upper = 10
    upper_inclusive = true

id > 10:
    has_lower = true
    lower = 10
    lower_inclusive = false
    has_upper = false

id BETWEEN 10 AND 20:
    has_lower = true
    lower = 10
    lower_inclusive = true
    has_upper = true
    upper = 20
    upper_inclusive = true
```
