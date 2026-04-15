# AGENT.md

## 프로젝트 개요
이 프로젝트는 C 언어로 구현하는 파일 기반 SQL 처리기이다.

목표는 다음과 같다.

- SQL 파일 또는 CLI 입력으로 SQL 문장을 받는다.
- SQL 문장을 파싱한다.
- 파싱 결과를 실행한다.
- `INSERT`는 바이너리 데이터 파일에 row를 저장한다.
- `SELECT`는 바이너리 데이터 파일에서 row를 읽어 출력한다.

구현 우선순위는 학습보다 결과물 완성에 둔다.  
단, 핵심 로직은 팀원이 직접 설명할 수 있어야 한다.

---

## 전제 조건
- `schema`는 이미 존재한다고 가정한다.
- 각 `schema` 아래의 `table`도 이미 존재한다고 가정한다.
- `CREATE SCHEMA`, `CREATE TABLE`은 구현하지 않는다.
- SQL Processor는 기존 `schema.table`에 대해서만 `INSERT`, `SELECT`를 수행한다.

예:
- `school.users`
- `school.scores`

---

## 지원 범위

### 최소 지원 SQL
- `INSERT INTO schema.table VALUES (...)`
- `SELECT * FROM schema.table`
- `SELECT col1, col2 FROM schema.table`
- `SELECT ... FROM schema.table WHERE column operator value`

### 지원 비교 연산자
- `=`
- `!=`
- `>`
- `>=`
- `<`
- `<=`

### 비지원 범위
- `CREATE SCHEMA`
- `CREATE TABLE`
- `UPDATE`
- `DELETE`
- `JOIN`
- `ORDER BY`
- `GROUP BY`
- 다중 WHERE 조건
- 실제 인덱스 구현

---

## 저장 구조

### 핵심 원칙
- 실제 row 데이터는 바이너리 파일에 저장한다.
- 컬럼 설정 정보는 CSV 메타파일로 별도 저장한다.
- 프로그램은 메타파일을 먼저 읽고, 그 정보를 기준으로 바이너리 row를 해석한다.

### 디렉터리 예시
```text
meta/
  school/
    users.schema.csv

data/
  school/
    users.dat
```

### 메타파일 예시
`meta/school/users.schema.csv`

```csv
column_name,type,size
id,INT,4
name,CHAR,20
age,INT,4
```

의미:
- `id`는 4바이트 정수
- `name`은 20바이트 문자열
- `age`는 4바이트 정수

### 데이터 파일 예시
`data/school/users.dat`

이 파일은 사람이 읽는 텍스트가 아니라 실제 바이너리 row가 연속 저장되는 파일이다.

컬럼 정의가 아래와 같으면:

- `id : INT -> 4 bytes`
- `name : CHAR(20) -> 20 bytes`
- `age : INT -> 4 bytes`

row 하나의 크기는 총 28바이트다.

즉 데이터는 다음처럼 저장된다.

```text
[row1 28바이트][row2 28바이트][row3 28바이트]...
```

---

## 바이너리 저장 규칙

### 타입별 저장 방식
- `INT`
  - 4바이트 정수로 저장
- `CHAR(n)`
  - 정확히 `n`바이트 영역 사용
  - 문자열 길이가 짧으면 남는 공간은 `\0` 또는 공백으로 채움

예:
- `CHAR(20)`에 `"hi"` 저장 시
- 실제 저장은 20바이트 사용
- 나머지는 빈 값으로 채움

### row 저장 순서
각 row는 메타파일에 정의된 컬럼 순서대로 저장한다.

예:
- `id`
- `name`
- `age`

이면 row는 항상 아래 순서로 기록한다.

```text
[id 4바이트][name 20바이트][age 4바이트]
```

### row 크기 계산
row 크기는 메타파일의 `size` 합으로 계산한다.

예:
- `id = 4`
- `name = 20`
- `age = 4`

총 `row_size = 28`

---

## 내부 자료구조

### SQL 파싱용 자료구조
SQL 문자열은 lexer를 거쳐 토큰 배열이 되고, parser는 그 토큰들을 노드 기반 AST로 변환한다.

핵심 요소:
- `TokenType`
- `Token`
- `ASTValueType`
- `NodeType`
- `ASTNode`

### AST 구조 원칙
현재 구현은 노드 기반 AST를 사용한다.

예:
```text
SELECT
├── COLUMN_LIST
│   └── COLUMN("name")
├── TABLE
│   ├── IDENTIFIER("school")
│   └── IDENTIFIER("users")
└── WHERE
    ├── COLUMN("id")
    ├── OPERATOR("=")
    └── VALUE("1")
```

### 저장 실행용 자료구조
메타파일을 읽어 테이블 구조를 메모리로 올릴 때는 아래 구조를 사용한다.

```c
typedef enum {
    COL_INT,
    COL_CHAR
} ColumnType;

typedef struct {
    char name[32];
    ColumnType type;
    int size;
    int offset;
} ColumnDef;

typedef struct {
    char schema_name[32];
    char table_name[32];
    ColumnDef columns[16];
    int column_count;
    int row_size;
    char data_file_path[260];
} TableMeta;
```

