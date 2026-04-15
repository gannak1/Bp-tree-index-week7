#include "engine_internal.h"

bool parse_int_value(const char *s, int *out) {
    char buf[256];
    safe_copy(buf, sizeof(buf), s);
    str_trim_inplace(buf);
    if ((buf[0] == '\'' || buf[0] == '"') && strlen(buf) >= 2) {
        char q = buf[0];
        size_t len = strlen(buf);
        if (buf[len - 1] == q) {
            memmove(buf, buf + 1, len - 2);
            buf[len - 2] = '\0';
        }
    }
    char *end = NULL;
    errno = 0;
    long v = strtol(buf, &end, 10);
    if (errno != 0 || end == buf) {
        return false;
    }
    while (*end) {
        if (!isspace((unsigned char)*end)) {
            return false;
        }
        end++;
    }
    if (v < INT_MIN || v > INT_MAX) {
        return false;
    }
    *out = (int)v;
    return true;
}

bool unquote_value(const char *src, char *dst, size_t dst_size) {
    char buf[512];
    safe_copy(buf, sizeof(buf), src);
    str_trim_inplace(buf);
    size_t len = strlen(buf);
    if (len >= 2 && ((buf[0] == '\'' && buf[len - 1] == '\'') ||
                     (buf[0] == '"' && buf[len - 1] == '"'))) {
        char quote = buf[0];
        size_t out = 0;
        for (size_t i = 1; i + 1 < len && out + 1 < dst_size; i++) {
            if (buf[i] == '\\' && i + 2 < len && buf[i + 1] == quote) {
                dst[out++] = quote;
                i++;
            } else {
                dst[out++] = buf[i];
            }
        }
        dst[out] = '\0';
        return true;
    }
    safe_copy(dst, dst_size, buf);
    return true;
}

int split_list_items(const char *sql, int offset, int length, ListItem *items, int max_items) {
    int count = 0;
    int start = offset;
    bool in_quote = false;
    char quote = '\0';
    for (int i = offset; i <= offset + length; i++) {
        char c = sql[i];
        bool at_end = i == offset + length;
        if (!at_end && (c == '\'' || c == '"')) {
            if (!in_quote) {
                in_quote = true;
                quote = c;
            } else if (quote == c && sql[i - 1] != '\\') {
                in_quote = false;
            }
        }
        if (at_end || (!in_quote && c == ',')) {
            if (count >= max_items) {
                return -1;
            }
            int end = i;
            while (start < end && isspace((unsigned char)sql[start])) start++;
            while (end > start && isspace((unsigned char)sql[end - 1])) end--;
            int len = end - start;
            if (len >= (int)sizeof(items[count].text)) {
                len = (int)sizeof(items[count].text) - 1;
            }
            memcpy(items[count].text, sql + start, (size_t)len);
            items[count].text[len] = '\0';
            items[count].start = start;
            items[count].length = end - start;
            count++;
            start = i + 1;
        }
    }
    return count;
}

const char *find_matching_paren(const char *open) {
    int depth = 0;
    bool in_quote = false;
    char quote = '\0';
    for (const char *p = open; *p; p++) {
        if (*p == '\'' || *p == '"') {
            if (!in_quote) {
                in_quote = true;
                quote = *p;
            } else if (quote == *p && p[-1] != '\\') {
                in_quote = false;
            }
        }
        if (in_quote) {
            continue;
        }
        if (*p == '(') {
            depth++;
        } else if (*p == ')') {
            depth--;
            if (depth == 0) {
                return p;
            }
        }
    }
    return NULL;
}

static int digits_int(int value) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%d", value);
    return (int)strlen(buf);
}

static int bounded_strlen(const char *s, int max_width) {
    int len = (int)strlen(s);
    return len > max_width ? max_width : len;
}

static void print_cell_string(const char *value, int width) {
    int len = (int)strlen(value);
    if (len <= width) {
        printf("%-*s", width, value);
        return;
    }
    if (width <= 1) {
        printf("%.*s", width, value);
        return;
    }
    printf("%.*s~", width - 1, value);
}

static void print_separator(int id_w, int name_w, int age_w, int email_w) {
    printf("+");
    for (int i = 0; i < id_w + 2; i++) putchar('-');
    printf("+");
    for (int i = 0; i < name_w + 2; i++) putchar('-');
    printf("+");
    for (int i = 0; i < age_w + 2; i++) putchar('-');
    printf("+");
    for (int i = 0; i < email_w + 2; i++) putchar('-');
    printf("+\n");
}

