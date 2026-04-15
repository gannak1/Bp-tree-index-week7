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

## 공개 헤더와 내부 헤더를 나눈 이유

```text
engine.h:
    main.c가 알아야 하는 최소 정보만 제공한다.
    현재는 db_app_main() 선언 하나만 있다.

engine_internal.h:
    Database, Table, BPlusTree 같은 내부 자료구조를 제공한다.
    executor.c, storage.c, query.c 등 내부 모듈끼리 공유한다.

이렇게 나누는 이유:
    외부에서 DB 내부 구조를 직접 만지지 못하게 한다.
    main.c는 Database 구조체가 어떻게 생겼는지 알 필요가 없다.
```

## 헤더 보호 코드 의미

```text
#ifndef MYSQL_BPTREE_ENGINE_H
#define MYSQL_BPTREE_ENGINE_H
...
#endif

의미:
    같은 헤더가 여러 파일에서 여러 번 include되어도
    함수 선언이 중복 처리되지 않게 막는다.

C 프로젝트에서는 거의 모든 헤더에 이런 보호 코드가 필요하다.
```
