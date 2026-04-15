#include "engine_internal.h"
#include "ast.h"

static ExecStatus execute_insert(Database *db, const char *sql, double begin) {
    const char *into = find_ci(sql, "INSERT INTO");
    (void)into;
    const char *users = find_ci(sql, TABLE_NAME);
    if (!users) {
        print_error_timed("Syntax Error", "Expected table name 'users'.", sql, 0, 6, begin);
        return EXEC_ERROR;
    }
    const char *open_cols = strchr(users, '(');
    if (!open_cols) {
        print_error_timed("Syntax Error", "Expected column list.", sql, (int)(users - sql), 5, begin);
        return EXEC_ERROR;
    }
    const char *close_cols = find_matching_paren(open_cols);
    if (!close_cols) {
        print_error_timed("Syntax Error", "Unclosed column list.", sql, (int)(open_cols - sql), 1, begin);
        return EXEC_ERROR;
    }
    const char *values_kw = find_ci(close_cols, "VALUES");
    if (!values_kw) {
        print_error_timed("Syntax Error", "Expected VALUES.", sql, (int)(close_cols - sql), 1, begin);
        return EXEC_ERROR;
    }
    const char *open_vals = strchr(values_kw, '(');
    if (!open_vals) {
        print_error_timed("Syntax Error", "Expected value list.", sql, (int)(values_kw - sql), 6, begin);
        return EXEC_ERROR;
    }
    const char *close_vals = find_matching_paren(open_vals);
    if (!close_vals) {
        print_error_timed("Syntax Error", "Unclosed value list.", sql, (int)(open_vals - sql), 1, begin);
        return EXEC_ERROR;
    }
    ListItem cols[16];
    ListItem vals[16];
    int col_count = split_list_items(sql, (int)(open_cols - sql + 1), (int)(close_cols - open_cols - 1), cols, 16);
    int val_count = split_list_items(sql, (int)(open_vals - sql + 1), (int)(close_vals - open_vals - 1), vals, 16);
    if (col_count <= 0 || val_count <= 0 || col_count != val_count) {
        print_error_timed("Syntax Error", "Column count does not match value count.", sql,
                          (int)(open_cols - sql), (int)(close_vals - open_cols + 1), begin);
        return EXEC_ERROR;
    }

    bool seen[MAX_COLUMNS] = {false};
    bool has_name = false, has_age = false, has_email = false;
    Record *r = (Record *)calloc(1, sizeof(Record));
    if (!r) {
        print_error_timed("Memory Error", "Failed to allocate record.", sql, 0, 1, begin);
        return EXEC_ERROR;
    }
    r->id = db->table.next_auto_increment_id;
    for (int i = 0; i < col_count; i++) {
        ColumnId col = column_from_name(cols[i].text);
        if (col == COL_UNKNOWN) {
            char msg[256];
            snprintf(msg, sizeof(msg), "Unknown column '%.120s'.", cols[i].text);
            free(r);
            print_error_timed("Error", msg, sql, cols[i].start, cols[i].length, begin);
            return EXEC_ERROR;
        }
        if (col == COL_ID) {
            free(r);
            print_error_timed("Error", "Column 'id' is auto-generated and cannot be inserted manually.",
                              sql, cols[i].start, cols[i].length, begin);
            return EXEC_ERROR;
        }
        if (seen[col]) {
            char msg[256];
            snprintf(msg, sizeof(msg), "Duplicate column '%.120s'.", cols[i].text);
            free(r);
            print_error_timed("Error", msg, sql, cols[i].start, cols[i].length, begin);
            return EXEC_ERROR;
        }
        seen[col] = true;
        if (col == COL_NAME) {
            unquote_value(vals[i].text, r->name, sizeof(r->name));
            has_name = true;
        } else if (col == COL_EMAIL) {
            unquote_value(vals[i].text, r->email, sizeof(r->email));
            has_email = true;
        } else if (col == COL_AGE) {
            if (!parse_int_value(vals[i].text, &r->age)) {
                char msg[256];
                snprintf(msg, sizeof(msg), "Column 'age' expects INT, but got '%.120s'.", vals[i].text);
                free(r);
                print_error_timed("Type Error", msg, sql, vals[i].start, vals[i].length, begin);
                return EXEC_ERROR;
            }
            has_age = true;
        }
    }
    if (!has_name || !has_age || !has_email || r->name[0] == '\0' || r->email[0] == '\0') {
        free(r);
        print_error_timed("Error", "Missing required column or empty NOT NULL value.", sql,
                          (int)(open_cols - sql), (int)(close_cols - open_cols + 1), begin);
        return EXEC_ERROR;
    }
    for (int i = 0; i < db->index_count; i++) {
        if (!db->indexes[i].active || !db->indexes[i].unique) {
            continue;
        }
        IndexKey k = key_from_record(db->indexes[i].column, r);
        if (bplus_tree_search(db->indexes[i].tree, k)) {
            char keybuf[256], msg[512];
            key_to_string(k, keybuf, sizeof(keybuf));
            snprintf(msg, sizeof(msg), "Duplicate value %s for unique index '%s'.", keybuf, db->indexes[i].index_name);
            free(r);
            print_error_timed("Index Error", msg, sql, 0, 6, begin);
            return EXEC_ERROR;
        }
    }
    if (!table_add_record(&db->table, r)) {
        free(r);
        print_error_timed("Memory Error", "Failed to append record.", sql, 0, 6, begin);
        return EXEC_ERROR;
    }
    char err[256];
    for (int i = 0; i < db->index_count; i++) {
        if (db->indexes[i].active && !index_insert_record(&db->indexes[i], r, err, sizeof(err))) {
            print_error_timed("Index Error", err, sql, 0, 6, begin);
            return EXEC_ERROR;
        }
    }
    printf("Query OK, 1 row affected.\n");
    printf("Inserted id: %d\n", r->id);
    printf("Execution time: %.6f sec\n", now_sec() - begin);
    return EXEC_OK;
}

