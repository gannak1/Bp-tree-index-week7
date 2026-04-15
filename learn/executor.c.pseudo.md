# executor.c 수도코드

역할:

- AST가 만든 root node의 명령 종류를 실제 DB 동작으로 실행합니다.
- INSERT, SELECT, CREATE INDEX, ALTER PRIMARY KEY, LOAD, SAVE, BENCHMARK를 담당합니다.

명령 분기:

```text
함수 execute_command(db, input):
    begin = 현재 시간

    ast = sql_ast_parse(input)
        // ast.c에서 AstNode 트리 생성

    command_kind = ast.root.kind

    만약 command_kind가 EMPTY라면:
        AST 메모리 해제
        EXEC_OK 반환

    command_kind에 따라 실행:
        EXIT:
            저장 후 EXEC_EXIT 반환

        INSERT:
            execute_insert()

        SELECT:
            execute_select(explain = false)

        EXPLAIN:
            execute_select(explain = true)

        SHOW_INDEX:
            execute_show_index()

        CREATE_INDEX:
            execute_create_index(unique = false)

        CREATE_UNIQUE_INDEX:
            execute_create_index(unique = true)

        DROP_INDEX:
            execute_drop_index()

        ALTER_PRIMARY_KEY:
            execute_alter_primary()

        SAVE:
            execute_save()

        LOAD_SCHEMA:
            execute_load_schema()

        LOAD_DATA_BINARY:
            execute_load_data_binary()

        BENCHMARK:
            execute_benchmark()

        그 외:
            Syntax Error 출력

    sql_ast_free(ast)
        // AST 노드 트리 메모리 해제
```

INSERT 실행:

```text
함수 execute_insert(db, sql, begin):
    "INSERT INTO users (...)" 구조 확인
    컬럼 리스트와 값 리스트 괄호 찾기
    컬럼 수와 값 수 비교

    새 Record 할당
    record.id = next_auto_increment_id

    각 컬럼에 대해:
        컬럼명이 유효한지 확인
        id 직접 삽입은 거부
        중복 컬럼 거부
        name/email은 문자열로 저장
        age는 INT 타입 체크 후 저장

    필수 컬럼이 없거나 비어 있으면 에러

    모든 active index에 먼저 삽입 가능한지 확인
        // unique index 중복이면 실패

    table에 record 추가
    성공 메시지와 id, 실행 시간 출력
```

SELECT 실행:

```text
함수 execute_select(db, sql, begin, explain):
    EXPLAIN이면 SELECT 부분만 추출

    FROM users 확인
    FORCE INDEX 또는 IGNORE INDEX 힌트 파싱
    WHERE 조건 파싱

    사용할 인덱스 결정:
        FORCE INDEX가 있으면 해당 인덱스 사용 시도
        IGNORE INDEX가 있으면 해당 인덱스 제외
        조건 컬럼에 active index가 있으면 자동 사용
        없으면 Full Table Scan

    explain == true라면:
        실행 계획만 출력
        종료

    인덱스를 사용할 수 있으면:
        조건이 '='이면 B+Tree 단일 검색
        조건이 범위이면 B+Tree range scan

    인덱스를 못 쓰면:
        모든 record를 순회하며 조건 검사

    결과 표 출력
```

CREATE INDEX:

```text
함수 execute_create_index(db, sql, unique):
    인덱스 이름 파싱
    ON users (column) 파싱
    같은 이름의 인덱스가 있으면 에러
    컬럼명이 유효한지 확인

    index metadata 추가
    B+Tree 생성 후 기존 모든 row를 삽입

    unique 중복이 발견되면:
        생성 실패 처리

    persist 모드라면 indexes.csv 저장
```

ALTER PRIMARY KEY:

```text
함수 execute_alter_primary(db, sql):
    PRIMARY KEY (column) 파싱
    해당 컬럼이 primary key로 가능한지 검사
        값이 비어 있으면 안 됨
        중복 값이 있으면 안 됨

    schema의 primary_column 변경
    PRIMARY 인덱스 column 변경
    PRIMARY B+Tree 재생성

    id가 primary key가 아니게 되면:
        id 보조 인덱스가 없을 경우 생성

    schema/index metadata 저장
```

BENCHMARK:

```text
함수 execute_benchmark(db, sql):
    row count 파싱
    기존 생성 데이터 초기화

    1부터 N까지 반복:
        id = i
        name = "user{i}"
        age = 18 + (i % 50)
        email = "user{i}@test.com"
        table에 record 추가

    INDEX (...) 옵션이 있으면:
        해당 컬럼 인덱스 보장

    모든 인덱스 재생성
    persist 모드라면 data/schema/index/meta 저장

    id 기준 full scan 시간 측정
    id 기준 B+Tree 검색 시간 측정
    비교 결과 출력
```

이해 포인트:

- `executor.c`는 프로젝트의 “SQL 실행 엔진”입니다.
- 실제 B+Tree 구현은 `bptree.c`, 저장/로드는 `storage.c`, WHERE 파싱은 `query.c`에 맡깁니다.
- 모든 명령은 실행 시간과 결과를 함께 출력하도록 구성되어 있습니다.

## execute_select()를 더 자세히 보기

예시:

```sql
SELECT * FROM users FORCE INDEX (PRIMARY) WHERE id BETWEEN 100 AND 200;
```

상세 흐름:

