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

