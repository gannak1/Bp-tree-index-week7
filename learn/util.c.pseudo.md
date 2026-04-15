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

