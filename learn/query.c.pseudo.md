# query.c 수도코드

역할:

- SQL의 `WHERE` 조건을 해석합니다.
- SELECT 결과를 담는 버퍼를 관리합니다.
- 결과를 MySQL처럼 표 형태로 출력합니다.
- B+Tree range scan 결과 수집을 담당합니다.

값 파싱:

```text
함수 parse_int_value(s):
    문자열 앞뒤 공백 제거

    만약 따옴표로 감싸져 있다면:
        따옴표 제거

    strtol로 정수 변환

    변환 실패, 남는 문자가 있음, int 범위 초과라면:
        false 반환

    out에 정수 저장
    true 반환
```

```text
함수 unquote_value(src, dst):
    앞뒤 공백 제거

    만약 '...' 또는 "..." 형태라면:
        바깥 따옴표 제거
        escape된 따옴표 처리

    아니면:
        그대로 복사
```

WHERE 조건 파싱:

```text
함수 parse_condition(sql, where, cond, err...):
    cond 초기화

    만약 "BETWEEN"이 있다면:
        column BETWEEN lower AND upper 형태로 파싱
        column 이름 확인
        lower 값 타입 체크
        upper 값 타입 체크
        lower <= upper 확인

        cond.column = column
        cond.op = BETWEEN
        cond.lower = lower
        cond.upper = upper
        cond.lower_inclusive = true
        cond.upper_inclusive = true
        true 반환

    아니면:
        따옴표 밖에서 연산자 찾기
            =, <, <=, >, >=

        왼쪽은 컬럼명으로 파싱
        오른쪽은 컬럼 타입에 맞는 값으로 파싱

        연산자에 따라 lower/upper 경계 설정:
            id = 10:
                lower = 10 inclusive
                upper = 10 inclusive

            id > 10:
                lower = 10 exclusive

            id >= 10:
                lower = 10 inclusive

            id < 10:
                upper = 10 exclusive

            id <= 10:
                upper = 10 inclusive

        true 반환
```

B+Tree range scan:

```text
함수 bplus_tree_collect_range(tree, cond, result):
    만약 cond에 lower가 있다면:
        lower key가 들어갈 leaf부터 시작
    아니면:
        가장 왼쪽 leaf부터 시작

    leaf를 따라 반복:
        leaf 안의 key를 순서대로 확인

        upper를 넘어가면:
            검색 종료

        key가 cond 범위에 맞으면:
            해당 key의 record list를 result에 추가

        다음 leaf로 이동

    true 반환
```

결과 출력:

```text
함수 print_record_table(rows, count, elapsed, access, index_name):
    만약 count == 0:
        Empty set 출력
        access type, index name 출력
        종료

    각 컬럼의 출력 폭 계산
        // 너무 긴 문자열은 최대 폭까지만 표시하고 ~ 붙임

    구분선 출력
    header 출력
    각 record 출력
    row count와 실행 시간 출력
    access type과 index name 출력
```

이해 포인트:

- 타입 체크는 WHERE 값 파싱 단계에서 수행됩니다.
- 범위 검색은 leaf linked list를 사용하므로 `O(log N + K)`에 가깝습니다.
- `IGNORE INDEX`를 쓰면 같은 조건도 선형 탐색으로 비교할 수 있습니다.

## WHERE 조건 파싱 예시

### id = 10

```text
입력:
    WHERE id = 10

parse_condition:
    1. BETWEEN이 있는지 먼저 확인한다.
       없음.

    2. 따옴표 밖에서 연산자를 찾는다.
       '=' 발견

    3. 왼쪽 문자열 "id"를 컬럼명으로 파싱한다.
       column = COL_ID

    4. 오른쪽 문자열 "10"을 COL_ID 타입에 맞게 파싱한다.
       id는 INT이므로 parse_int_value("10")

    5. QueryCondition 생성:
       op = OP_EQ
       lower = 10 inclusive
       upper = 10 inclusive
```

### age >= 20

```text
입력:
    WHERE age >= 20

결과:
    column = COL_AGE
    op = OP_GE
    lower = 20
    lower_inclusive = true
    upper 없음

의미:
    key >= 20인 모든 row를 찾는다.
```

### name = 'kim'

```text
입력:
    WHERE name = 'kim'

처리:
    name 컬럼은 VARCHAR 타입이다.
    오른쪽 값에서 따옴표를 제거한다.
    string key "kim"을 생성한다.

주의:
    문자열 key는 동적 할당될 수 있으므로 query_condition_free()에서 해제해야 한다.
```

### id BETWEEN 100 AND 200

```text
입력:
    WHERE id BETWEEN 100 AND 200

처리:
    1. " BETWEEN " 위치 찾기
    2. 왼쪽 "id"를 컬럼명으로 파싱
    3. BETWEEN 뒤에서 " AND " 위치 찾기
    4. lower_text = "100"
    5. upper_text = "200"
    6. 둘 다 INT로 타입 체크
    7. lower <= upper인지 확인

결과:
    lower = 100 inclusive
    upper = 200 inclusive
```

## 따옴표 밖 연산자 찾기가 필요한 이유

```text
예:
    WHERE name = 'a >= b'

문자열 안의 >=는 연산자가 아니다.
진짜 연산자는 name 뒤의 = 이다.

그래서 find_operator_outside_quotes()는:
    문자열을 왼쪽부터 훑으면서
    현재 따옴표 안인지 밖인지 추적한다.
    따옴표 밖에서 발견한 =, <, <=, >, >=만 연산자로 인정한다.
```

## 선형 탐색과 인덱스 탐색에서 같은 조건을 쓰는 방식

```text
선형 탐색:
    모든 Record를 순회한다.
    record_matches_condition(record, cond)를 호출한다.
    조건에 맞으면 result에 추가한다.

인덱스 탐색:
    QueryCondition의 column과 맞는 index를 찾는다.
    = 조건이면 bplus_tree_search()
    범위 조건이면 bplus_tree_collect_range()

공통점:
    둘 다 QueryCondition을 기준으로 판단한다.
    그래서 WHERE 파싱 로직이 한 곳에 모인다.
```

## bplus_tree_collect_range() 상세 흐름

```text
입력:
    tree
        // 사용할 B+Tree 인덱스
    cond
        // lower/upper 조건이 들어 있는 QueryCondition
    result
        // 결과 Record*를 담을 동적 배열

상세:
    1. 시작 leaf 결정
        lower가 있으면:
            lower key가 들어갈 leaf를 찾는다.
        lower가 없으면:
            가장 왼쪽 leaf에서 시작한다.

    2. leaf 내부 시작 위치 결정
        lower가 있으면:
            leaf_lower_bound(leaf, lower)
        없으면:
            0

    3. leaf key 순회
        key가 upper를 넘어가면 즉시 종료한다.
        B+Tree leaf는 정렬되어 있으므로 뒤는 볼 필요가 없다.

    4. key가 조건에 맞으면:
        그 key에 연결된 RecordRefList의 모든 record를 result에 추가한다.

    5. leaf = leaf->next
        다음 leaf로 이동한다.

    6. 더 이상 leaf가 없으면 종료한다.
```

## 출력 폭 계산

```text
print_record_table():
    결과 row를 출력하기 전에 먼저 모든 row를 한 번 본다.

    각 컬럼별 최대 표시 폭을 계산:
        id
        name
        age
        email

    name/email이 너무 길면:
        최대 폭까지만 출력하고 끝에 ~를 붙인다.

이유:
    긴 값 때문에 표가 옆으로 무한히 밀리는 문제를 막기 위해서다.
```
