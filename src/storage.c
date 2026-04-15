#include "engine_internal.h"

void table_init(Table *table) {
    memset(table, 0, sizeof(*table));
    table->next_auto_increment_id = 1;
}

void table_clear(Table *table) {
    for (int i = 0; i < table->count; i++) {
        free(table->rows[i]);
    }
    free(table->rows);
    table_init(table);
}

bool table_add_record(Table *table, Record *record) {
    if (table->count == table->capacity) {
        int new_cap = table->capacity == 0 ? 1024 : table->capacity * 2;
        Record **new_rows = (Record **)realloc(table->rows, (size_t)new_cap * sizeof(Record *));
        if (!new_rows) {
            return false;
        }
        table->rows = new_rows;
        table->capacity = new_cap;
    }
    table->rows[table->count++] = record;
    if (record->id >= table->next_auto_increment_id) {
        table->next_auto_increment_id = record->id + 1;
    }
    return true;
}

void schema_set_default(Schema *schema) {
    memset(schema, 0, sizeof(*schema));
    schema->column_count = 4;
    safe_copy(schema->columns[0].name, sizeof(schema->columns[0].name), "id");
    schema->columns[0].type = TYPE_INT;
    schema->columns[0].size = 4;
    schema->columns[0].not_null = true;
    schema->columns[0].auto_increment = true;
    schema->columns[0].primary_key = true;

    safe_copy(schema->columns[1].name, sizeof(schema->columns[1].name), "name");
    schema->columns[1].type = TYPE_VARCHAR;
    schema->columns[1].size = MAX_NAME_LEN;
    schema->columns[1].not_null = true;

    safe_copy(schema->columns[2].name, sizeof(schema->columns[2].name), "age");
    schema->columns[2].type = TYPE_INT;
    schema->columns[2].size = 4;
    schema->columns[2].not_null = true;

    safe_copy(schema->columns[3].name, sizeof(schema->columns[3].name), "email");
    schema->columns[3].type = TYPE_VARCHAR;
    schema->columns[3].size = MAX_EMAIL_LEN;
    schema->columns[3].not_null = true;
    schema->primary_column = COL_ID;
}

void free_indexes(Database *db) {
    for (int i = 0; i < db->index_count; i++) {
        bplus_tree_free(db->indexes[i].tree);
        db->indexes[i].tree = NULL;
    }
    db->index_count = 0;
}

void db_init(Database *db) {
    memset(db, 0, sizeof(*db));
    db->persist = true;
    schema_set_default(&db->schema);
    table_init(&db->table);
}

void db_clear(Database *db) {
    free_indexes(db);
    table_clear(&db->table);
}

static void ensure_data_dir(void) {
    struct stat st;
    if (stat(DATA_DIR, &st) != 0) {
        mkdir(DATA_DIR, 0755);
    }
}

static bool bool_from_text(const char *s) {
    return str_casecmp_local(s, "true") == 0 || strcmp(s, "1") == 0 ||
           str_casecmp_local(s, "yes") == 0;
}

static int split_csv_simple(char *line, char **fields, int max_fields) {
    int count = 0;
    char *p = line;
    while (count < max_fields) {
        fields[count++] = p;
        char *comma = strchr(p, ',');
        if (!comma) {
            break;
        }
        *comma = '\0';
        p = comma + 1;
    }
    for (int i = 0; i < count; i++) {
        str_trim_inplace(fields[i]);
    }
    return count;
}

bool save_schema(Database *db) {
    ensure_data_dir();
    FILE *fp = fopen(SCHEMA_FILE ".tmp", "w");
    if (!fp) {
        return false;
    }
    fprintf(fp, "column_name,type,size,not_null,auto_increment,primary_key\n");
    for (int i = 0; i < db->schema.column_count; i++) {
        ColumnMeta *c = &db->schema.columns[i];
        fprintf(fp, "%s,%s,%d,%s,%s,%s\n", c->name, type_name(c->type), c->size,
                c->not_null ? "true" : "false",
                c->auto_increment ? "true" : "false",
                c->primary_key ? "true" : "false");
    }
    if (fclose(fp) != 0) {
        return false;
    }
    return rename(SCHEMA_FILE ".tmp", SCHEMA_FILE) == 0;
}