static ExecStatus execute_select(Database *db, const char *sql, double begin, bool explain) {
    const char *select_sql = sql;
    if (explain) {
        select_sql = find_ci(sql, "SELECT");
        if (!select_sql) {
            print_error_timed("Syntax Error", "Expected SELECT after EXPLAIN.", sql, 0, 7, begin);
            return EXEC_ERROR;
        }
    }
    const char *from = find_ci(select_sql, "FROM");
    if (!from || !find_ci(from, TABLE_NAME)) {
        print_error_timed("Syntax Error", "Expected FROM users.", sql, (int)(select_sql - sql), 6, begin);
        return EXEC_ERROR;
    }
    const char *force = find_ci(from, "FORCE INDEX");
    const char *ignore = find_ci(from, "IGNORE INDEX");
    const char *where_kw = find_ci(from, "WHERE");
    char hint_name[64] = "";
    bool force_index = false;
    bool ignore_index = false;
    if (force && (!where_kw || force < where_kw)) {
        force_index = true;
        const char *open = strchr(force, '(');
        const char *close = open ? strchr(open, ')') : NULL;
        if (!open || !close) {
            print_error_timed("Syntax Error", "Invalid FORCE INDEX syntax.", sql, (int)(force - sql), 11, begin);
            return EXEC_ERROR;
        }
        int len = (int)(close - open - 1);
        if (len >= (int)sizeof(hint_name)) len = (int)sizeof(hint_name) - 1;
        memcpy(hint_name, open + 1, (size_t)len);
        hint_name[len] = '\0';
        str_trim_inplace(hint_name);
    } else if (ignore && (!where_kw || ignore < where_kw)) {
        ignore_index = true;
        const char *open = strchr(ignore, '(');
        const char *close = open ? strchr(open, ')') : NULL;
        if (!open || !close) {
            print_error_timed("Syntax Error", "Invalid IGNORE INDEX syntax.", sql, (int)(ignore - sql), 12, begin);
            return EXEC_ERROR;
        }
        int len = (int)(close - open - 1);
        if (len >= (int)sizeof(hint_name)) len = (int)sizeof(hint_name) - 1;
        memcpy(hint_name, open + 1, (size_t)len);
        hint_name[len] = '\0';
        str_trim_inplace(hint_name);
    }

    QueryCondition cond;
    memset(&cond, 0, sizeof(cond));
    bool has_where = where_kw != NULL;
    char err_kind[64], err_msg[512];
    int err_start = 0, err_len = 1;
    if (has_where) {
        const char *cond_text = where_kw + 5;
        while (isspace((unsigned char)*cond_text)) cond_text++;
        if (!parse_condition(sql, cond_text, &cond, &err_start, &err_len,
                             err_kind, sizeof(err_kind), err_msg, sizeof(err_msg))) {
            print_error_timed(err_kind, err_msg, sql, err_start, err_len, begin);
            return EXEC_ERROR;
        }
    }

    IndexMetaRuntime *idx = NULL;
    const char *access = "Full Table Scan";
    const char *index_used = "none";
    bool use_index = false;
    if (has_where && force_index) {
        idx = find_index_by_name(db, hint_name);
        if (!idx) {
            char msg[256];
            snprintf(msg, sizeof(msg), "Index '%s' does not exist.", hint_name);
            query_condition_free(&cond);
            print_error_timed("Index Error", msg, sql, (int)(force - sql), 11, begin);
            return EXEC_ERROR;
        }
        if (idx->column != cond.column) {
            char msg[256];
            snprintf(msg, sizeof(msg), "Index '%s' cannot be used for column '%s'.", hint_name, column_name(cond.column));
            query_condition_free(&cond);
            print_error_timed("Index Error", msg, sql, (int)(force - sql), 11, begin);
            return EXEC_ERROR;
        }
        use_index = true;
    } else if (has_where && !ignore_index) {
        idx = find_index_by_column(db, cond.column);
        use_index = idx != NULL;
    }

    if (explain) {
        if (use_index) {
            access = cond.op == OP_EQ ? "B+ Tree Index Scan" : "B+ Tree Range Scan";
            index_used = idx->index_name;
        }
        printf("Execution Plan\n");
        printf("--------------------------------\n");
        printf("Table       : users\n");
        printf("Access Type : %s\n", access);
        printf("Index       : %s\n", index_used);
        printf("Column      : %s\n", has_where ? column_name(cond.column) : "none");
        printf("Estimated   : %s\n", use_index ? (cond.op == OP_EQ ? "O(log N)" : "O(log N + K)") : "O(N)");
        printf("--------------------------------\n");
        printf("1 row in set (%.6f sec)\n", now_sec() - begin);
        query_condition_free(&cond);
        return EXEC_OK;
    }

    Record **matches = NULL;
    int match_count = 0;
    int match_cap = 0;
    QueryResult range_result;
    memset(&range_result, 0, sizeof(range_result));
    bool matches_owned = false;
    if (has_where && use_index) {
        if (cond.op == OP_EQ) {
            RecordRefList *list = bplus_tree_search(idx->tree, cond.lower);
            if (list) {
                match_count = list->count;
                matches = list->items;
            }
        } else {
            if (!query_result_init(&range_result, 64)) {
                query_condition_free(&cond);
                print_error_timed("Memory Error", "Failed to allocate result buffer.", sql, 0, 6, begin);
                return EXEC_ERROR;
            }
            matches_owned = true;
            if (!bplus_tree_collect_range(idx->tree, &cond, &range_result)) {
                query_result_free(&range_result);
                query_condition_free(&cond);
                print_error_timed("Memory Error", "Failed to collect range scan results.", sql, 0, 6, begin);
                return EXEC_ERROR;
            }
            matches = range_result.items;
            match_count = range_result.count;
        }
        access = cond.op == OP_EQ ? "B+ Tree Index Scan" : "B+ Tree Range Scan";
        index_used = idx->index_name;
    } else {
        match_cap = db->table.count > 0 ? db->table.count : 1;
        matches = (Record **)malloc((size_t)match_cap * sizeof(Record *));
        if (!matches) {
            query_condition_free(&cond);
            print_error_timed("Memory Error", "Failed to allocate result buffer.", sql, 0, 6, begin);
            return EXEC_ERROR;
        }
        matches_owned = true;
        for (int i = 0; i < db->table.count; i++) {
            if (!has_where || record_matches_condition(db->table.rows[i], &cond)) {
                matches[match_count++] = db->table.rows[i];
            }
        }
        access = "Full Table Scan";
        index_used = "none";
    }
    double elapsed = now_sec() - begin;
    print_record_table(matches, match_count, elapsed, access, index_used);
    if (matches_owned) {
        if (range_result.items) {
            query_result_free(&range_result);
        } else {
            free(matches);
        }
    }
    query_condition_free(&cond);
    return EXEC_OK;
}

