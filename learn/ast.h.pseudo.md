# ast.h 수도코드

역할:

- SQL을 트리 형태로 표현하기 위한 AST 타입을 정의합니다.
- 루트 노드는 `SELECT`, `INSERT`, `CREATE INDEX` 같은 명령을 나타내고, 자식 노드는 `TABLE`, `WHERE`, `CONDITION`, `COLUMN_LIST`, `VALUE_LIST`, `INDEX_HINT` 같은 SQL 구성 요소를 나타냅니다.

수도코드:

```text
상수 AST_SQL_MAX = 8192
    // 하나의 SQL 명령 최대 길이

enum AstKind:
    EMPTY
    EXIT
    INSERT
    SELECT
    EXPLAIN
    SHOW_INDEX
    CREATE_INDEX
    CREATE_UNIQUE_INDEX
    DROP_INDEX
    ALTER_PRIMARY_KEY
    SAVE
    LOAD_SCHEMA
    LOAD_DATA_BINARY
    BENCHMARK
    UNSUPPORTED
    TABLE
    SELECT_LIST
    COLUMN_LIST
    VALUE_LIST
    WHERE
    CONDITION
    INDEX_HINT
    PATH
    BENCHMARK_OPTIONS
```

```text
구조체 AstNode:
    kind
        // 이 노드가 어떤 SQL 요소인지 나타냄

    text
        // 노드가 담당하는 SQL 조각

    start, length
        // 원본 SQL에서 이 노드가 차지한 위치

    first_child
        // 첫 번째 자식 노드

    next_sibling
        // 같은 부모를 가진 다음 형제 노드
```

```text
구조체 SqlAst:
    kind
        // root->kind를 빠르게 접근하기 위한 명령 종류

    sql
        // 세미콜론과 앞뒤 공백을 정리한 SQL 문자열

    root
        // 실제 AST 트리의 루트 노드
```

```text
함수 sql_ast_parse(input, ast, err, err_size) -> bool
    // 입력 SQL을 AstNode 트리로 변환

함수 sql_ast_free(ast)
    // AST 노드 트리 전체 메모리 해제

함수 sql_ast_kind_name(kind) -> 문자열
    // 디버깅용 AST 종류 이름 반환
```

이해 포인트:

- `executor.c`는 `ast.root->kind`를 보고 실행 함수를 선택합니다.
- `SELECT`, `INSERT`, `CREATE INDEX` 같은 명령은 자식 노드로 주요 SQL 조각을 가집니다.
- WHERE 조건은 AST에서 `AST_WHERE -> AST_CONDITION`으로 표현되고, 실제 타입 체크와 범위 조건 변환은 `query.c`의 `QueryCondition`이 추가로 담당합니다.

## AstKind를 enum으로 둔 이유

```text
문자열로 계속 비교하는 방식:
    if starts_with(sql, "SELECT")
    else if starts_with(sql, "INSERT")
    else if starts_with(sql, "CREATE INDEX")
    ...

문제:
    실행 로직 안에 문자열 비교가 계속 섞인다.
    명령 종류가 늘어나면 코드가 지저분해진다.

enum 방식:
    AST_SELECT
    AST_INSERT
    AST_CREATE_INDEX

장점:
    명령 종류가 명확하다.
    switch문으로 실행 흐름을 관리할 수 있다.
    디버깅할 때 sql_ast_kind_name()으로 이름도 확인할 수 있다.
```

## SqlAst가 보관하는 정보

```text
SqlAst.kind:
    명령의 종류
    예: AST_SELECT

SqlAst.sql:
    실행에 사용할 정리된 SQL 문자열
    앞뒤 공백 제거
    마지막 세미콜론 제거

SqlAst.root:
    실제 AST root node
    예: root.kind = AST_SELECT
    root.first_child = AST_SELECT_LIST

왜 원본 SQL도 보관하는가:
    executor는 세부 파싱을 위해 SQL 문자열이 필요하다.
    예를 들어 SELECT라면 WHERE 뒤 조건을 다시 읽어야 한다.
```

## AST 예시

```text
SQL:
    SELECT * FROM users FORCE INDEX (PRIMARY) WHERE id BETWEEN 1 AND 10;

AST:
    AST_SELECT
        AST_SELECT_LIST "*"
        AST_TABLE "users"
        AST_INDEX_HINT "FORCE PRIMARY"
        AST_WHERE "id BETWEEN 1 AND 10"
            AST_CONDITION "id BETWEEN 1 AND 10"
```

## 현재 AST의 범위

```text
구현한 것:
    명령 root node
    SELECT list node
    table node
    WHERE/CONDITION node
    INSERT column/value list node
    CREATE INDEX column node
    LOAD path node
    BENCHMARK option node

아직 구현하지 않은 것:
    JOIN
    GROUP BY
    복잡한 논리식 AND/OR

하지만 B+Tree 인덱스 과제에서 필요한:
    WHERE column operator value
    WHERE column BETWEEN value AND value
는 AST와 QueryCondition 조합으로 처리한다.
```