bool load_schema(Database *db, const char *path, char *err, size_t err_size) {
    FILE *fp = fopen(path, "r");
    if (!fp) {
        schema_set_default(&db->schema);
        return db->persist ? save_schema(db) : true;
    }
    char line[1024];
    if (!fgets(line, sizeof(line), fp)) {
        fclose(fp);
        snprintf(err, err_size, "Invalid schema file. Empty file.");
        return false;
    }
    line[strcspn(line, "\r\n")] = '\0';
    char *headers[16];
    int header_count = split_csv_simple(line, headers, 16);
    int idx_name = -1, idx_type = -1, idx_size = -1, idx_not_null = -1, idx_auto = -1, idx_pk = -1;
    for (int i = 0; i < header_count; i++) {
        if (str_casecmp_local(headers[i], "column_name") == 0) idx_name = i;
        else if (str_casecmp_local(headers[i], "type") == 0) idx_type = i;
        else if (str_casecmp_local(headers[i], "size") == 0) idx_size = i;
        else if (str_casecmp_local(headers[i], "not_null") == 0) idx_not_null = i;
        else if (str_casecmp_local(headers[i], "auto_increment") == 0) idx_auto = i;
        else if (str_casecmp_local(headers[i], "primary_key") == 0) idx_pk = i;
    }
    if (idx_name < 0 || idx_type < 0 || idx_size < 0 || idx_not_null < 0 || idx_auto < 0 || idx_pk < 0) {
        fclose(fp);
        snprintf(err, err_size, "Invalid schema file. Missing required header.");
        return false;
    }
    Schema next;
    memset(&next, 0, sizeof(next));
    int pk_count = 0;
    while (fgets(line, sizeof(line), fp)) {
        line[strcspn(line, "\r\n")] = '\0';
        if (line[0] == '\0') {
            continue;
        }
        char *fields[16];
        int n = split_csv_simple(line, fields, 16);
        int need = idx_name;
        if (idx_type > need) need = idx_type;
        if (idx_size > need) need = idx_size;
        if (idx_not_null > need) need = idx_not_null;
        if (idx_auto > need) need = idx_auto;
        if (idx_pk > need) need = idx_pk;
        if (n <= need || next.column_count >= MAX_COLUMNS) {
            fclose(fp);
            snprintf(err, err_size, "Invalid schema row.");
            return false;
        }
        ColumnId col = column_from_name(fields[idx_name]);
        if (col == COL_UNKNOWN) {
            continue;
        }
        ColumnMeta *c = &next.columns[next.column_count++];
        safe_copy(c->name, sizeof(c->name), fields[idx_name]);
        c->type = str_casecmp_local(fields[idx_type], "INT") == 0 ? TYPE_INT : TYPE_VARCHAR;
        c->size = atoi(fields[idx_size]);
        c->not_null = bool_from_text(fields[idx_not_null]);
        c->auto_increment = bool_from_text(fields[idx_auto]);
        c->primary_key = bool_from_text(fields[idx_pk]);
        if (c->primary_key) {
            pk_count++;
            next.primary_column = col;
        }
    }
    fclose(fp);
    if (next.column_count != MAX_COLUMNS || pk_count != 1) {
        snprintf(err, err_size, "Invalid schema file. Expected 4 columns and exactly one primary key.");
        return false;
    }
    db->schema = next;
    return true;
}

bool save_meta(Database *db) {
    FILE *fp = fopen(META_FILE ".tmp", "w");
    if (!fp) {
        return false;
    }
    fprintf(fp, "version=1\n");
    fprintf(fp, "record_size=%zu\n", sizeof(RecordDisk));
    fprintf(fp, "record_count=%d\n", db->table.count);
    fprintf(fp, "next_auto_increment_id=%d\n", db->table.next_auto_increment_id);
    if (fclose(fp) != 0) {
        return false;
    }
    return rename(META_FILE ".tmp", META_FILE) == 0;
}