static ExecStatus execute_show_index(Database *db, double begin) {
    printf("+-------+------------------------------+--------+--------+--------+---------+\n");
    printf("| Table | Key_name                     | Column | Type   | Unique | Primary |\n");
    printf("+-------+------------------------------+--------+--------+--------+---------+\n");
    int rows = 0;
    for (int i = 0; i < db->index_count; i++) {
        if (!db->indexes[i].active) {
            continue;
        }
        printf("| %-5s | %-28s | %-6s | %-6s | %-6s | %-7s |\n",
               TABLE_NAME, db->indexes[i].index_name, column_name(db->indexes[i].column),
               "BTREE", db->indexes[i].unique ? "YES" : "NO",
               db->indexes[i].primary ? "YES" : "NO");
        rows++;
    }
    printf("+-------+------------------------------+--------+--------+--------+---------+\n");
    printf("%d rows in set (%.6f sec)\n", rows, now_sec() - begin);
    return EXEC_OK;
}

static ExecStatus execute_create_index(Database *db, const char *sql, double begin, bool unique) {
    const char *p = unique ? find_ci(sql, "CREATE UNIQUE INDEX") : find_ci(sql, "CREATE INDEX");
    if (!p) {
        print_error_timed("Syntax Error", "Invalid CREATE INDEX syntax.", sql, 0, 6, begin);
        return EXEC_ERROR;
    }
    p += unique ? strlen("CREATE UNIQUE INDEX") : strlen("CREATE INDEX");
    while (isspace((unsigned char)*p)) p++;
    char index_name[64];
    int n = 0;
    while (*p && !isspace((unsigned char)*p) && n + 1 < (int)sizeof(index_name)) {
        index_name[n++] = *p++;
    }
    index_name[n] = '\0';
    if (index_name[0] == '\0') {
        print_error_timed("Syntax Error", "Expected index name.", sql, (int)(p - sql), 1, begin);
        return EXEC_ERROR;
    }
    if (find_index_by_name(db, index_name)) {
        print_error_timed("Index Error", "Index name already exists.", sql, (int)(p - sql - n), n, begin);
        return EXEC_ERROR;
    }
    const char *on = find_ci(p, "ON");
    const char *open = on ? strchr(on, '(') : NULL;
    const char *close = open ? strchr(open, ')') : NULL;
    if (!on || !open || !close) {
        print_error_timed("Syntax Error", "Expected ON users (column).", sql, (int)(p - sql), 1, begin);
        return EXEC_ERROR;
    }
    char colbuf[64];
    int len = (int)(close - open - 1);
    if (len >= (int)sizeof(colbuf)) len = (int)sizeof(colbuf) - 1;
    memcpy(colbuf, open + 1, (size_t)len);
    colbuf[len] = '\0';
    str_trim_inplace(colbuf);
    ColumnId col = column_from_name(colbuf);
    if (col == COL_UNKNOWN) {
        char msg[256];
        snprintf(msg, sizeof(msg), "Unknown column '%s'.", colbuf);
        print_error_timed("Error", msg, sql, (int)(open + 1 - sql), len, begin);
        return EXEC_ERROR;
    }
    if (!add_index_meta(db, index_name, col, unique, false, true)) {
        print_error_timed("Index Error", "Too many indexes.", sql, 0, 6, begin);
        return EXEC_ERROR;
    }
    IndexMetaRuntime *idx = &db->indexes[db->index_count - 1];
    char err[512];
    if (!rebuild_one_index(db, idx, err, sizeof(err))) {
        db->index_count--;
        print_error_timed("Index Error", err, sql, (int)(open + 1 - sql), len, begin);
        return EXEC_ERROR;
    }
    if (db->persist) {
        save_indexes(db);
    }
    printf("Query OK, index created.\n");
    printf("Indexed rows: %d\n", db->table.count);
    printf("Execution time: %.6f sec\n", now_sec() - begin);
    return EXEC_OK;
}

