# SQL 처리기 슈도코드 분석

이 문서는 현재 프로젝트의 핵심 흐름을 "실제 C 코드와 1:1로 대응되는 수준"의 슈도코드로 정리한 것이다.  
발표, 코드 리뷰, 팀 내부 설명용으로 바로 읽을 수 있게 `main -> lexer -> parser -> executor -> storage -> B+ tree` 순서로 정리한다.

---

## 1. 전체 실행 흐름

```text
입력 SQL
-> lex_sql()
-> parse_statement()
-> AST 생성
-> execute_statement()
-> prepare_execution_context()
-> load_table_meta()
-> build_id_index_from_data() 또는 기존 context 재사용
-> INSERT / SELECT 분기
-> 결과 출력
```

---

## 2. `main.c`

### 2-1. `run_sql(sql_text, mode)`

```text
함수 run_sql(sql_text, mode):
    시작 시간 기록

    tokens = lex_sql(sql_text)
    if lexer 실패:
        오류 출력
        timing log 기록
        return 실패

    root = parse_statement(tokens)
    if parser 실패:
        오류 출력
        tokens 해제
        timing log 기록
        return 실패

    if execute_statement(root) 실패:
        오류 출력
        AST 해제
        tokens 해제
        timing log 기록
        return 실패

    AST 해제
    tokens 해제

    종료 시간 기록
    timing log 기록
    return 성공
```

### 2-2. `run_sql_file(path)`

```text
함수 run_sql_file(path):
    SQL 파일 열기
    파일 열기 실패면 오류 반환

    파일을 한 번 훑어서
        첫 번째 유효 SQL 한 줄 찾기
        preload_sql_context(그 줄) 호출
        break

    파일 포인터 rewind

    파일을 다시 한 줄씩 읽으며
        빈 줄이면 continue
        run_sql(trimmed_sql, "FILE") 실행
        실패하면 중단

    파일 닫기
    전체 성공 여부 반환
```

### 2-3. `run_repl(preload_target)`

```text
함수 run_repl(preload_target):
    preload_target이 없으면 기본값 users 사용

    preload_target을 [schema.]table 형태로 파싱
    파싱 성공 시 preload_table_context(schema, table)

    무한 반복:
        프롬프트 출력
        한 줄 입력
        EOF면 종료
        exit 또는 quit면 종료
        공백만 있으면 continue
        run_sql(input, "REPL")
```

---

## 3. `lexer.c`

### 3-1. `lex_sql(sql)`

```text
함수 lex_sql(sql):
    빈 토큰 배열 준비
    cursor = sql 시작

    while cursor가 문자열 끝이 아닐 때:
        공백이면:
            cursor++
            continue

        if 현재 문자가 *, , . ( ) ; 중 하나면:
            알맞은 TokenType 결정
            push_token()
            cursor++
            continue

        if 현재 문자가 ! > < = 중 하나면:
            !=, >=, <= 같은 2글자 연산자인지 먼저 확인
            아니면 1글자 연산자(=, >, <) 처리
            push_token()
            cursor 이동
            continue

        if 현재 문자가 ' 이면:
            다음 ' 가 나올 때까지 문자열 literal 탐색
            닫는 따옴표가 없으면 오류
            TOKEN_STRING push
            continue

        if 현재 문자가 숫자이거나 부호 뒤에 숫자면:
            숫자가 끝날 때까지 전진
            TOKEN_NUMBER push
            continue

        if 현재 문자가 영문자 또는 _ 면:
            identifier 끝까지 전진
            일단 TOKEN_IDENTIFIER로 push
            keyword_type()로 예약어인지 다시 판정
            마지막 토큰 type 수정
            continue

        그 외 문자면:
            unexpected character 오류
            토큰 배열 해제 후 반환

    마지막에 TOKEN_EOF 추가
    status.ok = 1
    반환
```

### 3-2. `keyword_type(text)`