static bool load_meta(TableMeta *meta) {
    memset(meta, 0, sizeof(*meta));
    meta->version = 1;
    meta->record_size = (int)sizeof(RecordDisk);
    meta->next_auto_increment_id = 1;
    FILE *fp = fopen(META_FILE, "r");
    if (!fp) {
        return false;
    }
    char line[256];
    while (fgets(line, sizeof(line), fp)) {
        char *eq = strchr(line, '=');
        if (!eq) {
            continue;
        }
        *eq = '\0';
        char *key = line;
        char *val = eq + 1;
        val[strcspn(val, "\r\n")] = '\0';
        str_trim_inplace(key);
        str_trim_inplace(val);
        if (strcmp(key, "version") == 0) meta->version = atoi(val);
        else if (strcmp(key, "record_size") == 0) meta->record_size = atoi(val);
        else if (strcmp(key, "record_count") == 0) meta->record_count = atoi(val);
        else if (strcmp(key, "next_auto_increment_id") == 0) meta->next_auto_increment_id = atoi(val);
    }
    fclose(fp);
    return true;
}

bool save_data(Database *db) {
    FILE *fp = fopen(DATA_FILE ".tmp", "wb");
    if (!fp) {
        return false;
    }
    for (int i = 0; i < db->table.count; i++) {
        if (fwrite(db->table.rows[i], sizeof(RecordDisk), 1, fp) != 1) {
            fclose(fp);
            return false;
        }
    }
    if (fclose(fp) != 0) {
        return false;
    }
    return rename(DATA_FILE ".tmp", DATA_FILE) == 0;
}

bool save_indexes(Database *db) {
    FILE *fp = fopen(INDEX_FILE ".tmp", "w");
    if (!fp) {
        return false;
    }
    fprintf(fp, "index_name,column_name,type,unique,primary,active\n");
    for (int i = 0; i < db->index_count; i++) {
        IndexMetaRuntime *idx = &db->indexes[i];
        fprintf(fp, "%s,%s,BTREE,%s,%s,%s\n", idx->index_name, column_name(idx->column),
                idx->unique ? "true" : "false",
                idx->primary ? "true" : "false",
                idx->active ? "true" : "false");
    }
    if (fclose(fp) != 0) {
        return false;
    }
    return rename(INDEX_FILE ".tmp", INDEX_FILE) == 0;
}

IndexMetaRuntime *find_index_by_name(Database *db, const char *name) {
    for (int i = 0; i < db->index_count; i++) {
        if (db->indexes[i].active && str_casecmp_local(db->indexes[i].index_name, name) == 0) {
            return &db->indexes[i];
        }
    }
    return NULL;
}

IndexMetaRuntime *find_index_by_column(Database *db, ColumnId col) {
    for (int i = 0; i < db->index_count; i++) {
        if (db->indexes[i].active && db->indexes[i].column == col) {
            if (db->indexes[i].primary) {
                return &db->indexes[i];
            }
        }
    }
    for (int i = 0; i < db->index_count; i++) {
        if (db->indexes[i].active && db->indexes[i].column == col) {
            return &db->indexes[i];
        }
    }
    return NULL;
}

IndexKey key_from_record(ColumnId col, Record *r) {
    switch (col) {
        case COL_ID: return make_int_key(r->id);
        case COL_AGE: return make_int_key(r->age);
        case COL_NAME: return make_string_key_borrowed(r->name);
        case COL_EMAIL: return make_string_key_borrowed(r->email);
        default: return make_int_key(0);
    }
}