void print_record_table(Record **rows, int count, double elapsed, const char *access, const char *index_name) {
    if (count == 0) {
        printf("Empty set (%.6f sec)\n", elapsed);
        if (access) {
            printf("Access Type: %s\n", access);
            printf("Index Used : %s\n", index_name ? index_name : "none");
        }
        return;
    }
    const int max_name_w = 32;
    const int max_email_w = 48;
    int id_w = 2;
    int name_w = 4;
    int age_w = 3;
    int email_w = 5;
    for (int i = 0; i < count; i++) {
        Record *r = rows[i];
        int id_len = digits_int(r->id);
        int age_len = digits_int(r->age);
        int name_len = bounded_strlen(r->name, max_name_w);
        int email_len = bounded_strlen(r->email, max_email_w);
        if (id_len > id_w) id_w = id_len;
        if (age_len > age_w) age_w = age_len;
        if (name_len > name_w) name_w = name_len;
        if (email_len > email_w) email_w = email_len;
    }

    print_separator(id_w, name_w, age_w, email_w);
    printf("| %-*s | %-*s | %-*s | %-*s |\n", id_w, "id", name_w, "name", age_w, "age", email_w, "email");
    print_separator(id_w, name_w, age_w, email_w);
    for (int i = 0; i < count; i++) {
        Record *r = rows[i];
        char id_buf[32];
        char age_buf[32];
        snprintf(id_buf, sizeof(id_buf), "%d", r->id);
        snprintf(age_buf, sizeof(age_buf), "%d", r->age);
        printf("| %*s | ", id_w, id_buf);
        print_cell_string(r->name, name_w);
        printf(" | %*s | ", age_w, age_buf);
        print_cell_string(r->email, email_w);
        printf(" |\n");
    }
    print_separator(id_w, name_w, age_w, email_w);
    printf("%d row%s in set (%.6f sec)\n", count, count == 1 ? "" : "s", elapsed);
    if (access) {
        printf("Access Type: %s\n", access);
        printf("Index Used : %s\n", index_name ? index_name : "none");
    }
}

void query_condition_free(QueryCondition *cond) {
    if (!cond) {
        return;
    }
    if (cond->has_lower) {
        key_free(&cond->lower);
    }
    if (cond->has_upper) {
        key_free(&cond->upper);
    }
    memset(cond, 0, sizeof(*cond));
}

static bool condition_matches_key(const QueryCondition *cond, IndexKey key) {
    if (cond->has_lower) {
        int cmp = key_compare(key, cond->lower);
        if (cmp < 0 || (cmp == 0 && !cond->lower_inclusive)) {
            return false;
        }
    }
    if (cond->has_upper) {
        int cmp = key_compare(key, cond->upper);
        if (cmp > 0 || (cmp == 0 && !cond->upper_inclusive)) {
            return false;
        }
    }
    return true;
}

bool record_matches_condition(Record *r, const QueryCondition *cond) {
    IndexKey rk = key_from_record(cond->column, r);
    return condition_matches_key(cond, rk);
}

bool query_result_init(QueryResult *result, int initial_capacity) {
    result->count = 0;
    result->capacity = initial_capacity > 0 ? initial_capacity : 16;
    result->items = (Record **)malloc((size_t)result->capacity * sizeof(Record *));
    return result->items != NULL;
}

void query_result_free(QueryResult *result) {
    free(result->items);
    result->items = NULL;
    result->count = 0;
    result->capacity = 0;
}

bool query_result_add(QueryResult *result, Record *record) {
    if (result->count == result->capacity) {
        int new_cap = result->capacity * 2;
        Record **new_items = (Record **)realloc(result->items, (size_t)new_cap * sizeof(Record *));
        if (!new_items) {
            return false;
        }
        result->items = new_items;
        result->capacity = new_cap;
    }
    result->items[result->count++] = record;
    return true;
}

bool bplus_tree_collect_range(BPlusTree *tree, const QueryCondition *cond, QueryResult *result) {
    BPlusNode *leaf = cond->has_lower ? bplus_tree_find_leaf(tree, cond->lower) : bplus_tree_leftmost_leaf(tree);
    while (leaf) {
        int start = 0;
        if (cond->has_lower) {
            start = leaf_lower_bound(leaf, cond->lower);
        }
        for (int i = start; i < leaf->num_keys; i++) {
            IndexKey key = leaf->keys[i];
            if (cond->has_upper) {
                int upper_cmp = key_compare(key, cond->upper);
                if (upper_cmp > 0 || (upper_cmp == 0 && !cond->upper_inclusive)) {
                    return true;
                }
            }
            if (!condition_matches_key(cond, key)) {
                continue;
            }
            RecordRefList *refs = leaf->ptrs.leaf.values[i];
            for (int j = 0; j < refs->count; j++) {
                if (!query_result_add(result, refs->items[j])) {
                    return false;
                }
            }
        }
        leaf = leaf->ptrs.leaf.next;
    }
    return true;
}

