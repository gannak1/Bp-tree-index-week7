# util.c 수도코드

역할:

- 여러 모듈에서 반복해서 쓰는 공통 함수를 모읍니다.
- 시간 측정, 문자열 처리, 컬럼 이름 변환, 인덱스 키 비교, 에러 출력 등을 담당합니다.

수도코드:

```text
함수 now_sec():
    현재 단조 증가 시간을 초 단위 double로 반환
    // 실행 시간 출력에 사용
```

```text
함수 safe_copy(dst, dst_size, src):
    dst_size가 0이면 종료
    src를 dst_size - 1까지만 복사
    마지막 문자를 '\0'으로 고정
    // C 문자열 버퍼 오버플로우 방지
```

```text
함수 str_trim_inplace(s):
    앞 공백을 제거
    뒤 공백을 제거
    // SQL 토큰, CSV 필드 정리에 사용
```

```text
함수 find_ci(haystack, needle):
    haystack 안에서 needle을 대소문자 무시하고 검색
    발견하면 위치 포인터 반환
    없으면 NULL 반환
```

```text
함수 print_error_timed(kind, message, sql, start, length, begin):
    에러 종류와 메시지 출력
    문제가 난 SQL 위치에 ^ 표시
    실행 시간 출력

    예:
        Type Error: Column 'age' expects INT
        INSERT INTO ...
                    ^^^
        Execution time: ...
```

```text
함수 column_from_name(name):
    "id"    -> COL_ID
    "name"  -> COL_NAME
    "age"   -> COL_AGE
    "email" -> COL_EMAIL
    그 외   -> COL_UNKNOWN
```

```text
함수 key_compare(a, b):
    만약 key 타입이 INT라면:
        정수 크기 비교

    만약 key 타입이 STRING이라면:
        strcmp로 문자열 비교

    반환값:
        a < b 이면 음수
        a == b 이면 0
        a > b 이면 양수
```

이해 포인트:

- B+Tree는 키 비교를 많이 하므로 `key_compare()`가 핵심입니다.
- 타입이 int인지 string인지에 따라 비교 방식이 달라집니다.
- 에러 출력은 SQL 위치를 표시하므로 디버깅과 과제 시연에 유리합니다.

## 키 관련 함수 상세

### make_int_key(v)

```text
목적:
    정수 컬럼 값을 B+Tree에서 비교 가능한 IndexKey로 감싼다.

결과:
    key.type = KEY_INT
    key.value.int_value = v
```

### make_string_key_borrowed(s)

```text
목적:
    문자열 컬럼 값을 IndexKey 형태로 만든다.

주의:
    borrowed라는 이름처럼 문자열 메모리를 새로 소유하지 않는다.
    Record 안의 name/email 포인터를 잠시 참조하는 용도다.

언제 쓰는가:
    key_from_record()에서 record->name, record->email을 key로 만들 때 사용한다.
```

### key_clone(src, out)

```text
목적:
    B+Tree 노드 안에 key를 오래 저장해야 할 때 사용한다.

처리:
    INT key:
        값만 복사하면 된다.

    STRING key:
        문자열을 xstrdup으로 새로 할당한다.
        B+Tree 노드가 그 문자열을 소유한다.

왜 필요한가:
    borrowed string은 원본 Record 메모리에 의존한다.
    B+Tree 내부 key는 노드가 살아 있는 동안 안정적으로 유지되어야 하므로 clone이 필요하다.
```

### key_free(k)

```text
목적:
    key_clone으로 복사한 string key를 해제한다.

처리:
    key.type이 KEY_STRING이면:
        string_value free

    key 전체를 0으로 초기화
```

### key_compare(a, b)

```text
목적:
    B+Tree의 모든 정렬 기준을 제공한다.

처리:
    둘 다 INT:
        int_value 크기 비교

    둘 다 STRING:
        strcmp 결과 사용

반환:
    a < b : 음수
    a = b : 0
    a > b : 양수

사용 위치:
    leaf_lower_bound()
    internal_child_index()
    bplus_tree_search()
    condition_matches_key()
```

## 문자열 유틸이 중요한 이유

```text
SQL 입력은 사용자가 직접 치는 문자열이다.
따라서 다음 문제가 항상 생긴다.

    - 앞뒤 공백
    - 대소문자 차이
    - 세미콜론
    - 따옴표
    - 너무 긴 입력

util.c는 이런 문제를 처리하는 공통 기반이다.
이 기반이 있어야 executor/query/storage가 같은 방식으로 문자열을 다룰 수 있다.
```

## 에러 위치 표시 원리

```text
print_error_timed(kind, message, sql, start, length):
    1. kind와 message 출력
    2. SQL 원문 출력
    3. start 위치까지 공백 출력
    4. length만큼 ^ 출력
    5. 실행 시간 출력

예:
    SQL:
        SELECT * FROM users WHERE age = 'abc';

    start:
        'abc'가 시작하는 위치

    length:
        5

    출력:
        SELECT * FROM users WHERE age = 'abc';
                                        ^^^^^
```
