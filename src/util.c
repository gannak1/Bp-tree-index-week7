#include "engine_internal.h"

/*
 * util.c
 *
 * 여러 모듈에서 공통으로 사용하는 작은 유틸리티 함수 모음입니다.
 * 문자열 처리, 대소문자 무시 비교, 에러 위치 출력, 컬럼/키 변환을 담당합니다.
 */

double now_sec(void) {
    /* 실행 시간 측정을 위해 초 단위 monotonic time을 반환합니다. */
    struct timespec ts;
#if defined(CLOCK_MONOTONIC)
    clock_gettime(CLOCK_MONOTONIC, &ts);
#else
    timespec_get(&ts, TIME_UTC);
#endif
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1000000000.0;
}

char *xstrdup(const char *s) {
    /* strdup이 표준 C 함수가 아니므로 직접 안전하게 복제합니다. */
    size_t n = strlen(s) + 1;
    char *p = (char *)malloc(n);
    if (!p) {
        return NULL;
    }
    memcpy(p, s, n);
    return p;
}

void safe_copy(char *dst, size_t dst_size, const char *src) {
    /* 항상 null-terminated 문자열이 되도록 최대 dst_size - 1까지만 복사합니다. */
    if (dst_size == 0) {
        return;
    }
    strncpy(dst, src, dst_size - 1);
    dst[dst_size - 1] = '\0';
}

void str_trim_inplace(char *s) {
    /* SQL 토큰이나 CSV 필드의 앞뒤 공백을 제자리에서 제거합니다. */
    char *start = s;
    while (isspace((unsigned char)*start)) {
        start++;
    }
    if (start != s) {
        memmove(s, start, strlen(start) + 1);
    }
    size_t len = strlen(s);
    while (len > 0 && isspace((unsigned char)s[len - 1])) {
        s[--len] = '\0';
    }
}

int ci_char(int c) {
    return tolower((unsigned char)c);
}

int str_casecmp_local(const char *a, const char *b) {
    while (*a && *b) {
        int ca = ci_char(*a);
        int cb = ci_char(*b);
        if (ca != cb) {
            return ca - cb;
        }
        a++;
        b++;
    }
    return ci_char(*a) - ci_char(*b);
}

bool starts_with_ci(const char *s, const char *prefix) {
    while (*prefix) {
        if (ci_char(*s) != ci_char(*prefix)) {
            return false;
        }
        s++;
        prefix++;
    }
    return true;
}

const char *find_ci(const char *haystack, const char *needle) {
    /* 대소문자를 무시하고 부분 문자열을 찾습니다. SQL 키워드 검색에 사용됩니다. */
    size_t n = strlen(needle);
    if (n == 0) {
        return haystack;
    }
    for (const char *p = haystack; *p; p++) {
        size_t i = 0;
        while (i < n && p[i] && ci_char(p[i]) == ci_char(needle[i])) {
            i++;
        }
        if (i == n) {
            return p;
        }
    }
    return NULL;
}

static void print_caret(const char *sql, int start, int length) {
    /* 에러가 발생한 SQL 위치를 ^ 문자로 표시합니다. */
    if (start < 0) {
        return;
    }
    if (length <= 0) {
        length = 1;
    }
    printf("\n%s\n", sql);
    for (int i = 0; i < start; i++) {
        putchar(sql[i] == '\t' ? '\t' : ' ');
    }
    for (int i = 0; i < length; i++) {
        putchar('^');
    }
    putchar('\n');
}

void print_error_timed(const char *kind, const char *message, const char *sql,
                              int start, int length, double begin) {
    /* 에러 메시지, SQL 위치, 실행 시간을 함께 출력해 컴파일러 오류처럼 읽히게 합니다. */
    printf("%s: %s\n", kind, message);
    print_caret(sql, start, length);
    printf("\nExecution time: %.6f sec\n", now_sec() - begin);
}

ColumnId column_from_name(const char *name) {
    /* 사용자 입력 컬럼명을 내부 enum 값으로 변환합니다. */
    if (str_casecmp_local(name, "id") == 0) {
        return COL_ID;
    }
    if (str_casecmp_local(name, "name") == 0) {
        return COL_NAME;
    }
    if (str_casecmp_local(name, "age") == 0) {
        return COL_AGE;
    }
    if (str_casecmp_local(name, "email") == 0) {
        return COL_EMAIL;
    }
    return COL_UNKNOWN;
}

const char *column_name(ColumnId col) {
    switch (col) {
        case COL_ID: return "id";
        case COL_NAME: return "name";
        case COL_AGE: return "age";
        case COL_EMAIL: return "email";
        default: return "unknown";
    }
}

KeyType column_key_type(ColumnId col) {
    /* id/age는 정수 key, name/email은 문자열 key로 B+Tree에 저장합니다. */
    return (col == COL_ID || col == COL_AGE) ? KEY_INT : KEY_STRING;
}

const char *type_name(ColumnType t) {
    return t == TYPE_INT ? "INT" : "VARCHAR";
}

ColumnType column_type(ColumnId col) {
    return (col == COL_ID || col == COL_AGE) ? TYPE_INT : TYPE_VARCHAR;
}

IndexKey make_int_key(int v) {
    /* 정수 값을 B+Tree 비교용 IndexKey로 감쌉니다. */
    IndexKey k;
    k.type = KEY_INT;
    k.value.int_value = v;
    return k;
}

IndexKey make_string_key_borrowed(const char *s) {
    /*
     * 문자열을 복사하지 않고 빌려 쓰는 key입니다.
     * B+Tree 노드에 저장할 때는 key_clone()으로 별도 복사해야 합니다.
     */
    IndexKey k;
    k.type = KEY_STRING;
    k.value.string_value = (char *)s;
    return k;
}

bool key_clone(IndexKey src, IndexKey *out) {
    /* B+Tree 노드가 key를 소유할 수 있도록 INT는 값 복사, STRING은 문자열 복사를 수행합니다. */
    out->type = src.type;
    if (src.type == KEY_INT) {
        out->value.int_value = src.value.int_value;
        return true;
    }
    out->value.string_value = xstrdup(src.value.string_value ? src.value.string_value : "");
    return out->value.string_value != NULL;
}

void key_free(IndexKey *k) {
    /* key_clone()으로 소유하게 된 STRING key 메모리를 해제합니다. */
    if (k && k->type == KEY_STRING) {
        free(k->value.string_value);
        k->value.string_value = NULL;
    }
}

int key_compare(IndexKey a, IndexKey b) {
    /* B+Tree 정렬 기준. 모든 삽입/검색/범위 비교가 이 함수를 통과합니다. */
    if (a.type == KEY_INT) {
        if (a.value.int_value < b.value.int_value) return -1;
        if (a.value.int_value > b.value.int_value) return 1;
        return 0;
    }
    return strcmp(a.value.string_value ? a.value.string_value : "",
                  b.value.string_value ? b.value.string_value : "");
}

bool key_to_string(IndexKey k, char *buf, size_t size) {
    /* 에러 메시지나 디버깅 출력용으로 key를 사람이 읽을 수 있는 문자열로 바꿉니다. */
    if (k.type == KEY_INT) {
        snprintf(buf, size, "%d", k.value.int_value);
    } else {
        snprintf(buf, size, "'%s'", k.value.string_value ? k.value.string_value : "");
    }
    return true;
}
