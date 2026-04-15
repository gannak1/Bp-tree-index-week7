#ifndef MYSQL_BPTREE_INTERNAL_H
#define MYSQL_BPTREE_INTERNAL_H

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#define DATA_DIR "data"
#define SCHEMA_FILE "data/users.schema.csv"
#define INDEX_FILE "data/users.indexes.csv"
#define DATA_FILE "data/users.data"
#define META_FILE "data/users.meta"

#define TABLE_NAME "users"
#define MAX_NAME_LEN 64
#define MAX_EMAIL_LEN 128
#define MAX_COLUMNS 4
#define MAX_INDEXES 32
#define SQL_BUF_SIZE 8192

#define BPLUS_TREE_NODE_SIZE_LIMIT 4096
#define BPLUS_TREE_TARGET_ORDER 340
#define BPLUS_TREE_ORDER 160
#define BPLUS_TREE_MAX_KEYS BPLUS_TREE_ORDER

typedef enum {
    COL_ID = 0,
    COL_NAME = 1,
    COL_AGE = 2,
    COL_EMAIL = 3,
    COL_UNKNOWN = -1
} ColumnId;

typedef enum {
    TYPE_INT = 1,
    TYPE_VARCHAR = 2
} ColumnType;

typedef enum {
    KEY_INT = 1,
    KEY_STRING = 2
} KeyType;

typedef enum {
    OP_EQ = 1,
    OP_LT,
    OP_LE,
    OP_GT,
    OP_GE,
    OP_BETWEEN
} ConditionOp;

typedef struct {
    int id;
    char name[MAX_NAME_LEN];
    int age;
    char email[MAX_EMAIL_LEN];
} RecordDisk;

typedef RecordDisk Record;

typedef struct {
    char name[32];
    ColumnType type;
    int size;
    bool not_null;
    bool auto_increment;
    bool primary_key;
} ColumnMeta;

typedef struct {
    int version;
    int record_size;
    int record_count;
    int next_auto_increment_id;
} TableMeta;

typedef struct {
    KeyType type;
    union {
        int int_value;
        char *string_value;
    } value;
} IndexKey;

typedef struct {
    ColumnId column;
    ConditionOp op;
    IndexKey lower;
    IndexKey upper;
    bool has_lower;
    bool has_upper;
    bool lower_inclusive;
    bool upper_inclusive;
} QueryCondition;

typedef struct {
    Record **items;
    int count;
    int capacity;
} QueryResult;

typedef struct {
    Record **items;
    int count;
    int capacity;
} RecordRefList;

typedef struct BPlusNode {
    int is_leaf;
    int num_keys;
    IndexKey keys[BPLUS_TREE_MAX_KEYS + 1];
    union {
        struct {
            struct BPlusNode *children[BPLUS_TREE_MAX_KEYS + 2];
        } internal;
        struct {
            RecordRefList *values[BPLUS_TREE_MAX_KEYS + 1];
            struct BPlusNode *next;
        } leaf;
    } ptrs;
} BPlusNode;

_Static_assert(sizeof(BPlusNode) <= BPLUS_TREE_NODE_SIZE_LIMIT,
               "B+ tree node exceeds 4KB; reduce BPLUS_TREE_ORDER");

typedef struct {
    BPlusNode *root;
    KeyType key_type;
    bool unique;
    int height;
    int key_count;
} BPlusTree;

typedef struct {
    char index_name[64];
    ColumnId column;
    KeyType key_type;
    bool unique;
    bool primary;
    bool active;
    BPlusTree *tree;
} IndexMetaRuntime;

typedef struct {
    ColumnMeta columns[MAX_COLUMNS];
    int column_count;
    ColumnId primary_column;
} Schema;

typedef struct {
    Record **rows;
    int count;
    int capacity;
    int next_auto_increment_id;
} Table;

typedef struct {
    Schema schema;
    Table table;
    IndexMetaRuntime indexes[MAX_INDEXES];
    int index_count;
    bool persist;
} Database;

typedef struct {
    char text[256];
    int start;
    int length;
} ListItem;

