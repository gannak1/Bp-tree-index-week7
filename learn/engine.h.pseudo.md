# engine.h 수도코드

역할:

- 외부에서 `engine` 모듈을 사용할 때 필요한 공개 함수만 선언합니다.
- 현재 공개 API는 `db_app_main()` 하나입니다.

수도코드:

```text
헤더 보호 시작
    // 같은 헤더가 여러 번 include되어도 중복 선언되지 않게 막음

함수 선언 db_app_main(argc, argv) -> int
    // main.c에서 호출하는 앱 실행 함수
    // argc, argv는 명령행 인자
    // 반환값은 프로그램 종료 코드

헤더 보호 종료
```

이해 포인트:

- `engine.h`는 외부 공개용입니다.
- 내부 자료구조와 세부 함수는 `engine_internal.h`에 숨겨져 있습니다.

