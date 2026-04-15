# engine.c 수도코드

역할:

- 앱 실행 흐름을 담당합니다.
- 시작 시 데이터 파일을 로드합니다.
- `mysql-bptree>` 프롬프트를 띄우고 SQL을 입력받습니다.
- 입력된 SQL은 `execute_command()`로 넘깁니다.
- 종료 시 데이터를 저장하고 메모리를 해제합니다.

수도코드:

```text
함수 run_repl(db):
    command = 빈 문자열

    무한 반복:
        "mysql-bptree> " 출력
        한 줄 입력 받기

        입력이 끝났다면:
            반복 종료

        command에 입력 줄을 이어 붙임
            // 여러 줄 SQL도 받을 수 있게 하기 위함

        만약 세미콜론이 없고 EXIT/QUIT도 아니라면:
            계속 입력 받기
            // SQL 명령이 아직 끝나지 않았다고 판단

        status = execute_command(db, command)
            // AST 분류와 실제 명령 실행은 executor.c 담당

        command 초기화

        만약 status가 EXEC_EXIT라면:
            return 0

    EOF로 끝난 경우:
        현재 DB를 저장
        return 0
```

```text
함수 run_self_test():
    db 초기화
    db.persist = false
        // 테스트 중에는 실제 data 파일에 저장하지 않음

    PRIMARY 인덱스 메타데이터 추가
    인덱스 재생성

    테스트 SQL 목록을 순서대로 실행:
        INSERT
        SELECT FORCE INDEX
        SELECT IGNORE INDEX
        CREATE INDEX
        EXPLAIN
        BENCHMARK
        범위 검색

    db 메모리 해제
    return 0
```

```text
함수 db_app_main(argc, argv):
    만약 argv[1] == "--self-test"라면:
        return run_self_test()

    db 초기화

    시작 시간 기록
    data/ 아래 schema, index, binary data, meta 파일 로드
        // 로드 후 메모리 B+Tree 인덱스를 다시 구성

    REPL 실행

    db 메모리 해제
    return REPL 결과
```

이해 포인트:

- `engine.c`는 이제 “앱 진행자” 역할만 합니다.
- SQL 문법 처리, 저장소 처리, B+Tree 처리는 다른 파일로 분리되어 있습니다.
- `--self-test`는 포트폴리오 시연이나 회귀 테스트에 유용합니다.

## 함수별 상세 설명

### run_repl(db)

```text
목적:
    사용자가 SQL을 입력하고 결과를 바로 확인할 수 있는 명령 프롬프트를 제공한다.

입력:
    db
        // 이미 초기화되고, 필요하면 data/에서 로드된 Database 객체

내부 변수:
    line
        // fgets로 한 줄씩 읽는 임시 버퍼

    command
        // 여러 줄 SQL을 하나로 합치는 누적 버퍼

상세 흐름:
    1. 프롬프트 출력
        "mysql-bptree> "

    2. 사용자 입력 한 줄 읽기
        만약 EOF라면:
            사용자가 Ctrl-D를 눌렀거나 입력이 종료된 상태
            반복을 끝낸다.

    3. command 길이 검사
        누적된 SQL이 SQL_BUF_SIZE를 넘으면:
            Syntax Error 출력
            command 초기화
            다음 입력을 받는다.

    4. 입력 줄을 command에 붙인다.

    5. 명령 종료 여부 판단
        세미콜론이 없고, EXIT/QUIT도 아니라면:
            아직 명령이 완성되지 않았다고 보고 계속 입력받는다.

    6. execute_command(db, command) 호출
        실제 SQL 분류와 실행은 executor.c가 처리한다.

    7. EXEC_EXIT가 반환되면 REPL을 끝낸다.

    8. 일반 명령이면 command를 비우고 다음 프롬프트를 출력한다.

EOF 처리:
    세미콜론 없이 입력 스트림이 끝난 경우에도 현재 데이터는 저장한다.
    그래서 run_repl 마지막에서 execute_save()를 호출한다.
```

### run_self_test()

```text
목적:
    구현한 기능이 기본적으로 동작하는지 빠르게 검증한다.

중요 설정:
    db.persist = false
        // self-test는 실제 data/users.data를 오염시키면 안 된다.
        // 그래서 메모리에서만 테스트하고 종료 시 날린다.

테스트 범위:
    - 컬럼 순서가 다른 INSERT
    - PRIMARY 인덱스 검색
    - IGNORE INDEX로 선형 탐색 강제
    - 보조 인덱스 생성
    - name 컬럼 인덱스 검색
    - EXPLAIN 출력
    - BENCHMARK 데이터 생성
    - id >, id <=, id BETWEEN 범위 검색
    - age 컬럼 범위 인덱스 검색
```

### db_app_main(argc, argv)

```text
목적:
    프로그램의 실제 main 역할을 한다.

상세 흐름:
    1. "--self-test" 인자가 있으면 run_self_test() 실행

    2. Database db 생성 후 db_init()
        schema 기본값, table 초기값, persist 설정이 이루어진다.

    3. db_load_startup()
        data/ 폴더 생성
        schema CSV 로드
        indexes CSV 로드
        binary data 로드
        meta 로드
        B+Tree 인덱스 재구성

    4. run_repl()
        사용자가 EXIT할 때까지 SQL 입력 처리

    5. db_clear()
        동적 할당한 record, index tree 등을 해제

    6. 종료 코드 반환
```

설계 이유:

- 앱 실행 흐름과 SQL 실행 로직을 분리하면, 나중에 테스트나 다른 UI를 붙이기 쉽습니다.
- REPL은 문자열을 모으는 역할만 하고, SQL 의미 해석은 executor/AST/query에 맡깁니다.
