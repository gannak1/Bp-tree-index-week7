# ast.c 수도코드

역할:

- 입력 SQL을 정리하고 AST 노드 트리를 생성합니다.
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
        ast.root = AST_EMPTY node
        true 반환

    classify_command(ast.sql)로 root kind 결정
    ast.root = AstNode(kind=ast.kind, text=ast.sql)

    명령 종류에 따라 자식 노드 생성:
        SELECT:
            SELECT_LIST
            TABLE
            INDEX_HINT
            WHERE
                CONDITION

        INSERT:
            TABLE
            COLUMN_LIST
            VALUE_LIST

        CREATE INDEX:
            TABLE
            COLUMN_LIST

        LOAD:
            PATH

        BENCHMARK:
            BENCHMARK_OPTIONS

    true 반환
```

이해 포인트:

- AST 단계에서는 명령 root와 주요 SQL 구성 요소를 노드로 만듭니다.
- 세부 타입 검사와 실행 오류는 각 executor/query 모듈이 더 정확한 위치와 함께 출력합니다.
- 대소문자 무시 비교를 직접 구현해서 `select`, `SELECT`, `SeLeCt` 모두 처리합니다.

## 더 자세한 설계 설명

현재 프로젝트의 AST는 SQL 명령과 핵심 구성 요소를 트리로 표현합니다.
다만 JOIN, GROUP BY, 복잡한 AND/OR 표현식까지 모두 다루는 범용 SQL AST는 아닙니다.

이렇게 한 이유:

```text
1. 과제 핵심은 SQL 전체 문법 구현이 아니라 B+Tree 인덱스 연결이다.
2. 그래도 문자열 if문이 executor 전체에 흩어지면 유지보수가 어렵다.
3. 그래서 입력 SQL을 한 번 정리하고 AstNode 트리를 만든다.
4. executor는 root kind로 실행 함수를 고르고, 필요하면 기존 세부 파서를 재사용한다.
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
    root node도 AST_UNSUPPORTED로 만든다.
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
    root node를 통해 SELECT/INSERT/WHERE/CONDITION 같은 SQL 구조를 저장한다.

QueryCondition:
    WHERE 조건을 실제 비교 가능한 lower/upper key 구조로 바꾼다.

즉:
    SqlAst/AstNode는 SQL 문법 구조 AST
    QueryCondition은 실행용 조건 표현
```

## SELECT AST 예시

```text
SQL:
    SELECT * FROM users FORCE INDEX (PRIMARY) WHERE id BETWEEN 1 AND 10;

AstNode tree:
    AST_SELECT text="SELECT * FROM users FORCE INDEX (PRIMARY) WHERE id BETWEEN 1 AND 10"
        AST_SELECT_LIST text="*"
        AST_TABLE text="users"
        AST_INDEX_HINT text="FORCE PRIMARY"
        AST_WHERE text="WHERE id BETWEEN 1 AND 10"
            AST_CONDITION text="id BETWEEN 1 AND 10"
```

## INSERT AST 예시

```text
SQL:
    INSERT INTO users (name, age, email) VALUES ('kim', 20, 'kim@test.com');

AstNode tree:
    AST_INSERT
        AST_TABLE "users"
        AST_COLUMN_LIST "name, age, email"
        AST_VALUE_LIST "'kim', 20, 'kim@test.com'"
```