static ExecStatus execute_drop_index(Database *db, const char *sql, double begin) {
    const char *p = find_ci(sql, "DROP INDEX");
    p += strlen("DROP INDEX");
    while (isspace((unsigned char)*p)) p++;
    char name[64];
    int n = 0;
    while (*p && !isspace((unsigned char)*p) && n + 1 < (int)sizeof(name)) {
        name[n++] = *p++;
    }
    name[n] = '\0';
    IndexMetaRuntime *idx = find_index_by_name(db, name);
    if (!idx) {
        print_error_timed("Index Error", "Index does not exist.", sql, (int)(p - sql - n), n, begin);
        return EXEC_ERROR;
    }
    if (idx->primary || str_casecmp_local(name, "PRIMARY") == 0) {
        print_error_timed("Index Error", "Primary key index cannot be dropped with DROP INDEX.",
                          sql, (int)(p - sql - n), n, begin);
        return EXEC_ERROR;
    }
    idx->active = false;
    bplus_tree_free(idx->tree);
    idx->tree = NULL;
    if (db->persist) {
        save_indexes(db);
    }
    printf("Query OK, index dropped.\n");
    printf("Execution time: %.6f sec\n", now_sec() - begin);
    return EXEC_OK;
}

static bool primary_candidate_valid(Database *db, ColumnId col, char *err, size_t err_size) {
    BPlusTree *tmp = bplus_tree_create(column_key_type(col), true);
    if (!tmp) {
        snprintf(err, err_size, "Failed to allocate temporary unique index.");
        return false;
    }
    for (int i = 0; i < db->table.count; i++) {
        Record *r = db->table.rows[i];
        if ((col == COL_NAME && r->name[0] == '\0') || (col == COL_EMAIL && r->email[0] == '\0')) {
            snprintf(err, err_size, "Primary key column '%s' cannot be empty.", column_name(col));
            bplus_tree_free(tmp);
            return false;
        }
        IndexKey key = key_from_record(col, r);
        bool dup = false;
        if (!bplus_tree_insert(tmp, key, r, &dup)) {
            char keybuf[256];
            key_to_string(key, keybuf, sizeof(keybuf));
            snprintf(err, err_size, "Cannot add primary key on column '%s'. Duplicate value found: %s.",
                     column_name(col), keybuf);
            bplus_tree_free(tmp);
            return false;
        }
    }
    bplus_tree_free(tmp);
    return true;
}