```text
함수 keyword_type(text):
    INSERT면 TOKEN_KEYWORD_INSERT
    INTO면 TOKEN_KEYWORD_INTO
    VALUES면 TOKEN_KEYWORD_VALUES
    SELECT면 TOKEN_KEYWORD_SELECT
    FROM이면 TOKEN_KEYWORD_FROM
    WHERE면 TOKEN_KEYWORD_WHERE
    BETWEEN이면 TOKEN_KEYWORD_BETWEEN
    AND면 TOKEN_KEYWORD_AND
    아니면 TOKEN_IDENTIFIER
```

---

## 4. `parser.c`

### 4-1. `parse_statement(tokens, &root)`

```text
함수 parse_statement(tokens, &root):
    parser = { tokens, index = 0 }

    첫 토큰이 INSERT면:
        root = parse_insert()
    else if 첫 토큰이 SELECT면:
        root = parse_select()
    else:
        only INSERT and SELECT are supported 오류

    root가 NULL이면 실패
    status.ok = 1
    성공 반환
```

### 4-2. `parse_insert()`

```text
함수 parse_insert():
    expect INSERT
    expect INTO

    table_node = parse_table_node()

    expect VALUES
    expect '('

    value_list_node = parse_value_list_node()

    expect ')'
    optional ';'
    expect EOF

    root = NODE_INSERT 생성
    root 아래에 table_node, value_list_node 연결
    return root
```

### 4-3. `parse_select()`

```text
함수 parse_select():
    expect SELECT

    column_list_node = parse_column_list_node()

    expect FROM
    table_node = parse_table_node()

    if WHERE가 있으면:
        where_node = parse_where_node()

    optional ';'
    expect EOF

    root = NODE_SELECT 생성
    root 아래에 column_list_node, table_node 연결
    where_node가 있으면 추가 연결
    return root
```

### 4-4. `parse_table_node()`

```text
함수 parse_table_node():
    first_identifier 읽기

    TABLE 노드 생성

    if 다음 토큰이 '.' 이면:
        table_identifier 읽기
        schema = first_identifier
        table = table_identifier
    else:
        schema = DEFAULT_SCHEMA_NAME
        table = first_identifier

    TABLE 노드 아래에
        IDENTIFIER(schema)
        IDENTIFIER(table)
    연결

    return TABLE 노드
```

### 4-5. `parse_where_node()`

```text
함수 parse_where_node():
    column_name 읽기

    WHERE 노드 생성
    COLUMN 노드 생성 후 WHERE 아래 연결

    if 다음 토큰이 BETWEEN이면:
        between_node = parse_between_node()
        WHERE 아래 between_node 연결
        return WHERE

    operator_text = parse_operator_text()
    value_node = parse_value_node()

    OPERATOR 노드 생성
    WHERE 아래 OPERATOR, VALUE 연결
    return WHERE
```

### 4-6. `parse_between_node()`

```text
함수 parse_between_node():
    expect BETWEEN
    lower_node = parse_value_node()
    expect AND
    upper_node = parse_value_node()

    BETWEEN 노드 생성
    lower_node, upper_node 연결
    반환
```

---

## 5. `meta.c`

### 5-1. `load_table_meta(schema_name, table_name, &meta)`

```text
함수 load_table_meta(schema_name, table_name, meta):
    meta 초기화

    meta.schema_name = schema_name
    meta.table_name = table_name
    meta.meta_file_path = meta/<schema>/<table>.schema.csv
    meta.data_file_path = data/<schema>/<table>.dat

    메타 파일 열기
    실패하면 table not found 오류

    첫 줄(header) 읽기
    실패하면 empty meta file 오류

    나머지 줄 반복:
        빈 줄이면 continue
        parse_csv_line(line, parts, 3)
        parts 개수가 3이 아니면 오류

        name = parts[0]
        type = parts[1]
        size = parts[2]

        column.name = name
        column.type = parse_column_type(type)
        column.size = atoi(size)
        column.offset = 현재 offset

        offset += column.size
        column_count++

    column_count == 0 이면 오류
    offset > MAX_ROW_SIZE면 오류

    meta.column_count = column_count
    meta.row_size = offset
    성공 반환
```

