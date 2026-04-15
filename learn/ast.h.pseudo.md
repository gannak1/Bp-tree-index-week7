# ast.h 수도코드

역할:

- SQL 명령을 어떤 종류로 분류할지 나타내는 AST 타입을 정의합니다.
- 여기서의 AST는 복잡한 트리 전체가 아니라, 현재 프로젝트 범위에 맞춘 “명령 단위 AST”입니다.

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
```

```text
구조체 SqlAst:
    kind
        // SQL 명령 종류

    sql
        // 세미콜론과 앞뒤 공백을 정리한 SQL 문자열
```

```text
함수 sql_ast_parse(input, ast, err, err_size) -> bool
    // 입력 SQL을 SqlAst로 변환

함수 sql_ast_kind_name(kind) -> 문자열
    // 디버깅용 AST 종류 이름 반환
```

이해 포인트:

- `executor.c`는 원본 문자열을 바로 분기하지 않고 `SqlAst.kind`를 보고 실행합니다.
- WHERE 조건의 세부 구조는 `query.c`의 `QueryCondition`이 추가로 담당합니다.

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

왜 원본 SQL도 보관하는가:
    executor는 세부 파싱을 위해 SQL 문자열이 필요하다.
    예를 들어 SELECT라면 WHERE 뒤 조건을 다시 읽어야 한다.
```

## 현재 AST의 한계

```text
현재 구현은 과제 목적에 맞춘 단순 AST다.

지원하는 것:
    명령 종류 분류
    SQL 문자열 정리

지원하지 않는 것:
    SELECT column list 전체 트리화
    JOIN
    GROUP BY
    복잡한 논리식 AND/OR

하지만 B+Tree 인덱스 과제에서 필요한:
    WHERE column operator value
    WHERE column BETWEEN value AND value
는 query.c의 QueryCondition으로 별도 처리한다.
```