bool index_insert_record(IndexMetaRuntime *idx, Record *r, char *err, size_t err_size) {
    IndexKey key = key_from_record(idx->column, r);
    bool duplicate = false;
    if (!bplus_tree_insert(idx->tree, key, r, &duplicate)) {
        if (duplicate) {
            char keybuf[256];
            key_to_string(key, keybuf, sizeof(keybuf));
            snprintf(err, err_size, "Duplicate value %s for unique index '%s'.", keybuf, idx->index_name);
        } else {
            snprintf(err, err_size, "Failed to insert into index '%s'.", idx->index_name);
        }
        return false;
    }
    return true;
}

bool rebuild_one_index(Database *db, IndexMetaRuntime *idx, char *err, size_t err_size) {
    bplus_tree_free(idx->tree);
    idx->tree = bplus_tree_create(idx->key_type, idx->unique);
    if (!idx->tree) {
        snprintf(err, err_size, "Failed to allocate B+ tree for index '%s'.", idx->index_name);
        return false;
    }
    for (int i = 0; i < db->table.count; i++) {
        if (!index_insert_record(idx, db->table.rows[i], err, err_size)) {
            return false;
        }
    }
    return true;
}

bool rebuild_indexes(Database *db, char *err, size_t err_size) {
    for (int i = 0; i < db->index_count; i++) {
        if (db->indexes[i].active) {
            if (!rebuild_one_index(db, &db->indexes[i], err, err_size)) {
                return false;
            }
        }
    }
    return true;
}

bool add_index_meta(Database *db, const char *index_name, ColumnId col,
                           bool unique, bool primary, bool active) {
    if (db->index_count >= MAX_INDEXES) {
        return false;
    }
    IndexMetaRuntime *idx = &db->indexes[db->index_count++];
    memset(idx, 0, sizeof(*idx));
    safe_copy(idx->index_name, sizeof(idx->index_name), index_name);
    idx->column = col;
    idx->key_type = column_key_type(col);
    idx->unique = unique;
    idx->primary = primary;
    idx->active = active;
    return true;
}

static bool load_indexes(Database *db, const char *path, char *err, size_t err_size) {
    FILE *fp = fopen(path, "r");
    if (!fp) {
        db->index_count = 0;
        bool ok = add_index_meta(db, "PRIMARY", db->schema.primary_column, true, true, true);
        return ok && (db->persist ? save_indexes(db) : true);
    }
    db->index_count = 0;
    char line[1024];
    if (!fgets(line, sizeof(line), fp)) {
        fclose(fp);
        return add_index_meta(db, "PRIMARY", db->schema.primary_column, true, true, true);
    }
    line[strcspn(line, "\r\n")] = '\0';
    char *headers[16];
    int header_count = split_csv_simple(line, headers, 16);
    int idx_name = -1, idx_col = -1, idx_unique = -1, idx_primary = -1, idx_active = -1;
    for (int i = 0; i < header_count; i++) {
        if (str_casecmp_local(headers[i], "index_name") == 0) idx_name = i;
        else if (str_casecmp_local(headers[i], "column_name") == 0) idx_col = i;
        else if (str_casecmp_local(headers[i], "unique") == 0) idx_unique = i;
        else if (str_casecmp_local(headers[i], "primary") == 0) idx_primary = i;
        else if (str_casecmp_local(headers[i], "active") == 0) idx_active = i;
    }
    if (idx_name < 0 || idx_col < 0 || idx_unique < 0 || idx_primary < 0 || idx_active < 0) {
        fclose(fp);
        snprintf(err, err_size, "Invalid indexes file. Missing required header.");
        return false;
    }
    bool has_primary = false;
    while (fgets(line, sizeof(line), fp)) {
        line[strcspn(line, "\r\n")] = '\0';
        if (line[0] == '\0') {
            continue;
        }
        char *fields[16];
        int n = split_csv_simple(line, fields, 16);
        int need = idx_name;
        if (idx_col > need) need = idx_col;
        if (idx_unique > need) need = idx_unique;
        if (idx_primary > need) need = idx_primary;
        if (idx_active > need) need = idx_active;
        if (n <= need) {
            fclose(fp);
            snprintf(err, err_size, "Invalid indexes row.");
            return false;
        }
        ColumnId col = column_from_name(fields[idx_col]);
        if (col == COL_UNKNOWN) {
            fclose(fp);
            snprintf(err, err_size, "Index references unknown column '%s'.", fields[idx_col]);
            return false;
        }
        bool primary = bool_from_text(fields[idx_primary]);
        if (primary) {
            has_primary = true;
        }
        if (!add_index_meta(db, fields[idx_name], col, bool_from_text(fields[idx_unique]),
                            primary, bool_from_text(fields[idx_active]))) {
            fclose(fp);
            snprintf(err, err_size, "Too many indexes.");
            return false;
        }
    }
    fclose(fp);
    if (!has_primary) {
        add_index_meta(db, "PRIMARY", db->schema.primary_column, true, true, true);
    }
    return true;
}

