#ifndef SQL_PROCESSOR_H
#define SQL_PROCESSOR_H

#include <stdio.h>

/*
 * 프로젝트 전체에서 사용하는 고정 제한값들이다.
 * 과제를 단순하고 예측 가능하게 유지하기 위해 컬럼 수, 경로 길이,
 * SQL 입력 길이, 바이너리 row 최대 크기를 상수로 제한한다.
 */
#define MAX_COLUMNS 16
#define MAX_NAME_LEN 32
#define MAX_PATH_LEN 260
#define MAX_SQL_TEXT 4096
#define MAX_ROW_SIZE 1024
#define DEFAULT_SCHEMA_NAME "school"

/*
 * B+ 트리 차수 설정이다.
 * BPTREE_MAX_KEYS는 노드가 정상 상태에서 가질 수 있는 최대 key 수고,
 * 배열은 overflow 후 split을 처리할 수 있도록 한 칸 더 크게 잡는다.
 */
#define BPTREE_ORDER 32
#define BPTREE_MAX_KEYS (BPTREE_ORDER - 1)

/* Lexer가 원본 SQL 문자열을 잘라서 만드는 토큰 종류들이다. */
typedef enum {
    TOKEN_IDENTIFIER,        /* 일반 이름: users, age, school */
    TOKEN_STRING,            /* 문자열 literal: 'Kim' */
    TOKEN_NUMBER,            /* 숫자 literal: 10, 123 */
    TOKEN_STAR,              /* '*' */
    TOKEN_COMMA,             /* ',' */
    TOKEN_DOT,               /* '.' */
    TOKEN_LPAREN,            /* '(' */
    TOKEN_RPAREN,            /* ')' */
    TOKEN_EQUAL,             /* '=' */
    TOKEN_NOT_EQUAL,         /* '!=' */
    TOKEN_GREATER,           /* '>' */
    TOKEN_GREATER_EQUAL,     /* '>=' */
    TOKEN_LESS,              /* '<' */
    TOKEN_LESS_EQUAL,        /* '<=' */
    TOKEN_SEMICOLON,         /* ';' */
    TOKEN_EOF,               /* 입력 끝 */
    TOKEN_KEYWORD_INSERT,    /* INSERT 키워드 */
    TOKEN_KEYWORD_INTO,      /* INTO 키워드 */
    TOKEN_KEYWORD_VALUES,    /* VALUES 키워드 */
    TOKEN_KEYWORD_SELECT,    /* SELECT 키워드 */
    TOKEN_KEYWORD_FROM,      /* FROM 키워드 */
    TOKEN_KEYWORD_WHERE,     /* WHERE 키워드 */
    TOKEN_KEYWORD_BETWEEN,   /* BETWEEN 키워드 */
    TOKEN_KEYWORD_AND        /* BETWEEN 구문의 AND 키워드 */
} TokenType;

/* SQL을 자른 가장 작은 단위 하나다. type은 종류, text는 실제 문자열이다. */
typedef struct {
    TokenType type;
    char *text;
} Token;

/* Lexer가 반환하는 동적 토큰 배열이다. */
typedef struct {
    Token *items;
    int count;
} TokenArray;

/* AST 값 노드가 어떤 종류의 literal인지 표시한다. */
typedef enum {
    AST_VALUE_NONE,    /* 값이 없는 노드: SELECT, TABLE, WHERE 같은 구조 노드 */
    AST_VALUE_STRING,  /* 문자열 literal 값: 'Kim' */
    AST_VALUE_NUMBER   /* 숫자 literal 값: 10, 123 */
} ASTValueType;

/* 노드 기반 AST에서 사용하는 노드 종류다. */
typedef enum {
    NODE_SELECT,       /* SELECT 문 전체를 나타내는 루트 노드 */
    NODE_INSERT,       /* INSERT 문 전체를 나타내는 루트 노드 */
    NODE_TABLE,        /* 대상 schema/table 정보를 담는 노드 */
    NODE_COLUMN,       /* 컬럼 이름 하나를 담는 노드 */
    NODE_COLUMN_LIST,  /* SELECT 컬럼 목록을 담는 부모 노드 */
    NODE_VALUE,        /* literal 값 하나를 담는 노드 */
    NODE_VALUE_LIST,   /* INSERT VALUES 목록을 담는 부모 노드 */
    NODE_WHERE,        /* WHERE 절 전체를 담는 노드 */
    NODE_IDENTIFIER,   /* schema 이름이나 table 이름 같은 식별자 노드 */
    NODE_OPERATOR,     /* WHERE 비교 연산자 노드: =, !=, >, >=, <, <= */
    NODE_BETWEEN       /* BETWEEN 절의 lower/upper bound를 담는 노드 */
} NodeType;

/*
 * 노드 기반 AST의 기본 구조체다.
 * text에는 컬럼명, 테이블명, 실제 literal 값, 연산자 문자열 등이 들어간다.
 * first_child는 첫 자식 노드, next_sibling은 같은 부모를 가진 다음 노드다.
 */