static ExecStatus execute_alter_primary(Database *db, const char *sql, double begin) {
    const char *open = strchr(sql, '(');
    const char *close = open ? strchr(open, ')') : NULL;
    if (!open || !close) {
        print_error_timed("Syntax Error", "Expected PRIMARY KEY (column).", sql, 0, 5, begin);
        return EXEC_ERROR;
    }
    char colbuf[64];
    int len = (int)(close - open - 1);
    if (len >= (int)sizeof(colbuf)) len = (int)sizeof(colbuf) - 1;
    memcpy(colbuf, open + 1, (size_t)len);
    colbuf[len] = '\0';
    str_trim_inplace(colbuf);
    ColumnId col = column_from_name(colbuf);
    if (col == COL_UNKNOWN) {
        char msg[256];
        snprintf(msg, sizeof(msg), "Unknown column '%s'.", colbuf);
        print_error_timed("Error", msg, sql, (int)(open + 1 - sql), len, begin);
        return EXEC_ERROR;
    }
    char err[512];
    if (!primary_candidate_valid(db, col, err, sizeof(err))) {
        print_error_timed("Index Error", err, sql, (int)(open + 1 - sql), len, begin);
        return EXEC_ERROR;
    }
    for (int i = 0; i < db->schema.column_count; i++) {
        db->schema.columns[i].primary_key = column_from_name(db->schema.columns[i].name) == col;
    }
    db->schema.primary_column = col;
    for (int i = 0; i < db->index_count; i++) {
        if (db->indexes[i].primary) {
            db->indexes[i].primary = false;
        }
    }
    IndexMetaRuntime *primary = find_index_by_name(db, "PRIMARY");
    if (!primary) {
        if (!add_index_meta(db, "PRIMARY", col, true, true, true)) {
            print_error_timed("Index Error", "Too many indexes.", sql, 0, 5, begin);
            return EXEC_ERROR;
        }
        primary = &db->indexes[db->index_count - 1];
    }
    primary->column = col;
    primary->key_type = column_key_type(col);
    primary->unique = true;
    primary->primary = true;
    primary->active = true;
    if (!rebuild_one_index(db, primary, err, sizeof(err))) {
        print_error_timed("Index Error", err, sql, (int)(open + 1 - sql), len, begin);
        return EXEC_ERROR;
    }
    if (col == COL_ID) {
        for (int i = 0; i < db->index_count; i++) {
            if (!db->indexes[i].primary && db->indexes[i].column == COL_ID &&
                str_casecmp_local(db->indexes[i].index_name, "idx_users_id") == 0) {
                db->indexes[i].active = false;
                bplus_tree_free(db->indexes[i].tree);
                db->indexes[i].tree = NULL;
            }
        }
    } else if (!find_index_by_column(db, COL_ID)) {
        if (!add_index_meta(db, "idx_users_id", COL_ID, true, false, true)) {
            print_error_timed("Index Error", "Too many indexes.", sql, 0, 5, begin);
            return EXEC_ERROR;
        }
        if (!rebuild_one_index(db, &db->indexes[db->index_count - 1], err, sizeof(err))) {
            print_error_timed("Index Error", err, sql, 0, 5, begin);
            return EXEC_ERROR;
        }
    }
    if (db->persist) {
        save_schema(db);
        save_indexes(db);
    }
    printf("Query OK.\n");
    printf("Primary key changed to '%s'.\n", column_name(col));
    printf("Unique B+ Tree index created on '%s'.\n", column_name(col));
    printf("Execution time: %.6f sec\n", now_sec() - begin);
    return EXEC_OK;
}

