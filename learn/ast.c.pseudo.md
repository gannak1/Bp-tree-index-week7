# ast.c 수도코드

역할:

- 입력 SQL을 정리하고 명령 종류를 판별합니다.
- 대소문자를 구분하지 않고 `SELECT`, `INSERT`, `CREATE INDEX` 등을 인식합니다.

수도코드:

```text
함수 trim_inplace(sql):
    앞쪽 공백 제거
    뒤쪽 공백 제거
```

```text
함수 strip_optional_semicolon(sql):
    trim_inplace(sql)

    만약 마지막 문자가 ';'라면:
        마지막 문자를 문자열 끝으로 바꿈
        다시 trim_inplace(sql)

    // REPL에서는 사용자가 세미콜론을 붙이므로 실행 전 제거
```

```text
함수 sql_ast_parse(input, ast, err, err_size):
    ast 초기화
    ast.kind = EMPTY

    만약 input이 NULL이라면:
        true 반환

    만약 input 길이가 AST_SQL_MAX보다 크다면:
        err에 "SQL command is too long" 저장
        false 반환

    ast.sql에 input 복사
    세미콜론과 공백 정리

    만약 ast.sql이 비어 있다면:
        ast.kind = EMPTY
        true 반환

    만약 ast.sql이 "EXIT" 또는 "QUIT"라면:
        ast.kind = EXIT
    아니고 "INSERT"로 시작하면:
        ast.kind = INSERT
    아니고 "EXPLAIN"으로 시작하면:
        ast.kind = EXPLAIN
    아니고 "SELECT"로 시작하면:
        ast.kind = SELECT
    아니고 "SHOW INDEX"로 시작하면:
        ast.kind = SHOW_INDEX
    아니고 "CREATE UNIQUE INDEX"로 시작하면:
        ast.kind = CREATE_UNIQUE_INDEX
    아니고 "CREATE INDEX"로 시작하면:
        ast.kind = CREATE_INDEX
    아니고 "DROP INDEX"로 시작하면:
        ast.kind = DROP_INDEX
    아니고 "ALTER TABLE"이며 "MODIFY PRIMARY KEY"를 포함하면:
        ast.kind = ALTER_PRIMARY_KEY
    아니고 "SAVE"라면:
        ast.kind = SAVE
    아니고 "LOAD SCHEMA"로 시작하면:
        ast.kind = LOAD_SCHEMA
    아니고 "LOAD DATA BINARY"로 시작하면:
        ast.kind = LOAD_DATA_BINARY
    아니고 "BENCHMARK"로 시작하면:
        ast.kind = BENCHMARK
    그 외:
        ast.kind = UNSUPPORTED

    true 반환
```

이해 포인트:

- AST 단계에서는 명령의 큰 종류만 판별합니다.
- 세부 문법 오류는 각 executor가 더 정확한 위치와 함께 출력합니다.
- 대소문자 무시 비교를 직접 구현해서 `select`, `SELECT`, `SeLeCt` 모두 처리합니다.

## 더 자세한 설계 설명

현재 프로젝트의 AST는 일반적인 컴파일러처럼 완전한 트리 구조를 만들지는 않습니다.
대신 SQL을 먼저 “명령 종류” 단위로 분류합니다.

이렇게 한 이유:

```text
1. 과제 핵심은 SQL 전체 문법 구현이 아니라 B+Tree 인덱스 연결이다.
2. 그래도 문자열 if문이 executor 전체에 흩어지면 유지보수가 어렵다.
3. 그래서 입력 SQL을 한 번 정리하고 AstKind로 분류하는 얇은 AST 계층을 둔다.
4. 세부 문법은 각 명령 executor가 자신에게 필요한 범위만 파싱한다.
```

## sql_ast_parse()의 에러 처리

```text
입력 SQL이 너무 긴 경우:
    ast.sql 버퍼에 복사하면 overflow 위험이 있다.
    그래서 strlen(input) >= AST_SQL_MAX이면 즉시 false를 반환한다.
    executor는 이 false를 받아 Syntax Error와 실행 시간을 출력한다.

입력이 빈 문자열인 경우:
    오류가 아니라 EMPTY 명령으로 처리한다.
    사용자가 빈 줄을 입력해도 프롬프트가 계속 유지된다.

지원하지 않는 명령인 경우:
    UNSUPPORTED로 분류한다.
    실제 에러 출력은 execute_command()가 담당한다.
```

## 명령 분류 순서가 중요한 이유

```text
"CREATE UNIQUE INDEX"는 "CREATE"로도 시작하고 "CREATE INDEX"와 비슷하다.
그래서 더 긴 구문을 먼저 검사해야 한다.

예:
    1. CREATE UNIQUE INDEX 먼저 검사
    2. CREATE INDEX 검사

만약 반대로 하면 unique index 명령을 일반 index로 잘못 분류할 수 있다.
```

## AST와 QueryCondition의 관계

```text
SqlAst:
    SELECT인지 INSERT인지 같은 큰 명령 종류를 저장한다.

QueryCondition:
    WHERE id BETWEEN 1 AND 10 같은 조건 자체를 저장한다.

즉:
    SqlAst는 명령 레벨 AST
    QueryCondition은 WHERE 조건 레벨 AST
```
