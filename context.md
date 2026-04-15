# Context

## 프로젝트 목적

- 프로젝트명: `SQLInsertSelect`
- 구현 언어: `C`
- 목표: 파일 기반 SQL 처리기에 `id` 자동 증가, 메모리 기반 B+ 트리 인덱스, `BETWEEN` 문법, 대량 데이터 생성기를 추가한다.

## 현재 아키텍처

전체 흐름은 아래와 같다.

```text
입력 SQL
-> lexer
-> parser
-> AST
-> executor
-> meta 로드
-> binary data 읽기/쓰기
-> 결과 출력
```

저장 구조는 그대로 유지한다.

- 메타: `meta/<schema>/<table>.schema.csv`
- 데이터: `data/<schema>/<table>.dat`

예시 메타 파일:

```csv
column_name,type,size
id,INT,4
name,CHAR,20
age,INT,4
```

예시 row 레이아웃:

```text
[id 4바이트][name 20바이트][age 4바이트]
```

현재 `users`의 `row_size`는 `28` 바이트다.

## AST 구조

현재 파서는 노드 기반 AST를 만든다.

```c
typedef struct ASTNode {
    NodeType type;
    ASTValueType value_type;
    char *text;
    struct ASTNode *first_child;
    struct ASTNode *next_sibling;
} ASTNode;
```

주요 노드:

- `NODE_SELECT`
- `NODE_INSERT`
- `NODE_TABLE`
- `NODE_COLUMN_LIST`
- `NODE_VALUE_LIST`
- `NODE_WHERE`
- `NODE_OPERATOR`
- `NODE_BETWEEN`
- `NODE_IDENTIFIER`
- `NODE_VALUE`

### SELECT AST 예시

```text
NODE_SELECT
├── NODE_COLUMN_LIST
│   └── NODE_COLUMN("name")
├── NODE_TABLE
│   ├── NODE_IDENTIFIER("school")
│   └── NODE_IDENTIFIER("users")
└── NODE_WHERE
    ├── NODE_COLUMN("id")
    ├── NODE_OPERATOR("=")
    └── NODE_VALUE("1")
```

### INSERT AST 예시

```text
NODE_INSERT
├── NODE_TABLE
│   ├── NODE_IDENTIFIER("school")
│   └── NODE_IDENTIFIER("users")
└── NODE_VALUE_LIST
    ├── NODE_VALUE("Kim")
    └── NODE_VALUE("20")
```

### BETWEEN AST 예시

```text
NODE_WHERE
├── NODE_COLUMN("age")
└── NODE_BETWEEN
    ├── NODE_VALUE("20")
    └── NODE_VALUE("30")
```

## B+ 트리 연동 상태

이번 구현에서 `id` 전용 메모리 기반 B+ 트리를 추가했다.

- 키: `int id`
- 값: `long row_offset`
- 저장 위치: 메모리 전용
- 재구축 방식: 프로그램 시작 시 `.dat` 전체 스캔

즉 실제 row는 계속 바이너리 파일에 저장되고, 인덱스만 메모리에 유지된다.

### 실행 방식

- `WHERE id = <number>`
  - B+ 트리 사용
  - `id -> row_offset`를 찾은 뒤 해당 row 하나만 파일에서 읽음
- 그 외 WHERE
  - 기존 선형 탐색 사용
  - `.dat` 파일을 처음부터 끝까지 읽으며 검사

## INSERT 규칙

현재 `INSERT`는 `id`를 직접 입력받지 않는다.

예:

```sql
INSERT INTO users VALUES ('Kim', 20);
```

실행 순서:

1. 현재 row 수로 새 `id` 계산
2. row 버퍼 첫 컬럼에 `id` 기록
3. 입력값을 나머지 컬럼에 기록
4. `.dat` 파일 끝에 append
5. `id -> row_offset`를 B+ 트리에 삽입

## WHERE 지원 범위

현재 지원:

- `=`
- `!=`
- `>`
- `>=`
- `<`
- `<=`
- `BETWEEN ... AND ...`

제약:

- `BETWEEN`은 `INT` 컬럼만 지원
- `AND`, `OR`, 다중 조건, 괄호식은 미지원

## 주요 소스 파일 역할

- `src/lexer.c`
  - SQL 문자열을 토큰 배열로 분해
  - `BETWEEN`, `AND` 키워드 인식
- `src/parser.c`
  - 토큰 배열을 AST로 변환
  - `BETWEEN` 형태의 `WHERE` 생성
- `src/bptree.c`
  - `id -> row_offset`용 메모리 기반 B+ 트리 구현
- `src/storage.c`
  - 바이너리 row append
  - 인덱스 재구축
  - `id` 인덱스 검색과 선형 탐색 분기
- `src/executor.c`
  - 테이블 메타와 실행 컨텍스트 로드
  - AST 타입에 따라 `INSERT`/`SELECT` 실행
- `src/main.c`
  - 파일 모드 / REPL 진입점
  - SQL 파일을 줄 단위로 실행

## 생성기와 테스트

추가된 파일:

- `tools/generate_records.py`
  - 대량 INSERT SQL 생성기
- `tests/bptree_tests.c`
  - B+ 트리 삽입/검색과 `BETWEEN` 파서 테스트
- `sample_where_between.sql`
  - `BETWEEN` 샘플 쿼리

## 최근 구현 변경 요약

- `id` 자동 증가 추가
- `id` 전용 메모리 기반 B+ 트리 추가
- `WHERE id = ?` 인덱스 경로 추가
- `BETWEEN` 문법 추가
- 대량 INSERT SQL 생성기 추가
- 샘플 `users.dat`를 중복 `id` 없는 상태로 정리
- 루트 `Dockerfile`과 `.dockerignore` 추가

## Docker 실행 방식

도커 전용 실행을 위해 루트 `Dockerfile`을 추가했다.

- build stage에서 `sql_processor_bptree`를 컴파일한다.
- runtime stage에서 `/usr/local/bin/sql_processor_bptree`를 실행 파일로 사용한다.
- 기본 명령은 `--repl`이다.
- 컨테이너 내부 작업 디렉터리는 `/workspace`다.

예상 사용 방식:

```powershell
docker build -t sql-insert-select .
docker run --rm -it sql-insert-select
docker run --rm -it sql-insert-select sample_select.sql
docker run --rm -it -v ${PWD}:/workspace sql-insert-select sample_insert.sql
docker run --rm -it --entrypoint python3 -v ${PWD}:/workspace sql-insert-select /workspace/tools/generate_records.py --count 1000000 --output /workspace/generated_records.sql
```

## Dev Container 메모

- `.devcontainer/Dockerfile`을 Ubuntu 24.04 기준으로 정리했다.
- 기존 `RUN apt install clang-format` 줄은 비대화식 빌드에서 멈출 수 있어서 `apt-get install -y` 묶음으로 통합했다.
- 현재 사용자가 보고한 VS Code Dev Container 실패의 1차 원인은 Docker Desktop 빌드 과정에서 다음 경로 접근 권한이 막히는 문제다.
  - `C:\Users\gi676\.docker\config.json`
  - `C:\Users\gi676\.docker\buildx\instances`
- 즉 devcontainer가 막히는 원인은 저장소 코드만의 문제라기보다 로컬 Docker 설정 권한 문제와 겹쳐 있다.

## 현재 확인된 상태

- 메인 빌드 성공
- B+ 트리 단위 테스트 통과
- `SELECT * FROM users` 정상 동작
- `SELECT * FROM users WHERE id = 1` 정상 동작
- `SELECT * FROM users WHERE age BETWEEN 20 AND 30` 정상 동작
- `INSERT INTO verify.users VALUES ('Kim', 20)` 형태의 자동 `id` 부여 확인

## 남아 있는 메모

- `snprintf` 경로 길이 관련 경고가 일부 남아 있다.
- PowerShell 출력에서는 일부 한글 파일이 인코딩 문제로 깨져 보일 수 있다.
- 실제 소스/문서 파일은 UTF-8 기준으로 유지하는 방향이 안전하다.
