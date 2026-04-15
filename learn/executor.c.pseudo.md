# executor.c 수도코드

역할:

- AST가 분류한 SQL 명령을 실제로 실행합니다.
- INSERT, SELECT, CREATE INDEX, ALTER PRIMARY KEY, LOAD, SAVE, BENCHMARK를 담당합니다.

명령 분기:

```text
함수 execute_command(db, input):
    begin = 현재 시간

    ast = sql_ast_parse(input)
        // ast.c에서 명령 종류 분류

    만약 ast가 EMPTY라면:
        EXEC_OK 반환

    ast.kind에 따라 실행:
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