static bool parse_condition_value(const char *sql, ColumnId col, const char *value_text,
                                  int value_start, IndexKey *out,
                                  int *err_start, int *err_len,
                                  char *err_kind, size_t err_kind_size,
                                  char *err_msg, size_t err_msg_size) {
    char valbuf[512];
    safe_copy(valbuf, sizeof(valbuf), value_text);
    str_trim_inplace(valbuf);
    if (valbuf[0] == '\0') {
        snprintf(err_kind, err_kind_size, "Syntax Error");
        snprintf(err_msg, err_msg_size, "Missing WHERE value.");
        *err_start = value_start;
        *err_len = 1;
        return false;
    }
    if (column_type(col) == TYPE_INT) {
        int v = 0;
        if (!parse_int_value(valbuf, &v)) {
            snprintf(err_kind, err_kind_size, "Type Error");
            snprintf(err_msg, err_msg_size, "Column '%s' expects INT, but got '%.160s'.", column_name(col), valbuf);
            *err_start = value_start;
            while (*err_start < (int)strlen(sql) && isspace((unsigned char)sql[*err_start])) (*err_start)++;
            *err_len = (int)strlen(valbuf);
            return false;
        }
        *out = make_int_key(v);
    } else {
        char strval[256];
        unquote_value(valbuf, strval, sizeof(strval));
        IndexKey borrowed = make_string_key_borrowed(strval);
        if (!key_clone(borrowed, out)) {
            snprintf(err_kind, err_kind_size, "Memory Error");
            snprintf(err_msg, err_msg_size, "Failed to allocate string key.");
            *err_start = value_start;
            *err_len = (int)strlen(valbuf);
            return false;
        }
    }
    return true;
}

static const char *find_operator_outside_quotes(const char *s, const char **op_out, int *op_len) {
    bool in_quote = false;
    char quote = '\0';
    for (const char *p = s; *p; p++) {
        if (*p == '\'' || *p == '"') {
            if (!in_quote) {
                in_quote = true;
                quote = *p;
            } else if (quote == *p && p > s && p[-1] != '\\') {
                in_quote = false;
            }
            continue;
        }
        if (in_quote) {
            continue;
        }
        if ((p[0] == '<' && p[1] == '=') || (p[0] == '>' && p[1] == '=')) {
            *op_out = p;
            *op_len = 2;
            return p;
        }
        if (p[0] == '<' || p[0] == '>' || p[0] == '=') {
            *op_out = p;
            *op_len = 1;
            return p;
        }
    }
    return NULL;
}

static bool parse_column_name_slice(const char *sql, const char *start, int len,
                                    ColumnId *col_out, int *err_start, int *err_len,
                                    char *err_kind, size_t err_kind_size,
                                    char *err_msg, size_t err_msg_size) {
    while (len > 0 && isspace((unsigned char)*start)) {
        start++;
        len--;
    }
    while (len > 0 && isspace((unsigned char)start[len - 1])) {
        len--;
    }
    if (len <= 0 || len >= 64) {
        snprintf(err_kind, err_kind_size, "Syntax Error");
        snprintf(err_msg, err_msg_size, "Invalid WHERE column.");
        *err_start = (int)(start - sql);
        *err_len = len > 0 ? len : 1;
        return false;
    }
    char colbuf[64];
    memcpy(colbuf, start, (size_t)len);
    colbuf[len] = '\0';
    ColumnId col = column_from_name(colbuf);
    if (col == COL_UNKNOWN) {
        snprintf(err_kind, err_kind_size, "Error");
        snprintf(err_msg, err_msg_size, "Unknown column '%s'.", colbuf);
        *err_start = (int)(start - sql);
        *err_len = len;
        return false;
    }
    *col_out = col;
    return true;
}

