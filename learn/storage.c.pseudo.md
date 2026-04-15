# storage.c 수도코드

역할:

- 테이블 메모리 관리, 스키마 저장/로드, 바이너리 데이터 저장/로드, 인덱스 메타데이터 저장/로드를 담당합니다.
- 프로그램 시작 시 `data/` 파일을 읽고 B+Tree 인덱스를 다시 만듭니다.

테이블 관리:

```text
함수 table_init(table):
    table을 0으로 초기화
    next_auto_increment_id = 1
```

```text
함수 table_add_record(table, record):
    만약 rows 배열이 가득 찼다면:
        capacity를 2배로 늘림

    rows[count] = record
    count 증가

    record.id가 next_auto_increment_id 이상이면:
        next_auto_increment_id = record.id + 1

    true 반환
```

스키마 기본값:

```text
함수 schema_set_default(schema):
    컬럼 4개 생성:
        id    INT, primary key, auto_increment
        name  VARCHAR
        age   INT
        email VARCHAR

    primary_column = id
```

저장 흐름:

```text
함수 db_save_all(db):
    data/ 폴더가 없으면 생성

    save_schema(db)
    save_indexes(db)
    save_data(db)
    save_meta(db)

    모두 성공하면 true
```

로드 흐름:

```text
함수 db_load_startup(db, err, err_size, begin):
    data/ 폴더가 없으면 생성

    users.schema.csv 로드
        // 컬럼 순서가 바뀌어도 header 이름으로 읽음

    users.indexes.csv 로드
        // 없으면 PRIMARY 인덱스 메타데이터 생성

    users.meta 로드
        // 레코드 개수, 다음 auto_increment id 확인

    users.data 바이너리 로드
        // RecordDisk 크기 단위로 읽음

    meta의 record_size와 실제 구조체 크기 비교
    meta의 record_count와 실제 로드 개수 비교

    rebuild_indexes(db)
        // 바이너리 데이터에는 B+Tree 자체를 저장하지 않음
        // 시작할 때 레코드를 읽고 메모리 B+Tree를 다시 구성

    로드 결과 출력
```

인덱스 재생성:

```text
함수 rebuild_one_index(db, idx):
    기존 idx.tree 해제
    새 B+Tree 생성

    모든 레코드를 순회:
        record에서 idx.column 값을 key로 추출
        B+Tree에 key -> record 삽입

    성공하면 true
```

파일 역할:

```text
data/users.schema.csv:
    컬럼명, 타입, 크기, not null, auto increment, primary key

data/users.indexes.csv:
    인덱스 이름, 컬럼명, unique 여부, primary 여부, active 여부

data/users.data:
    RecordDisk 배열을 그대로 저장한 바이너리 파일

data/users.meta:
    record_size, record_count, next_auto_increment_id
```

이해 포인트:

- 실제 데이터는 바이너리라서 `cat`으로 읽을 수 없습니다.
- 스키마와 인덱스 메타데이터는 CSV라 사람이 확인하기 쉽습니다.
- B+Tree는 실행 중 메모리에만 있고, 재시작 시 바이너리 데이터로부터 재구성합니다.