### 5-2. `ensure_parent_directory(file_path)`

```text
함수 ensure_parent_directory(file_path):
    file_path에서 마지막 경로 구분자 앞까지 잘라 parent path 생성

    parent path를 segment 단위로 순회:
        current 경로를 하나씩 누적
        mkdir(current)
        이미 있으면 무시
        진짜 실패면 오류 반환

    성공 반환
```

---

## 6. `executor.c`

### 6-1. 전역 컨텍스트

```text
g_context:
    meta
    id_index (B+ tree)
    record_count
    is_loaded
    last_execution_path
```

### 6-2. `ensure_execution_context(meta)`

```text
함수 ensure_execution_context(meta):
    if 현재 g_context가 같은 테이블이면:
        그냥 재사용

    기존 g_context reset
    g_context.meta = meta 복사
    id_index 초기화

    build_id_index_from_data(g_context) 호출
    실패하면 context reset 후 실패

    g_context.is_loaded = 1
    성공 반환
```

### 6-3. `execute_statement(root)`

```text
함수 execute_statement(root):
    prepare_execution_context(root)
    실패하면 종료

    last_execution_path = UNKNOWN

    if root.type == NODE_INSERT:
        last_execution_path = INSERT
        append_binary_row()
        성공 시 "1 row inserted"와 "Inserted id" 출력
        return 성공

    if root.type == NODE_SELECT:
        return execute_select()

    unsupported statement 오류
```

---

## 7. `storage.c`

### 7-1. `build_id_index_from_data(context)`

```text
함수 build_id_index_from_data(context):
    id 컬럼 규칙 검사

    기존 B+ 트리 비우기
    새 B+ 트리 초기화
    record_count = 0

    data 파일 열기
    파일이 없으면 빈 테이블로 간주하고 성공

    while true:
        offset = 현재 파일 위치(ftell)
        row_size 만큼 fread
        EOF면 break
        fread 실패면 오류

        row의 첫 id 컬럼 읽기
        bptree_insert(id, offset)
        record_count++

    파일 닫기
    성공 반환
```

### 7-2. `append_binary_row(context, root, &inserted_id)`

```text
함수 append_binary_row(context, root, inserted_id):
    VALUE_LIST 노드 찾기
    id 컬럼 규칙 검사

    row 버퍼 전체를 0으로 초기화
    next_id = record_count + 1
    row 첫 컬럼 위치에 next_id 기록

    VALUE_LIST를 순회하면서
        실제 데이터 컬럼 1번부터 끝까지
        write_value()로 row 버퍼에 기록

    값이 부족하거나 너무 많으면 오류

    parent directory 보장

    row_offset = record_count * row_size
    data 파일 append 모드로 열기
    fwrite(row)
    파일 닫기

    bptree_insert(next_id, row_offset)
    record_count++
    inserted_id = next_id
    성공 반환
```

### 7-3. `row_matches_where(meta, row, where_node)`

```text
함수 row_matches_where(meta, row, where_node):
    where_node가 없으면 항상 true

    if WHERE가 BETWEEN 구조면:
        컬럼 index 찾기
        컬럼이 INT 아니면 오류
        lower / upper 숫자 파싱
        실제 row 값 읽기
        return lower <= actual <= upper

    아니면 일반 비교식:
        column / operator / value 추출
        컬럼 index 찾기

        if INT 컬럼이면:
            actual int 읽기
            expected int 파싱
            compare_ints(actual, expected, op)

        if CHAR 컬럼이면:
            actual string 복원
            expected string 복원
            compare_strings(actual, expected, op)
```

### 7-4. `execute_select(context, root)`

```text
함수 execute_select(context, root):
    COLUMN_LIST 노드 찾기
    WHERE 노드 찾기

    if SELECT * 이면:
        모든 컬럼 인덱스를 selected_indexes에 채움
    else:
        COLUMN_LIST를 순회하며 컬럼 인덱스 찾기

    헤더 출력

    if WHERE가 정확히 id = number 이면:
        last_execution_path = INDEXED
        return select_by_id_index()

    last_execution_path = FULL_SCAN

    data 파일 열기
    while row_size 만큼 fread:
        matches = row_matches_where(...)
        오류면 중단
        조건 만족하면 row 출력
        rows_selected++

    마지막에 N rows selected 출력
```

