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

## 파일 저장을 더 자세히 보기

### save_schema(db)

```text
목적:
    현재 schema 정보를 data/users.schema.csv에 저장한다.

저장 방식:
    1. data/users.schema.csv.tmp 파일을 먼저 연다.
    2. header를 쓴다.
       column_name,type,size,not_null,auto_increment,primary_key
    3. 각 컬럼 정보를 한 줄씩 쓴다.
    4. 파일을 닫는다.
    5. tmp 파일을 실제 users.schema.csv로 rename한다.

왜 tmp 파일을 쓰는가:
    저장 도중 프로그램이 중단되면 원본 파일이 깨질 수 있다.
    tmp에 먼저 완성본을 쓴 다음 rename하면 더 안전하다.
```

### load_schema(db, path, err)

```text
목적:
    CSV 스키마 파일을 읽어서 db->schema를 채운다.

상세 흐름:
    1. 파일이 없으면 기본 schema를 사용한다.
       persist 모드이면 기본 schema를 파일로 저장한다.

    2. 첫 줄 header를 읽는다.

    3. header 이름을 기준으로 column 위치를 찾는다.
       예:
           column_name이 몇 번째 필드인지
           type이 몇 번째 필드인지
           primary_key가 몇 번째 필드인지

    4. 각 row를 읽으면서 ColumnMeta를 구성한다.

    5. primary_key=true인 컬럼을 schema.primary_column으로 설정한다.

장점:
    CSV 컬럼 순서가 바뀌어도 header 이름으로 찾기 때문에 정상 동작한다.
```

### save_data(db)

```text
목적:
    실제 users 레코드를 data/users.data 바이너리 파일에 저장한다.

상세 흐름:
    1. data/users.data.tmp를 wb 모드로 연다.
    2. table.rows를 처음부터 끝까지 순회한다.
    3. 각 Record를 RecordDisk 크기만큼 fwrite한다.
    4. 파일을 닫는다.
    5. tmp 파일을 users.data로 rename한다.

특징:
    텍스트 CSV가 아니라 고정 크기 구조체를 그대로 저장한다.
    그래서 100만 건 저장/로드가 단순하고 빠르다.
```

### load_data(db, path, replace, err)

```text
목적:
    바이너리 파일에서 Record를 읽어 메모리 table에 적재한다.

상세 흐름:
    1. 파일을 rb 모드로 연다.
       파일이 없으면 빈 테이블로 보고 성공 처리한다.

    2. 파일 크기를 구한다.

    3. 파일 크기가 sizeof(RecordDisk)로 나누어떨어지는지 검사한다.
       나누어떨어지지 않으면 손상된 데이터 파일로 판단한다.

    4. replace=true이면 기존 table을 비운다.

    5. row 수 = 파일 크기 / sizeof(RecordDisk)

    6. row 수만큼 반복:
        Record 메모리 할당
        fread로 RecordDisk 하나 읽기
        name/email 마지막에 '\0' 보장
        table_add_record()로 table에 추가

    7. 성공하면 true 반환
```

### save_indexes(db), load_indexes(db)

```text
save_indexes:
    각 인덱스의 메타데이터만 CSV로 저장한다.
    B+Tree 노드 자체는 저장하지 않는다.

저장 필드:
    index_name
    column_name
    unique
    primary
    active

load_indexes:
    indexes.csv를 읽고 IndexMetaRuntime 배열을 복원한다.
    파일이 없으면 PRIMARY 인덱스를 기본으로 만든다.
    실제 B+Tree tree 포인터는 아직 비어 있다.
```

### rebuild_indexes(db)

```text
목적:
    인덱스 메타데이터와 table row를 이용해 메모리 B+Tree를 다시 만든다.

상세 흐름:
    모든 active index에 대해:
        기존 tree 해제
        새 B+Tree 생성

        모든 row에 대해:
            row에서 index column 값을 key로 추출
            B+Tree에 key -> row pointer 삽입

중요:
    data/users.data에는 row만 저장되어 있다.
    인덱스는 프로그램 시작 때 항상 다시 만들어진다.
```

## 시작 로드 전체 순서

```text
db_load_startup():
    ensure_data_dir()

    load_schema()
        // users.schema.csv

    load_indexes()
        // users.indexes.csv

    load_meta()
        // users.meta

    load_data()
        // users.data

    meta 검증:
        record_size가 현재 RecordDisk 크기와 같은가?
        record_count가 실제 로드한 row 수와 같은가?

    rebuild_indexes()
        // 모든 B+Tree 재구성

    로드 결과 출력
```

## 왜 schema는 CSV, data는 binary인가

```text
schema:
    사람이 직접 확인하거나 수정할 수 있으면 좋다.
    크기가 작다.
    그래서 CSV가 적합하다.

data:
    100만 건 이상 저장해야 한다.
    빠르게 읽고 써야 한다.
    고정 크기 RecordDisk 구조체라 바이너리 저장이 쉽다.
    그래서 binary가 적합하다.
```
