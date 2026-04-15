# Learn Guide

이 폴더는 `src/` 아래 C 소스코드를 읽기 쉽게 풀어쓴 학습용 문서입니다.

각 파일은 실제 구현을 그대로 복사하지 않고, 핵심 흐름을 한국어 주석이 달린 수도코드로 정리합니다.

읽는 순서 추천:

1. `main.c.pseudo.md`
2. `engine.c.pseudo.md`
3. `ast.c.pseudo.md`, `ast.h.pseudo.md`
4. `executor.c.pseudo.md`
5. `query.c.pseudo.md`
6. `storage.c.pseudo.md`
7. `bptree.c.pseudo.md`
8. `util.c.pseudo.md`
9. `engine_internal.h.pseudo.md`, `engine.h.pseudo.md`

큰 흐름:

```text
main.c
  -> db_app_main()
  -> 시작 시 data/ 파일 로드
  -> REPL에서 SQL 입력
  -> AST로 명령 종류 분류
  -> executor가 명령 실행
  -> storage/table/B+Tree/query 모듈을 사용
  -> 종료 시 data/ 파일 저장
```

