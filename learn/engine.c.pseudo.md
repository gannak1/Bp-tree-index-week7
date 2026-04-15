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

