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

## 예시 SQL이 흘러가는 전체 경로

아래 SQL을 입력했다고 가정합니다.

```sql
SELECT * FROM users FORCE INDEX (PRIMARY) WHERE id BETWEEN 100 AND 200;
```

실행 흐름은 다음과 같습니다.

```text
1. engine.c
   run_repl()
     // 사용자가 입력한 여러 줄을 모아서 하나의 SQL 문자열로 만든다.
     // 세미콜론을 만나면 명령 하나가 끝났다고 판단한다.

2. executor.c
   execute_command()
     // ast.c의 sql_ast_parse()를 호출한다.
     // AST 결과가 AST_SELECT인지 확인한다.
     // SELECT라면 execute_select()로 넘긴다.

3. ast.c
   sql_ast_parse()
     // SQL 앞부분을 보고 SELECT 명령이라는 사실만 분류한다.
     // 세부 WHERE 조건은 여기서 파싱하지 않는다.

4. executor.c
   execute_select()
     // FROM users 확인
     // FORCE INDEX (PRIMARY) 힌트 파싱
     // WHERE 뒤 문자열을 query.c의 parse_condition()으로 넘김

5. query.c
   parse_condition()
     // id BETWEEN 100 AND 200을 QueryCondition으로 변환한다.
     // lower = 100 inclusive
     // upper = 200 inclusive
     // column = id

6. executor.c
   execute_select()
     // FORCE INDEX로 지정한 PRIMARY 인덱스를 찾는다.
     // WHERE 컬럼 id와 PRIMARY 인덱스 컬럼 id가 맞는지 확인한다.
     // 조건이 = 이 아니므로 B+ Tree Range Scan을 선택한다.

7. query.c + bptree.c
   bplus_tree_collect_range()
     // lower key 100이 위치할 leaf를 bptree.c에서 찾는다.
     // leaf의 next 포인터를 따라가며 200 이하 key를 결과에 담는다.

8. query.c
   print_record_table()
     // 결과 rows를 표 형태로 출력한다.
     // 실행 시간, Access Type, Index Used도 같이 출력한다.
```

## 문서를 읽을 때 기준

- `executor.c.pseudo.md`는 SQL 명령이 어떻게 실행되는지 중심으로 읽습니다.
- `query.c.pseudo.md`는 WHERE 조건이 어떻게 내부 조건 객체로 바뀌는지 중심으로 읽습니다.
- `bptree.c.pseudo.md`는 B+Tree가 왜 빠른지, leaf 연결이 범위 검색에 어떻게 쓰이는지 중심으로 읽습니다.
- `storage.c.pseudo.md`는 파일 저장 구조와 프로그램 재시작 시 복구 흐름을 중심으로 읽습니다.
- `engine_internal.h.pseudo.md`는 전체 자료구조 관계도를 보는 용도로 읽습니다.