ExecStatus execute_save(Database *db, double begin) {
    if (!db->persist) {
        printf("Saved table 'users'.\n");
        printf("Rows        : %d\n", db->table.count);
        printf("Execution time: %.6f sec\n", now_sec() - begin);
        return EXEC_OK;
    }
    if (!db_save_all(db)) {
        printf("File Error: Failed to save table.\n");
        printf("Execution time: %.6f sec\n", now_sec() - begin);
        return EXEC_ERROR;
    }
    printf("Saved table 'users'.\n");
    printf("Schema file : %s\n", SCHEMA_FILE);
    printf("Index file  : %s\n", INDEX_FILE);
    printf("Data file   : %s\n", DATA_FILE);
    printf("Meta file   : %s\n", META_FILE);
    printf("Rows        : %d\n", db->table.count);
    printf("Execution time: %.6f sec\n", now_sec() - begin);
    return EXEC_OK;
}

static const char *extract_quoted_path(const char *sql, char *path, size_t path_size) {
    const char *q1 = strchr(sql, '\'');
    char quote = '\'';
    if (!q1) {
        q1 = strchr(sql, '"');
        quote = '"';
    }
    if (!q1) {
        return NULL;
    }
    const char *q2 = strchr(q1 + 1, quote);
    if (!q2) {
        return NULL;
    }
    size_t len = (size_t)(q2 - q1 - 1);
    if (len >= path_size) len = path_size - 1;
    memcpy(path, q1 + 1, len);
    path[len] = '\0';
    return q2 + 1;
}

static ExecStatus execute_load_schema(Database *db, const char *sql, double begin) {
    char path[512];
    if (!extract_quoted_path(sql, path, sizeof(path))) {
        print_error_timed("Syntax Error", "Expected schema path.", sql, 0, 4, begin);
        return EXEC_ERROR;
    }
    char err[512];
    if (!load_schema(db, path, err, sizeof(err))) {
        print_error_timed("Schema Error", err, sql, 0, 4, begin);
        return EXEC_ERROR;
    }
    if (db->persist) {
        save_schema(db);
    }
    printf("Query OK.\n");
    printf("Loaded schema: %s\n", path);
    printf("Execution time: %.6f sec\n", now_sec() - begin);
    return EXEC_OK;
}

