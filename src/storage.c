#include "engine_internal.h"

/*
 * storage.c
 *
 * 테이블 메모리 관리와 파일 저장/로드를 담당합니다.
 *
 * 저장 정책:
 * - schema와 index metadata는 사람이 읽을 수 있도록 CSV로 저장합니다.
 * - 실제 row 데이터는 100만 건 이상을 빠르게 다루기 위해 고정 크기 바이너리로 저장합니다.
 * - B+Tree 노드 자체는 파일에 저장하지 않고, 프로그램 시작 시 레코드로부터 다시 구성합니다.
 */

/* Table은 Record* 배열을 소유하며, auto_increment 시작값은 1입니다. */
void table_init(Table *table) {
    memset(table, 0, sizeof(*table));
    table->next_auto_increment_id = 1;
}

/* Table이 소유한 모든 Record와 rows 배열을 해제하고 초기 상태로 되돌립니다. */
void table_clear(Table *table) {
    for (int i = 0; i < table->count; i++) {
        free(table->rows[i]);
    }
    free(table->rows);
    table_init(table);
}

/* Record* 배열이 가득 차면 2배로 늘려 대량 insert 비용을 줄입니다. */
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
    /* 외부 바이너리 데이터를 로드해도 다음 id가 기존 최대 id보다 크게 유지되도록 보정합니다. */
    if (record->id >= table->next_auto_increment_id) {
        table->next_auto_increment_id = record->id + 1;
    }
    return true;
}

/* users 테이블의 기본 schema입니다. schema 파일이 없을 때 이 구조로 시작합니다. */
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

/* Database가 가진 모든 B+Tree 인덱스를 해제합니다. Record는 Table이 관리하므로 여기서 건드리지 않습니다. */
void free_indexes(Database *db) {
    for (int i = 0; i < db->index_count; i++) {
        bplus_tree_free(db->indexes[i].tree);
        db->indexes[i].tree = NULL;
    }
    db->index_count = 0;
}

/* 새 Database 객체를 사용할 수 있는 기본 상태로 초기화합니다. */
void db_init(Database *db) {
    memset(db, 0, sizeof(*db));
    db->persist = true;
    schema_set_default(&db->schema);
    table_init(&db->table);
}

/* DB 종료 시 index tree와 table row 메모리를 모두 정리합니다. */
void db_clear(Database *db) {
    free_indexes(db);
    table_clear(&db->table);
}

/* data/ 폴더가 없으면 생성합니다. Docker volume을 마운트한 경우에도 같은 경로를 사용합니다. */
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
    /*
     * schema CSV 저장.
     *
     * tmp 파일에 먼저 완성본을 쓴 뒤 rename하여, 저장 중 실패해도 기존 파일 손상 가능성을 줄입니다.
     */
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
    /*
     * schema CSV 로드.
     *
     * header 이름으로 컬럼 위치를 찾기 때문에 CSV 컬럼 순서가 바뀌어도 동작합니다.
     */
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
    /* column_name,type,size,...가 몇 번째 필드인지 header를 보고 찾습니다. */
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
    /* 각 row를 ColumnMeta로 변환합니다. 알 수 없는 컬럼은 무시합니다. */
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
    /* 과제 테이블은 users 고정 4컬럼이며 primary key는 정확히 하나여야 합니다. */
    if (next.column_count != MAX_COLUMNS || pk_count != 1) {
        snprintf(err, err_size, "Invalid schema file. Expected 4 columns and exactly one primary key.");
        return false;
    }
    db->schema = next;
    return true;
}

bool save_meta(Database *db) {
    /* 바이너리 데이터 검증에 필요한 record_size, row count, 다음 auto_increment 값을 저장합니다. */
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
    /* meta 파일은 key=value 형식이라 필요한 값만 찾아 채웁니다. 없으면 기본값을 유지합니다. */
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
    /*
     * 실제 row를 바이너리로 저장합니다.
     * RecordDisk는 고정 크기 구조체라 fwrite/fread로 빠르게 처리할 수 있습니다.
     */
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
    /*
     * 인덱스는 B+Tree 노드가 아니라 메타데이터만 저장합니다.
     * 재시작 시 이 메타데이터와 data 파일의 row를 이용해 B+Tree를 다시 만듭니다.
     */
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
    /* FORCE INDEX, DROP INDEX, SHOW INDEX 등에서 이름으로 active index를 찾습니다. */
    for (int i = 0; i < db->index_count; i++) {
        if (db->indexes[i].active && str_casecmp_local(db->indexes[i].index_name, name) == 0) {
            return &db->indexes[i];
        }
    }
    return NULL;
}