설명:
- `name`: 컬럼명
- `type`: 컬럼 타입
- `size`: 바이트 크기
- `offset`: row 내부 시작 위치
- `row_size`: row 전체 크기

---

## 실행 흐름

### INSERT
1. SQL을 파싱해 `NODE_INSERT` 루트 AST 생성
2. `NODE_TABLE`에서 `schema.table` 추출
3. 메타 CSV 파일 읽기
4. 메타정보로 컬럼 개수와 타입 확인
5. `NODE_VALUE_LIST`를 읽어 row 버퍼 생성
6. 각 컬럼 순서에 맞게 바이너리 값 기록
7. `.dat` 파일 끝에 append

### SELECT
1. SQL을 파싱해 `NODE_SELECT` 루트 AST 생성
2. `NODE_TABLE`에서 `schema.table` 추출
3. 메타 CSV 파일 읽기
4. `row_size` 계산
5. `.dat` 파일에서 `row_size` 단위로 반복 읽기
6. 각 row에서 컬럼별 값을 복원
7. `NODE_WHERE`가 있으면 조건 검사
8. 통과한 row만 출력

---

## WHERE 동작 방식
기본 구현에서 `WHERE`는 단일 조건만 지원한다.

예:
```sql
SELECT name FROM school.users WHERE id = 3;
SELECT * FROM users WHERE age >= 10;
```

동작:
- 메타파일에서 비교 컬럼의 offset과 size를 찾는다.
- 각 row를 읽는다.
- 해당 offset 위치의 값을 꺼내 연산자에 맞게 비교한다.
- 일치하면 선택 컬럼만 출력한다.

즉 기본 구현은 인덱스 없이 row를 처음부터 끝까지 검사하는 순차 탐색 방식이다.

주의:
- 실제 DB는 빠른 검색을 위해 `B-Tree` 인덱스를 많이 사용한다.
- 이번 프로젝트는 구현 단순성을 위해 인덱스 없는 순차 탐색으로 처리한다.

---

## 메모리 사용 원칙
- 전체 테이블을 메모리에 모두 올리지 않는다.
- 메타정보(`TableMeta`)는 메모리에 유지한다.
- 데이터 파일은 row 하나씩 읽는다.
- 현재 SQL 1개와 현재 row 1개만 중심으로 처리한다.

즉 프로그램 재시작 후에도 정보가 유지되려면:
- 컬럼명
- 타입
- 크기

이 정보는 반드시 메타 CSV 파일에 저장되어 있어야 한다.

---

## CLI 규격

### 파일 실행 모드
```bash
sql_processor sample.sql
```

### REPL 모드
```bash
sql_processor --repl
```

REPL에서는 SQL을 한 문장씩 입력받아 실행하고, `exit` 또는 `quit` 입력 시 종료한다.

---

## 출력 규칙
- INSERT 성공 시:
  - `1 row inserted.`
- SELECT 성공 시:
  - 선택 컬럼 헤더 출력
  - 결과 row 출력
  - 마지막에 `N rows selected.`
- 오류 발생 시:
  - `Parse error`
  - `Schema error`
  - `Table error`
  - `Execution error`

예:
- `Schema error: column 'age2' does not exist`
- `Table error: table 'school.users' not found`

---

## 구현 우선순위
1. lexer 구현
2. parser 구현
3. 노드 기반 AST 구성
4. 메타 CSV 파일 로더 구현
5. 바이너리 row 저장 구현
6. `INSERT` 구현
7. 바이너리 row 읽기 구현
8. `SELECT *` 구현
9. 특정 컬럼 선택 구현
10. `WHERE column operator value` 구현
11. REPL 추가
12. 에러 메시지 개선

---

## 테스트 체크리스트

### 메타파일 테스트
- 메타 CSV 파일 정상 로딩
- 컬럼 수 계산
- offset 계산
- row_size 계산

### INSERT 테스트
- 정상 row 저장
- 값 개수 불일치 오류
- 타입 불일치 오류
- 없는 table 오류

### SELECT 테스트
- 전체 row 조회
- 특정 컬럼 조회
- WHERE 조건 조회
- 없는 컬럼 오류
- `=`, `!=`, `>`, `>=`, `<`, `<=` 비교 확인

### 바이너리 읽기 테스트
- `INT` 복원 정상 동작
- `CHAR(n)` 복원 정상 동작
- 짧은 문자열 padding 처리 확인

---

## 팀 작업 원칙
- AI가 생성한 코드라도 반드시 직접 읽고 이해한다.
- 핵심 로직은 팀원이 직접 설명할 수 있어야 한다.
- 구현 범위를 벗어나는 기능은 확장 아이디어로만 둔다.
- 현재 버전은 바이너리 기반 미니 SQL 처리기이며, 실제 DB 전체를 재현하는 것이 목표는 아니다.

---

## 발표용 핵심 문장
이 프로젝트는 `schema` 아래에 이미 존재하는 `table`을 대상으로, SQL 문장을 노드 기반 AST로 파싱한 뒤 컬럼 메타정보는 CSV로, 실제 row 데이터는 바이너리로 관리하여 `INSERT`와 `SELECT`를 수행하는 미니 SQL 처리기이다.