typedef struct ASTNode {
    NodeType type;
    ASTValueType value_type;
    char *text;
    struct ASTNode *first_child;
    struct ASTNode *next_sibling;
} ASTNode;

/* 메타 CSV에서 읽어온 컬럼 저장 타입이다. */
typedef enum {
    COL_INT,   /* 4바이트 정수 컬럼 */
    COL_CHAR   /* 고정 길이 문자 컬럼 */
} ColumnType;

/*
 * 컬럼 하나의 메타정보다.
 * 바이너리 row를 해석하거나 쓸 때 각 컬럼의 이름, 타입, 크기, 시작 위치를 알려준다.
 */
typedef struct {
    char name[MAX_NAME_LEN];  /* 컬럼 이름: id, name, age */
    ColumnType type;          /* 컬럼 저장 타입: INT 또는 CHAR */
    int size;                 /* 이 컬럼이 row 안에서 차지하는 바이트 수 */
    int offset;               /* row 시작점 기준 이 컬럼이 시작하는 바이트 위치 */
} ColumnDef;

/*
 * 특정 schema.table 전체의 메타정보를 메모리에 올린 구조체다.
 * 메타 CSV를 읽은 뒤 실행기와 저장소 계층이 이 구조를 기준으로 동작한다.
 */
typedef struct {
    char schema_name[MAX_NAME_LEN];     /* 스키마 이름: school */
    char table_name[MAX_NAME_LEN];      /* 테이블 이름: users */
    ColumnDef columns[MAX_COLUMNS];     /* 이 테이블이 가진 컬럼 목록 */
    int column_count;                   /* 실제로 로드된 컬럼 개수 */
    int row_size;                       /* row 하나 전체 크기 = 모든 컬럼 size의 합 */
    char data_file_path[MAX_PATH_LEN];  /* 실제 바이너리 데이터 파일 경로 */
    char meta_file_path[MAX_PATH_LEN];  /* 메타 CSV 파일 경로 */
} TableMeta;

/* 함수 성공 여부와 사용자에게 보여줄 메시지를 함께 전달한다. */
typedef struct {
    int ok;
    char message[256];
} Status;

/* B+ 트리의 노드 하나다. leaf는 key->offset을, internal은 key->child를 가진다. */
typedef struct BPTreeNode {
    int is_leaf;                                 /* 1이면 leaf node, 0이면 internal node */
    int key_count;                               /* 현재 노드에 들어 있는 key 수 */
    int keys[BPTREE_ORDER];                      /* overflow split 전까지 담을 key 배열 */
    long offsets[BPTREE_ORDER];                  /* leaf node에서 key와 매핑되는 row offset */
    struct BPTreeNode *children[BPTREE_ORDER + 1]; /* internal node에서 자식 포인터 */
    struct BPTreeNode *next;                     /* leaf node끼리 순서대로 연결하는 포인터 */
} BPTreeNode;

/* 현재 테이블의 id 인덱스를 메모리에 유지하는 B+ 트리 구조다. */
typedef struct {
    BPTreeNode *root;  /* 트리의 루트 노드 */
} BPTree;

/*
 * 실행 중 현재 테이블 상태를 함께 들고 있는 컨텍스트다.
 * 메타, 현재 record 수, 그리고 id 인덱스를 한 곳에 모아 둔다.
 */
typedef struct {
    TableMeta meta;       /* 현재 로드된 테이블 메타 */
    BPTree id_index;      /* id -> row_offset B+ 트리 인덱스 */
    int record_count;     /* 현재 data 파일 기준 row 수 */
    int is_loaded;        /* 컨텍스트가 유효하게 초기화됐는지 여부 */
} ExecutionContext;

/* 문자열 유틸 함수들 */
char *sp_strdup(const char *text);
char *trim_whitespace(char *text);
void strip_quotes(char *text);
int equals_ignore_case(const char *left, const char *right);

/* AST 유틸 함수들 */
ASTNode *create_ast_node(NodeType type, const char *text, ASTValueType value_type);
void append_child(ASTNode *parent, ASTNode *child);
ASTNode *find_child(ASTNode *parent, NodeType type);
void free_ast(ASTNode *node);

/* Lexer / Parser */
TokenArray lex_sql(const char *sql, Status *status);
void free_tokens(TokenArray *tokens);
int parse_statement(const TokenArray *tokens, ASTNode **root, Status *status);

/* B+ 트리 */
void bptree_init(BPTree *tree);
void bptree_free(BPTree *tree);
int bptree_insert(BPTree *tree, int key, long offset, Status *status);
int bptree_search(const BPTree *tree, int key, long *offset);

/* Meta / Storage / Execution */
int load_table_meta(const char *schema_name, const char *table_name, TableMeta *meta, Status *status);
int ensure_parent_directory(const char *file_path, Status *status);
int build_id_index_from_data(ExecutionContext *context, Status *status);
int append_binary_row(ExecutionContext *context, ASTNode *root, int *inserted_id, Status *status);
int execute_select(ExecutionContext *context, ASTNode *root, Status *status);
int execute_statement(ASTNode *root, Status *status);

#endif