IndexMetaRuntime *find_index_by_column(Database *db, ColumnId col) {
    /*
     * WHERE 컬럼에 맞는 index 자동 선택.
     * 같은 컬럼에 여러 index가 있을 수 있으므로 PRIMARY를 우선 반환합니다.
     */
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
    /* Record의 특정 컬럼 값을 B+Tree에서 사용할 IndexKey로 변환합니다. */
    switch (col) {
        case COL_ID: return make_int_key(r->id);
        case COL_AGE: return make_int_key(r->age);
        case COL_NAME: return make_string_key_borrowed(r->name);
        case COL_EMAIL: return make_string_key_borrowed(r->email);
        default: return make_int_key(0);
    }
}

bool index_insert_record(IndexMetaRuntime *idx, Record *r, char *err, size_t err_size) {
    /* 인덱스 컬럼 값을 key로 뽑아 B+Tree에 key -> Record*를 등록합니다. */
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
    /*
     * 특정 인덱스 하나를 처음부터 다시 만듭니다.
     * CREATE INDEX, ALTER PRIMARY KEY, startup load 후 index 복구에 사용됩니다.
     */
    bplus_tree_free(idx->tree);
    idx->tree = bplus_tree_create(idx->key_type, idx->unique);
    if (!idx->tree) {
        snprintf(err, err_size, "Failed to allocate B+ tree for index '%s'.", idx->index_name);
        return false;
    }
    /* 이미 메모리에 올라온 모든 row를 새 B+Tree에 삽입합니다. */
    for (int i = 0; i < db->table.count; i++) {
        if (!index_insert_record(idx, db->table.rows[i], err, err_size)) {
            return false;
        }
    }
    return true;
}

bool rebuild_indexes(Database *db, char *err, size_t err_size) {
    /* active index 전체를 재구성합니다. 대량 load/benchmark 이후 호출됩니다. */
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
    /* B+Tree 생성 전 단계의 인덱스 메타데이터를 indexes 배열에 추가합니다. */
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
    /*
     * index metadata CSV 로드.
     * 파일이 없거나 비어 있으면 PRIMARY 인덱스를 기본 생성합니다.
     */
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
    /* schema와 마찬가지로 header 이름으로 필드 위치를 찾아 CSV 컬럼 순서 변경에 대응합니다. */
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
    /* 각 CSV row를 IndexMetaRuntime으로 변환합니다. 실제 B+Tree는 rebuild 단계에서 만들어집니다. */
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
    /*
     * 바이너리 RecordDisk 배열을 메모리 Table로 로드합니다.
     * replace=true이면 기존 table을 비우고, false이면 APPEND처럼 뒤에 붙입니다.
     */
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
        /* 파일 크기가 record 크기의 배수가 아니면 중간에 깨진 파일로 판단합니다. */
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
        /* 바이너리 파일에서 읽은 고정 문자열이 항상 null-terminated 되도록 방어합니다. */
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
    /* SAVE 또는 종료 시 schema/index/data/meta를 한 번에 저장합니다. */
    ensure_data_dir();
    return save_schema(db) && save_indexes(db) && save_data(db) && save_meta(db);
}

bool db_load_startup(Database *db, char *err, size_t err_size, double begin) {
    /*
     * 프로그램 시작 시 data/ 폴더의 파일들을 읽어 DB를 복구합니다.
     * B+Tree는 파일에서 읽지 않고, load_data 후 rebuild_indexes로 메모리에 재생성합니다.
     */
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
        /* 구조체 크기가 달라졌다면 기존 바이너리 파일을 현재 코드로 안전하게 읽을 수 없습니다. */
        snprintf(err, err_size, "Meta record_size does not match current record size.");
        return false;
    }
    if (!load_data(db, DATA_FILE, true, err, err_size)) {
        return false;
    }
    if (have_meta) {
        db->table.next_auto_increment_id = meta.next_auto_increment_id;
        if (meta.record_count != db->table.count) {
            /* meta와 data 파일이 서로 다른 시점의 파일이면 row count가 맞지 않을 수 있습니다. */
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