static ExecStatus execute_load_data_binary(Database *db, const char *sql, double begin) {
    char path[512];
    if (!extract_quoted_path(sql, path, sizeof(path))) {
        print_error_timed("Syntax Error", "Expected binary data path.", sql, 0, 4, begin);
        return EXEC_ERROR;
    }
    bool replace = find_ci(sql, "APPEND") == NULL;
    char err[512];
    if (!load_data(db, path, replace, err, sizeof(err))) {
        print_error_timed("File Error", err, sql, 0, 4, begin);
        return EXEC_ERROR;
    }
    if (!rebuild_indexes(db, err, sizeof(err))) {
        print_error_timed("Index Error", err, sql, 0, 4, begin);
        return EXEC_ERROR;
    }
    printf("Query OK.\n");
    printf("Loaded rows: %d\n", db->table.count);
    printf("Indexes rebuilt: %d\n", db->index_count);
    printf("Execution time: %.6f sec\n", now_sec() - begin);
    return EXEC_OK;
}

static void db_reset_generated(Database *db) {
    table_clear(&db->table);
    db->table.next_auto_increment_id = 1;
    char err[256];
    rebuild_indexes(db, err, sizeof(err));
}

static bool ensure_index_on_column(Database *db, ColumnId col, bool unique, char *err, size_t err_size) {
    if (find_index_by_column(db, col)) {
        return true;
    }
    char name[64];
    snprintf(name, sizeof(name), "idx_users_%s", column_name(col));
    if (!add_index_meta(db, name, col, unique, false, true)) {
        snprintf(err, err_size, "Too many indexes.");
        return false;
    }
    return rebuild_one_index(db, &db->indexes[db->index_count - 1], err, err_size);
}

static ExecStatus execute_benchmark(Database *db, const char *sql, double begin) {
    const char *p = sql + strlen("BENCHMARK");
    while (isspace((unsigned char)*p)) p++;
    int n = atoi(p);
    if (n <= 0) {
        print_error_timed("Syntax Error", "Expected positive benchmark row count.", sql, 0, 9, begin);
        return EXEC_ERROR;
    }
    double t_gen0 = now_sec();
    db_reset_generated(db);
    double t_insert0 = now_sec();
    for (int i = 0; i < n; i++) {
        Record *r = (Record *)calloc(1, sizeof(Record));
        if (!r) {
            print_error_timed("Memory Error", "Failed to allocate benchmark record.", sql, 0, 9, begin);
            return EXEC_ERROR;
        }
        r->id = db->table.next_auto_increment_id;
        snprintf(r->name, sizeof(r->name), "user%d", r->id);
        r->age = 18 + (i % 70);
        snprintf(r->email, sizeof(r->email), "user%d@test.com", r->id);
        if (!table_add_record(&db->table, r)) {
            free(r);
            print_error_timed("Memory Error", "Failed to append benchmark record.", sql, 0, 9, begin);
            return EXEC_ERROR;
        }
    }
    double t_insert = now_sec() - t_insert0;
    double t_gen = t_insert0 - t_gen0;

    char err[512];
    const char *idx_clause = find_ci(sql, "INDEX");
    if (idx_clause) {
        const char *open = strchr(idx_clause, '(');
        const char *close = open ? strchr(open, ')') : NULL;
        if (open && close) {
            ListItem cols[16];
            int cnt = split_list_items(sql, (int)(open - sql + 1), (int)(close - open - 1), cols, 16);
            for (int i = 0; i < cnt; i++) {
                ColumnId col = column_from_name(cols[i].text);
                if (col != COL_UNKNOWN) {
                    ensure_index_on_column(db, col, false, err, sizeof(err));
                }
            }
        }
    }

    double t_build0 = now_sec();
    if (!rebuild_indexes(db, err, sizeof(err))) {
        print_error_timed("Index Error", err, sql, 0, 9, begin);
        return EXEC_ERROR;
    }
    double t_build = now_sec() - t_build0;

    double t_save0 = now_sec();
    if (db->persist) {
        save_data(db);
    }
    double t_binary_save = now_sec() - t_save0;
    double t_schema0 = now_sec();
    if (db->persist) {
        save_schema(db);
        save_indexes(db);
        save_meta(db);
    }
    double t_schema_save = now_sec() - t_schema0;

    int target_id = n;
    IndexKey idkey = make_int_key(target_id);
    double t_full0 = now_sec();
    int found_full = 0;
    for (int i = 0; i < db->table.count; i++) {
        if (db->table.rows[i]->id == target_id) {
            found_full++;
            break;
        }
    }
    double t_full = now_sec() - t_full0;

    IndexMetaRuntime *primary = find_index_by_name(db, "PRIMARY");
    double t_idx0 = now_sec();
    RecordRefList *id_found = primary ? bplus_tree_search(primary->tree, idkey) : NULL;
    double t_idx = now_sec() - t_idx0;

    printf("Performance Comparison\n");
    printf("--------------------------------------------------\n");
    printf("Records                    : %d\n", n);
    printf("Schema File                : %s\n", SCHEMA_FILE);
    printf("Index File                 : %s\n", INDEX_FILE);
    printf("Data File                  : %s\n", DATA_FILE);
    printf("Meta File                  : %s\n", META_FILE);
    printf("Indexes                    : ");
    for (int i = 0; i < db->index_count; i++) {
        if (db->indexes[i].active) {
            printf("%s%s", i == 0 ? "" : ", ", db->indexes[i].index_name);
        }
    }
    printf("\n\n");
    printf("Data Generation Time       : %.6f sec\n", t_gen);
    printf("Memory Insert Time         : %.6f sec\n", t_insert);
    printf("Binary Data Save Time      : %.6f sec\n", t_binary_save);
    printf("Schema/Index Save Time     : %.6f sec\n", t_schema_save);
    printf("Index Build Time           : %.6f sec\n\n", t_build);
    printf("SELECT by id full scan     : %.6f sec (matched %d)\n", t_full, found_full);
    printf("SELECT by id B+ Tree       : %.6f sec (matched %d)\n", t_idx, id_found ? id_found->count : 0);
    printf("\nTotal Benchmark Time       : %.6f sec\n", now_sec() - begin);
    printf("--------------------------------------------------\n");
    return EXEC_OK;
}

