#include "engine_internal.h"

double now_sec(void) {
    struct timespec ts;
#if defined(CLOCK_MONOTONIC)
    clock_gettime(CLOCK_MONOTONIC, &ts);
#else
    timespec_get(&ts, TIME_UTC);
#endif
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1000000000.0;
}

char *xstrdup(const char *s) {
    size_t n = strlen(s) + 1;
    char *p = (char *)malloc(n);
    if (!p) {
        return NULL;
    }
    memcpy(p, s, n);
    return p;
}

void safe_copy(char *dst, size_t dst_size, const char *src) {
    if (dst_size == 0) {
        return;
    }
    strncpy(dst, src, dst_size - 1);
    dst[dst_size - 1] = '\0';
}

void str_trim_inplace(char *s) {
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
    printf("%s: %s\n", kind, message);
    print_caret(sql, start, length);
    printf("\nExecution time: %.6f sec\n", now_sec() - begin);
}

ColumnId column_from_name(const char *name) {
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
    return (col == COL_ID || col == COL_AGE) ? KEY_INT : KEY_STRING;
}

const char *type_name(ColumnType t) {
    return t == TYPE_INT ? "INT" : "VARCHAR";
}

ColumnType column_type(ColumnId col) {
    return (col == COL_ID || col == COL_AGE) ? TYPE_INT : TYPE_VARCHAR;
}

IndexKey make_int_key(int v) {
    IndexKey k;
    k.type = KEY_INT;
    k.value.int_value = v;
    return k;
}

IndexKey make_string_key_borrowed(const char *s) {
    IndexKey k;
    k.type = KEY_STRING;
    k.value.string_value = (char *)s;
    return k;
}

bool key_clone(IndexKey src, IndexKey *out) {
    out->type = src.type;
    if (src.type == KEY_INT) {
        out->value.int_value = src.value.int_value;
        return true;
    }
    out->value.string_value = xstrdup(src.value.string_value ? src.value.string_value : "");
    return out->value.string_value != NULL;
}

void key_free(IndexKey *k) {
    if (k && k->type == KEY_STRING) {
        free(k->value.string_value);
        k->value.string_value = NULL;
    }
}

int key_compare(IndexKey a, IndexKey b) {
    if (a.type == KEY_INT) {
        if (a.value.int_value < b.value.int_value) return -1;
        if (a.value.int_value > b.value.int_value) return 1;
        return 0;
    }
    return strcmp(a.value.string_value ? a.value.string_value : "",
                  b.value.string_value ? b.value.string_value : "");
}

bool key_to_string(IndexKey k, char *buf, size_t size) {
    if (k.type == KEY_INT) {
        snprintf(buf, size, "%d", k.value.int_value);
    } else {
        snprintf(buf, size, "'%s'", k.value.string_value ? k.value.string_value : "");
    }
    return true;
}