bool load_data(Database *db, const char *path, bool replace, char *err, size_t err_size) {
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        if (replace) {
            table_clear(&db->table);
        }
        return true;
    }
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        snprintf(err, err_size, "Failed to seek data file.");
        return false;
    }
    long size = ftell(fp);
    if (size < 0) {
        fclose(fp);
        snprintf(err, err_size, "Failed to inspect data file.");
        return false;
    }
    if (size % (long)sizeof(RecordDisk) != 0) {
        fclose(fp);
        snprintf(err, err_size, "Data file size does not match record size.");
        return false;
    }
    rewind(fp);
    if (replace) {
        table_clear(&db->table);
    }
    int rows = (int)(size / (long)sizeof(RecordDisk));
    for (int i = 0; i < rows; i++) {
        Record *r = (Record *)calloc(1, sizeof(Record));
        if (!r) {
            fclose(fp);
            snprintf(err, err_size, "Failed to allocate record.");
            return false;
        }
        if (fread(r, sizeof(RecordDisk), 1, fp) != 1) {
            free(r);
            fclose(fp);
            snprintf(err, err_size, "Failed to read record data.");
            return false;
        }
        r->name[MAX_NAME_LEN - 1] = '\0';
        r->email[MAX_EMAIL_LEN - 1] = '\0';
        if (!table_add_record(&db->table, r)) {
            free(r);
            fclose(fp);
            snprintf(err, err_size, "Failed to store record in memory.");
            return false;
        }
    }
    fclose(fp);
    return true;
}

bool db_save_all(Database *db) {
    ensure_data_dir();
    return save_schema(db) && save_indexes(db) && save_data(db) && save_meta(db);
}

bool db_load_startup(Database *db, char *err, size_t err_size, double begin) {
    ensure_data_dir();
    if (!load_schema(db, SCHEMA_FILE, err, err_size)) {
        return false;
    }
    if (!load_indexes(db, INDEX_FILE, err, err_size)) {
        return false;
    }
    TableMeta meta;
    bool have_meta = load_meta(&meta);
    if (have_meta && meta.record_size != (int)sizeof(RecordDisk)) {
        snprintf(err, err_size, "Meta record_size does not match current record size.");
        return false;
    }
    if (!load_data(db, DATA_FILE, true, err, err_size)) {
        return false;
    }
    if (have_meta) {
        db->table.next_auto_increment_id = meta.next_auto_increment_id;
        if (meta.record_count != db->table.count) {
            snprintf(err, err_size, "Meta record_count does not match data file.");
            return false;
        }
    }
    if (!rebuild_indexes(db, err, err_size)) {
        return false;
    }
    printf("Loaded table 'users'.\n");
    printf("Schema columns : %d\n", db->schema.column_count);
    printf("Indexes        : %d\n", db->index_count);
    printf("Rows           : %d\n", db->table.count);
    printf("Indexes rebuilt: %d\n", db->index_count);
    printf("Execution time : %.6f sec\n", now_sec() - begin);
    return true;
}