ExecStatus execute_command(Database *db, const char *input) {
    double begin = now_sec();
    SqlAst ast;
    char ast_err[256] = "";
    if (!sql_ast_parse(input, &ast, ast_err, sizeof(ast_err))) {
        print_error_timed("Syntax Error", ast_err, input ? input : "", 0, 1, begin);
        return EXEC_ERROR;
    }
    if (ast.kind == AST_EMPTY) {
        return EXEC_OK;
    }
    switch (ast.kind) {
        case AST_EXIT:
            execute_save(db, begin);
            return EXEC_EXIT;
        case AST_INSERT:
            return execute_insert(db, ast.sql, begin);
        case AST_EXPLAIN:
            return execute_select(db, ast.sql, begin, true);
        case AST_SELECT:
            return execute_select(db, ast.sql, begin, false);
        case AST_SHOW_INDEX:
            return execute_show_index(db, begin);
        case AST_CREATE_UNIQUE_INDEX:
            return execute_create_index(db, ast.sql, begin, true);
        case AST_CREATE_INDEX:
            return execute_create_index(db, ast.sql, begin, false);
        case AST_DROP_INDEX:
            return execute_drop_index(db, ast.sql, begin);
        case AST_ALTER_PRIMARY_KEY:
            return execute_alter_primary(db, ast.sql, begin);
        case AST_SAVE:
            return execute_save(db, begin);
        case AST_LOAD_SCHEMA:
            return execute_load_schema(db, ast.sql, begin);
        case AST_LOAD_DATA_BINARY:
            return execute_load_data_binary(db, ast.sql, begin);
        case AST_BENCHMARK:
            return execute_benchmark(db, ast.sql, begin);
        case AST_UNSUPPORTED:
        default:
            print_error_timed("Syntax Error", "Unsupported command.", ast.sql, 0, 1, begin);
            return EXEC_ERROR;
    }
}
