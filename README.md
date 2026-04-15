# B+ Tree Mini DBMS

C 언어로 구현한 MySQL 스타일 명령 프롬프트 기반 미니 DBMS입니다.

`users` 테이블에 레코드를 저장하고, `id`, `name`, `age`, `email` 컬럼에 대해 B+Tree 인덱스를 생성하거나 사용할 수 있습니다. `WHERE id = ?` 같은 단일 검색뿐 아니라 `<`, `<=`, `>`, `>=`, `BETWEEN` 범위 검색도 지원합니다.

## 주요 기능

- MySQL 스타일 CLI 프롬프트 제공
- `INSERT`, `SELECT`, `CREATE INDEX`, `DROP INDEX`, `SHOW INDEX`, `EXPLAIN`, `BENCHMARK` 지원
- `FORCE INDEX`, `IGNORE INDEX`를 이용한 B+Tree 검색과 선형 탐색 비교
- 컬럼 순서가 바뀐 `INSERT` 처리
- 타입 체크 및 SQL 오류 위치 표시
- Primary Key 컬럼 변경 지원
- 스키마는 CSV, 실제 레코드는 바이너리 파일로 저장
- 프로그램 시작 시 `data/` 파일을 읽고 메모리 B+Tree 인덱스 재구성
- Ubuntu Docker 환경 지원
- 소스코드 이해를 위한 `learn/` 수도코드 문서 제공

## 저장 구조

프로그램은 프로젝트 루트의 `data/` 폴더를 기본 데이터베이스 디렉터리로 사용합니다.

```text
data/
├── users.schema.csv    # 컬럼, 타입, PK, auto_increment 등 스키마 정보
├── users.indexes.csv   # 인덱스 메타데이터
├── users.data          # 실제 레코드 데이터, 바이너리 형식
└── users.meta          # 레코드 개수, 다음 auto_increment id 등 메타정보
```

실제 B+Tree 구조는 파일에 직접 저장하지 않습니다. 프로그램이 시작될 때 바이너리 레코드와 인덱스 메타데이터를 읽은 뒤, 메모리에서 B+Tree를 다시 구성합니다.

## 소스 구조

```text
src/
├── main.c              # 프로그램 진입점
├── engine.c            # REPL 루프, self-test, 앱 시작/종료 흐름
├── engine.h            # 외부 공개 엔트리포인트
├── engine_internal.h   # 내부 공통 타입, 상수, 함수 선언
├── ast.c / ast.h       # SQL 명령 AST 분류
├── executor.c          # SQL 명령 실행
├── query.c             # WHERE 파싱, 결과 출력, range scan 보조 로직
├── storage.c           # schema/index/data/meta 저장 및 로드
├── bptree.c            # B+Tree 삽입, 검색, leaf 탐색
└── util.c              # 문자열, 시간 측정, 키 비교, 에러 출력 유틸
```

학습용 수도코드는 `learn/` 폴더에 있습니다.

## 로컬 빌드 및 실행

```sh
make
./mysql_bptree
```

실행하면 다음 프롬프트가 출력됩니다.

```text
mysql-bptree>
```

## Docker 실행

Ubuntu 기반 Docker 이미지로 실행할 수 있습니다.

```sh
docker build -t mysql-bptree .
make docker-run
```

`make docker-run`은 로컬 프로젝트의 `data/` 폴더를 컨테이너 내부 `/app/data`에 마운트합니다. 따라서 Docker 안에서 삽입하거나 벤치마크로 생성한 데이터가 로컬 `data/` 폴더에 남습니다.

동일한 raw Docker 명령어는 다음과 같습니다.

```sh
mkdir -p data
docker run --rm -it -v "$PWD/data:/app/data" mysql-bptree
```

## 기본 명령어

```sql
INSERT INTO users (name, age, email) VALUES ('kim', 20, 'kim@test.com');
SELECT * FROM users WHERE id = 1;
SELECT * FROM users WHERE id > 100;
SELECT * FROM users WHERE id <= 1000;
SELECT * FROM users WHERE id BETWEEN 100 AND 200;
```

## 인덱스 사용 비교

B+Tree 인덱스를 강제로 사용합니다.

```sql
SELECT * FROM users FORCE INDEX (PRIMARY) WHERE id = 1;
SELECT * FROM users FORCE INDEX (PRIMARY) WHERE id BETWEEN 100 AND 200;
```

인덱스를 무시하고 선형 탐색으로 검색합니다.

```sql
SELECT * FROM users IGNORE INDEX (PRIMARY) WHERE id = 1;
SELECT * FROM users IGNORE INDEX (PRIMARY) WHERE id >= 100;
```

## 인덱스 관리

```sql
CREATE INDEX idx_users_name ON users (name);
CREATE UNIQUE INDEX idx_users_email_unique ON users (email);
DROP INDEX idx_users_name ON users;
SHOW INDEX FROM users;
```

Primary Key 컬럼도 변경할 수 있습니다.

```sql
ALTER TABLE users MODIFY PRIMARY KEY (email);
```

## 실행 계획 확인

```sql
EXPLAIN SELECT * FROM users WHERE id = 1;
EXPLAIN SELECT * FROM users WHERE id BETWEEN 100 AND 200;
```

인덱스를 사용하는 경우 `B+ Tree Index Scan` 또는 `B+ Tree Range Scan`이 출력됩니다. 인덱스를 사용하지 못하면 `Full Table Scan`이 출력됩니다.

## 100만 건 벤치마크

```sql
BENCHMARK 1000000 INDEX (id, name, age);
```

벤치마크 데이터는 다음 형식으로 생성됩니다.

```text
id    = 1, 2, 3, ...
name  = user{id}
age   = 18 + (id % 50)
email = user{id}@test.com
```

벤치마크 실행 후에는 데이터 생성 시간, 바이너리 저장 시간, 인덱스 생성 시간, 선형 탐색 SELECT 시간, B+Tree SELECT 시간이 함께 출력됩니다.

## 저장 및 로드

현재 데이터를 명시적으로 저장합니다.

```sql
SAVE;
```

외부 스키마와 바이너리 데이터 파일을 직접 로드할 수도 있습니다.

```sql
LOAD SCHEMA 'data/users.schema.csv' INTO TABLE users;
LOAD DATA BINARY 'data/users.data' INTO TABLE users REPLACE;
LOAD DATA BINARY 'data/users.data' INTO TABLE users APPEND;
```

## 테스트

```sh
make test
```

`make test`는 `./mysql_bptree --self-test`를 실행합니다. self-test에서는 INSERT, SELECT, FORCE/IGNORE INDEX, CREATE INDEX, EXPLAIN, BENCHMARK, 범위 검색이 정상 동작하는지 확인합니다.

## 종료

```sql
EXIT;
```

종료 시 현재 데이터가 `data/` 폴더에 저장됩니다.
