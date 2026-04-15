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

