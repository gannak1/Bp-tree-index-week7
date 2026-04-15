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