```text
1. EXPLAIN 여부 처리
    execute_select()는 SELECT와 EXPLAIN SELECT를 같이 처리한다.
    explain=true이면 실제 row를 출력하지 않고 실행 계획만 출력한다.

2. FROM users 확인
    현재 구현은 users 테이블만 지원한다.
    FROM users가 없으면 Syntax Error를 출력한다.

3. 인덱스 힌트 확인
    FORCE INDEX (PRIMARY)가 있으면:
        hint_name = "PRIMARY"
        force = true

    IGNORE INDEX (PRIMARY)가 있으면:
        hint_name = "PRIMARY"
        ignore = true

4. WHERE 위치 찾기
    WHERE가 없으면 현재 구현 범위에서는 오류로 처리한다.
    WHERE 뒤 문자열을 parse_condition()으로 넘긴다.

5. QueryCondition 생성
    query.c가 id BETWEEN 100 AND 200을 다음처럼 변환한다.
        column = COL_ID
        op = OP_BETWEEN
        lower = 100 inclusive
        upper = 200 inclusive

6. 사용할 index 결정
    FORCE INDEX가 있으면:
        이름으로 index를 찾는다.
        index가 없으면 Index Error
        index.column과 WHERE column이 다르면 Index Error

    FORCE INDEX가 없고 IGNORE INDEX도 아니면:
        WHERE column에 맞는 active index를 자동으로 찾는다.

    IGNORE INDEX가 있으면:
        해당 이름의 index는 사용하지 않는다.

7. access type 결정
    index 사용 가능 + op == '=':
        "B+ Tree Index Scan"

    index 사용 가능 + 범위 조건:
        "B+ Tree Range Scan"

    index 사용 불가:
        "Full Table Scan"

8. EXPLAIN이면:
    access type, index name, column, estimated cost를 출력하고 종료한다.

9. 실제 실행
    B+Tree Index Scan:
        bplus_tree_search(index.tree, cond.lower)

    B+Tree Range Scan:
        bplus_tree_collect_range(index.tree, cond, result)

    Full Table Scan:
        모든 table.rows[i]에 대해 record_matches_condition() 검사

10. 결과 출력
    print_record_table()
        row 표
        실행 시간
        Access Type
        Index Used
```

## INSERT에서 인덱스 중복을 처리하는 방식

```text
문제:
    INSERT할 때 table에는 추가했는데 unique index 삽입이 실패하면
    table과 index 상태가 불일치할 수 있다.

현재 흐름:
    1. Record를 먼저 메모리에 만든다.
    2. 값 타입과 NOT NULL을 검사한다.
    3. active index들에 index_insert_record()를 시도한다.
    4. 모두 성공하면 table_add_record()로 table에 추가한다.

의미:
    unique index 중복이 있으면 table에 들어가기 전에 실패한다.

주의:
    이 프로젝트는 과제용 단순 DBMS라 완전한 transaction rollback까지 구현하지는 않는다.
    하지만 unique duplicate를 table 추가 전에 잡도록 설계했다.
```

## CREATE INDEX 상세 흐름

```text
입력:
    CREATE INDEX idx_users_name ON users (name);

흐름:
    1. index name 파싱
        idx_users_name

    2. ON users (column) 구문 확인
        table은 users만 허용
        column은 id/name/age/email 중 하나여야 함

    3. 같은 index name이 이미 있는지 확인
        있으면 Index Error

    4. add_index_meta()
        indexes 배열에 새 IndexMetaRuntime 추가
        아직 B+Tree는 비어 있음

    5. rebuild_one_index()
        기존 table.rows 전체를 순회하며 새 B+Tree에 삽입

    6. unique index이면:
        같은 key가 발견될 때 실패

    7. persist 모드이면:
        users.indexes.csv 저장
```

## ALTER PRIMARY KEY 상세 흐름

```text
입력:
    ALTER TABLE users MODIFY PRIMARY KEY (email);

흐름:
    1. PRIMARY KEY (...) 안의 컬럼명 파싱

    2. primary_candidate_valid()
        모든 row를 검사한다.

        조건:
            값이 비어 있으면 안 됨
            중복 값이 있으면 안 됨

    3. schema.primary_column 변경

    4. PRIMARY index metadata 변경
        index_name = PRIMARY
        column = email
        unique = true
        primary = true

    5. PRIMARY B+Tree 재생성

    6. id가 더 이상 primary가 아니면:
        id 검색 성능 보존을 위해 idx_users_id 보조 인덱스를 보장한다.

    7. schema/index 파일 저장
```

## BENCHMARK 상세 흐름

```text
입력:
    BENCHMARK 1000000 INDEX (id, name, age);

흐름:
    1. row count 파싱
        1000000

    2. 기존 generated data 초기화
        table row 제거
        auto_increment 초기화

    3. 1부터 N까지 Record 생성
        id = i
        name = "user{i}"
        age = 18 + (i % 50)
        email = "user{i}@test.com"

    4. INDEX 옵션 파싱
        id, name, age 컬럼에 인덱스가 없으면 생성

    5. rebuild_indexes()
        모든 인덱스 B+Tree 재구성

    6. persist 모드이면 파일 저장
        users.data
        users.schema.csv
        users.indexes.csv
        users.meta

    7. 성능 비교
        Full Table Scan으로 id 하나 찾는 시간 측정
        PRIMARY B+Tree로 같은 id 찾는 시간 측정

    8. 결과 출력
        데이터 생성 시간
        메모리 insert 시간
        바이너리 저장 시간
        index build 시간
        full scan select 시간
        B+Tree select 시간
```

## 에러 출력 방식

```text
모든 주요 문법/타입 오류는 print_error_timed()를 사용한다.

출력 내용:
    - Error 종류
    - 메시지
    - SQL 원문
    - 문제가 발생한 위치를 ^로 표시
    - 실행 시간

예:
    Type Error: Column 'age' expects INT, but got 'abc'.

    INSERT INTO users (name, age, email) VALUES ('kim', 'abc', 'a@b.com')
                                           ^^^^^

    Execution time: 0.000012 sec
```