### 7-5. `select_by_id_index(context, target_id, ...)`

```text
함수 select_by_id_index(context, target_id, selected_columns):
    bptree_search(target_id)로 row offset 찾기
    못 찾으면 0 rows selected 출력 후 성공

    data 파일 열기
    fseek(offset)
    row 하나 fread
    해당 row만 출력
    1 rows selected 출력
```

---

## 8. `bptree.c`

### 8-1. `bptree_insert(tree, key, offset)`

```text
함수 bptree_insert(tree, key, offset):
    root가 없으면:
        새 leaf root 생성
        key/offset 저장
        종료

    if root가 leaf면:
        result = insert_into_leaf(root, key, offset)
    else:
        result = insert_into_internal(root, key, offset)

    오류 메시지가 있으면 실패

    if root split 발생하면:
        새 internal root 생성
        promoted_key 저장
        기존 root와 right_node를 자식으로 연결
        tree.root = 새 root

    성공 반환
```

### 8-2. `insert_into_leaf(node, key, offset)`

```text
함수 insert_into_leaf(node, key, offset):
    정렬 위치 찾기
    중복 key면 오류

    뒤쪽 key/offset 한 칸씩 밀기
    새 key/offset 삽입
    key_count++

    if overflow 없으면:
        split 없음 반환

    split_index = key_count / 2
    새 오른쪽 leaf 생성

    split_index 뒤쪽 key/offset을 오른쪽 leaf로 복사
    왼쪽 leaf key_count 줄이기
    leaf next 연결 갱신

    promoted_key = 오른쪽 첫 key
    split 결과 반환
```

### 8-3. `insert_into_internal(node, key, offset)`

```text
함수 insert_into_internal(node, key, offset):
    key가 들어갈 child 위치 찾기

    child에 재귀 삽입
    child split 없으면 그대로 반환

    child split 있으면:
        promoted_key를 현재 internal에 삽입
        right child 포인터 연결
        key_count++

    if overflow 없으면:
        split 없음 반환

    mid_index = key_count / 2
    새 오른쪽 internal 생성

    mid_index 오른쪽 key들을 새 internal로 이동
    mid_index 오른쪽 child 포인터도 이동

    promoted_key = 기존 mid_index key
    왼쪽 key_count 줄이기
    split 결과 반환
```

### 8-4. `bptree_search(tree, key)`

```text
함수 bptree_search(tree, key):
    root가 없으면 실패

    node = root
    while node가 internal이면:
        child_index = find_child_index(node, key)
        node = node.children[child_index]

    leaf에 도착하면:
        keys 배열 순회
        같은 key 있으면 offset 반환
        없으면 실패
```

---

## 9. 시간 측정 관련 흐름

현재는 SQL 한 문장마다 `run_sql()`에서 시간을 잰다.

```text
started_at = now_ms()
lex -> parse -> execute
ended_at = now_ms()
append_query_timing_log(mode, sql, ended_at - started_at, execution_path)
```

즉 현재 시간 측정은:

- 인덱스 preload
- parser
- executor
- 실제 row 읽기/쓰기

까지 포함한 end-to-end 시간이다.

---

## 10. 핵심 요약

- `INSERT`는 사용자가 `id`를 넣지 않고, 프로그램이 자동으로 `next_id`를 만든다.
- `SELECT ... WHERE id = ?`만 B+ 트리를 사용한다.
- 그 외 `age = ?`, `name = ?`, `BETWEEN`은 선형 탐색이다.
- 프로그램은 같은 테이블을 처음 쓸 때 `.dat` 전체를 읽어 `id -> row_offset` B+ 트리를 메모리에 재구축한다.
- 실제 row 데이터는 계속 `.dat` 파일에 있고, 메모리에는 메타와 인덱스만 유지된다.