bool parse_condition(const char *sql, const char *where, QueryCondition *cond,
                            int *err_start, int *err_len,
                            char *err_kind, size_t err_kind_size,
                            char *err_msg, size_t err_msg_size) {
    memset(cond, 0, sizeof(*cond));
    const char *between = find_ci(where, " BETWEEN ");
    if (between) {
        ColumnId col = COL_UNKNOWN;
        if (!parse_column_name_slice(sql, where, (int)(between - where), &col,
                                     err_start, err_len, err_kind, err_kind_size, err_msg, err_msg_size)) {
            return false;
        }
        const char *value1 = between + strlen(" BETWEEN ");
        const char *and_kw = find_ci(value1, " AND ");
        if (!and_kw) {
            snprintf(err_kind, err_kind_size, "Syntax Error");
            snprintf(err_msg, err_msg_size, "BETWEEN requires AND.");
            *err_start = (int)(between - sql);
            *err_len = 7;
            return false;
        }
        char lower_text[512];
        char upper_text[512];
        int lower_len = (int)(and_kw - value1);
        if (lower_len >= (int)sizeof(lower_text)) lower_len = (int)sizeof(lower_text) - 1;
        memcpy(lower_text, value1, (size_t)lower_len);
        lower_text[lower_len] = '\0';
        safe_copy(upper_text, sizeof(upper_text), and_kw + strlen(" AND "));
        if (!parse_condition_value(sql, col, lower_text, (int)(value1 - sql),
                                   &cond->lower, err_start, err_len,
                                   err_kind, err_kind_size, err_msg, err_msg_size)) {
            return false;
        }
        cond->has_lower = true;
        if (!parse_condition_value(sql, col, upper_text, (int)(and_kw + strlen(" AND ") - sql),
                                   &cond->upper, err_start, err_len,
                                   err_kind, err_kind_size, err_msg, err_msg_size)) {
            query_condition_free(cond);
            return false;
        }
        cond->has_upper = true;
        cond->column = col;
        cond->op = OP_BETWEEN;
        cond->lower_inclusive = true;
        cond->upper_inclusive = true;
        if (key_compare(cond->lower, cond->upper) > 0) {
            snprintf(err_kind, err_kind_size, "Syntax Error");
            snprintf(err_msg, err_msg_size, "BETWEEN lower bound must be less than or equal to upper bound.");
            *err_start = (int)(value1 - sql);
            *err_len = (int)strlen(lower_text);
            query_condition_free(cond);
            return false;
        }
        return true;
    }

    const char *op = NULL;
    int op_len = 0;
    if (!find_operator_outside_quotes(where, &op, &op_len)) {
        snprintf(err_kind, err_kind_size, "Syntax Error");
        snprintf(err_msg, err_msg_size, "Expected one of '=', '<', '<=', '>', '>=', or BETWEEN.");
        *err_start = (int)(where - sql);
        *err_len = (int)strlen(where);
        return false;
    }
    ColumnId col = COL_UNKNOWN;
    if (!parse_column_name_slice(sql, where, (int)(op - where), &col,
                                 err_start, err_len, err_kind, err_kind_size, err_msg, err_msg_size)) {
        return false;
    }
    IndexKey value;
    memset(&value, 0, sizeof(value));
    if (!parse_condition_value(sql, col, op + op_len, (int)(op + op_len - sql),
                               &value, err_start, err_len,
                               err_kind, err_kind_size, err_msg, err_msg_size)) {
        return false;
    }
    cond->column = col;
    if (op_len == 1 && *op == '=') {
        cond->op = OP_EQ;
        cond->lower = value;
        cond->upper = value;
        if (value.type == KEY_STRING && !key_clone(value, &cond->upper)) {
            key_free(&value);
            snprintf(err_kind, err_kind_size, "Memory Error");
            snprintf(err_msg, err_msg_size, "Failed to allocate string key.");
            *err_start = (int)(op + op_len - sql);
            *err_len = 1;
            return false;
        }
        cond->has_lower = true;
        cond->has_upper = true;
        cond->lower_inclusive = true;
        cond->upper_inclusive = true;
    } else if (op_len == 1 && *op == '>') {
        cond->op = OP_GT;
        cond->lower = value;
        cond->has_lower = true;
        cond->lower_inclusive = false;
    } else if (op_len == 2 && op[0] == '>' && op[1] == '=') {
        cond->op = OP_GE;
        cond->lower = value;
        cond->has_lower = true;
        cond->lower_inclusive = true;
    } else if (op_len == 1 && *op == '<') {
        cond->op = OP_LT;
        cond->upper = value;
        cond->has_upper = true;
        cond->upper_inclusive = false;
    } else if (op_len == 2 && op[0] == '<' && op[1] == '=') {
        cond->op = OP_LE;
        cond->upper = value;
        cond->has_upper = true;
        cond->upper_inclusive = true;
    } else {
        key_free(&value);
        snprintf(err_kind, err_kind_size, "Syntax Error");
        snprintf(err_msg, err_msg_size, "Unsupported operator.");
        *err_start = (int)(op - sql);
        *err_len = op_len;
        return false;
    }
    return true;
}
