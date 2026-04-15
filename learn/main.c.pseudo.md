# main.c 수도코드

역할:

- 프로그램의 가장 바깥 진입점입니다.
- 실제 DB 로직은 `engine.c`의 `db_app_main()`으로 넘깁니다.
- `main.c`를 작게 유지해서 코드 구조를 깔끔하게 만듭니다.

수도코드:

```text
함수 main(argc, argv):
    // 운영체제가 프로그램을 실행하면 가장 먼저 호출되는 함수

    결과 = db_app_main(argc, argv)
        // 실제 초기화, REPL 실행, self-test 처리는 engine.c에 위임

    return 결과
        // 프로그램 종료 코드를 운영체제에 전달
```

이해 포인트:

- `main.c`에는 비즈니스 로직을 넣지 않습니다.
- 실행 흐름만 `engine` 모듈로 연결합니다.
- 이렇게 하면 나중에 CLI 외의 다른 실행 방식을 붙여도 구조가 덜 흔들립니다.

## 왜 main.c를 작게 유지하는가

```text
나쁜 구조:
    main.c 안에
        - 파일 로드
        - SQL 파싱
        - B+Tree 구현
        - REPL
        - 테스트
    를 모두 넣는다.

문제:
    파일이 너무 커진다.
    기능별 수정 위치를 찾기 어렵다.
    테스트와 실행 흐름이 섞인다.

현재 구조:
    main.c는 db_app_main()만 호출한다.
    실제 작업은 engine.c부터 시작한다.
```

## 실행 인자가 전달되는 이유

```text
main(argc, argv):
    argc:
        명령행 인자 개수

    argv:
        명령행 인자 문자열 배열

예:
    ./mysql_bptree --self-test

    argc = 2
    argv[0] = "./mysql_bptree"
    argv[1] = "--self-test"

db_app_main()은 이 인자를 보고:
    일반 REPL을 실행할지
    self-test를 실행할지 결정한다.
```
