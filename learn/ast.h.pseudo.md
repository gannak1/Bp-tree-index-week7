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