typedef enum {
    EXEC_OK = 0,
    EXEC_EXIT = 1,
    EXEC_ERROR = 2
} ExecStatus;

/* util.c */
double now_sec(void);
char *xstrdup(const char *s);
void safe_copy(char *dst, size_t dst_size, const char *src);
void str_trim_inplace(char *s);
int ci_char(int c);
int str_casecmp_local(const char *a, const char *b);
bool starts_with_ci(const char *s, const char *prefix);
const char *find_ci(const char *haystack, const char *needle);
void print_error_timed(const char *kind, const char *message, const char *sql,
                       int start, int length, double begin);
ColumnId column_from_name(const char *name);
const char *column_name(ColumnId col);
KeyType column_key_type(ColumnId col);
const char *type_name(ColumnType t);
ColumnType column_type(ColumnId col);
IndexKey make_int_key(int v);
IndexKey make_string_key_borrowed(const char *s);
bool key_clone(IndexKey src, IndexKey *out);
void key_free(IndexKey *k);
int key_compare(IndexKey a, IndexKey b);
bool key_to_string(IndexKey k, char *buf, size_t size);

/* bptree.c */
BPlusTree *bplus_tree_create(KeyType type, bool unique);
void bplus_tree_free(BPlusTree *tree);
int leaf_lower_bound(BPlusNode *leaf, IndexKey key);
bool bplus_tree_insert(BPlusTree *tree, IndexKey key, Record *record, bool *duplicate);
RecordRefList *bplus_tree_search(BPlusTree *tree, IndexKey key);
BPlusNode *bplus_tree_leftmost_leaf(BPlusTree *tree);
BPlusNode *bplus_tree_find_leaf(BPlusTree *tree, IndexKey key);

/* storage.c */
void table_init(Table *table);
void table_clear(Table *table);
bool table_add_record(Table *table, Record *record);
void schema_set_default(Schema *schema);
void free_indexes(Database *db);
void db_init(Database *db);
void db_clear(Database *db);
bool save_schema(Database *db);
bool load_schema(Database *db, const char *path, char *err, size_t err_size);
bool save_meta(Database *db);
bool save_data(Database *db);
bool save_indexes(Database *db);
IndexMetaRuntime *find_index_by_name(Database *db, const char *name);
IndexMetaRuntime *find_index_by_column(Database *db, ColumnId col);
IndexKey key_from_record(ColumnId col, Record *r);
bool index_insert_record(IndexMetaRuntime *idx, Record *r, char *err, size_t err_size);
bool rebuild_one_index(Database *db, IndexMetaRuntime *idx, char *err, size_t err_size);
bool rebuild_indexes(Database *db, char *err, size_t err_size);
bool add_index_meta(Database *db, const char *index_name, ColumnId col,
                    bool unique, bool primary, bool active);
bool load_data(Database *db, const char *path, bool replace, char *err, size_t err_size);
bool db_save_all(Database *db);
bool db_load_startup(Database *db, char *err, size_t err_size, double begin);

/* query.c */
bool parse_int_value(const char *s, int *out);
bool unquote_value(const char *src, char *dst, size_t dst_size);
int split_list_items(const char *sql, int offset, int length, ListItem *items, int max_items);
const char *find_matching_paren(const char *open);
void print_record_table(Record **rows, int count, double elapsed, const char *access, const char *index_name);
void query_condition_free(QueryCondition *cond);
bool record_matches_condition(Record *r, const QueryCondition *cond);
bool query_result_init(QueryResult *result, int initial_capacity);
void query_result_free(QueryResult *result);
bool query_result_add(QueryResult *result, Record *record);
bool bplus_tree_collect_range(BPlusTree *tree, const QueryCondition *cond, QueryResult *result);
bool parse_condition(const char *sql, const char *where, QueryCondition *cond,
                     int *err_start, int *err_len,
                     char *err_kind, size_t err_kind_size,
                     char *err_msg, size_t err_msg_size);

/* executor.c */
ExecStatus execute_save(Database *db, double begin);
ExecStatus execute_command(Database *db, const char *input);

#endif
