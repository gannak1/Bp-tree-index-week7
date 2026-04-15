#ifndef SQL_PROCESSOR_H
#define SQL_PROCESSOR_H

#include <stdio.h>

/*
 * 이 프로젝트에서 공통으로 사용하는 고정 상수들이다.
 * 과제 범위를 벗어나지 않도록 컬럼 수, 경로 길이, SQL 길이, row 버퍼 크기를
 * 적당한 상한선으로 제한해서 메모리 관리와 예외 처리를 단순하게 만든다.
 */
#define MAX_COLUMNS 16
#define MAX_NAME_LEN 32
#define MAX_PATH_LEN 260
#define MAX_SQL_TEXT 4096
#define MAX_ROW_SIZE 1024
#define DEFAULT_SCHEMA_NAME "school"

/*
 * 메모리 기반 B+ 트리의 차수 설정이다.
 * - BPTREE_ORDER: 노드 배열을 얼마나 크게 잡을지 나타낸다.
 * - BPTREE_MAX_KEYS: 분할 전 정상 상태에서 한 노드가 가질 수 있는 최대 key 수다.
 *
 * 배열 크기를 BPTREE_ORDER 기준으로 한 칸 더 크게 잡아두고,
 * 삽입 직후 잠깐 overflow 상태가 된 뒤 split하는 흐름을 구현한다.
 */
#define BPTREE_ORDER 32
#define BPTREE_MAX_KEYS (BPTREE_ORDER - 1)

/*
 * Lexer가 SQL 문자열을 잘라 만든 토큰 종류다.
 * 같은 문자열이라도 문맥에 따라 schema / table / column 역할이 달라지므로,
 * lexer 단계에서는 일단 문법적 종류만 분류하고 실제 의미 해석은 parser가 담당한다.
 */
typedef enum {
    TOKEN_IDENTIFIER,      /* 일반 이름: users, age, school, verify */
    TOKEN_STRING,          /* 문자열 literal: 'Kim' */
    TOKEN_NUMBER,          /* 숫자 literal: 10, -3, 123 */
    TOKEN_STAR,            /* '*' */
    TOKEN_COMMA,           /* ',' */
    TOKEN_DOT,             /* '.' */
    TOKEN_LPAREN,          /* '(' */
    TOKEN_RPAREN,          /* ')' */
    TOKEN_EQUAL,           /* '=' */
    TOKEN_NOT_EQUAL,       /* '!=' */
    TOKEN_GREATER,         /* '>' */
    TOKEN_GREATER_EQUAL,   /* '>=' */
    TOKEN_LESS,            /* '<' */
    TOKEN_LESS_EQUAL,      /* '<=' */
    TOKEN_SEMICOLON,       /* ';' */
    TOKEN_EOF,             /* 입력 끝 표시용 가상 토큰 */
    TOKEN_KEYWORD_INSERT,  /* INSERT 예약어 */
    TOKEN_KEYWORD_INTO,    /* INTO 예약어 */
    TOKEN_KEYWORD_VALUES,  /* VALUES 예약어 */
    TOKEN_KEYWORD_SELECT,  /* SELECT 예약어 */
    TOKEN_KEYWORD_FROM,    /* FROM 예약어 */
    TOKEN_KEYWORD_WHERE,   /* WHERE 예약어 */
    TOKEN_KEYWORD_BETWEEN, /* BETWEEN 예약어 */
    TOKEN_KEYWORD_AND      /* BETWEEN low AND high 의 AND 예약어 */
} TokenType;

/*
 * 토큰 하나를 표현하는 구조체다.
 * - type: 토큰의 문법적 종류
 * - text: 원본 SQL에서 잘라낸 문자열
 *
 * 예:
 *   SELECT * FROM users;
 * 에서 "users"는 TOKEN_IDENTIFIER, text="users" 로 저장된다.
 */
typedef struct {
    TokenType type;
    char *text;
} Token;

/*
 * lexer가 동적으로 만들어 반환하는 토큰 배열이다.
 * - items: heap에 할당된 Token 배열
 * - count: 실제 토큰 개수
 */
typedef struct {
    Token *items;
    int count;
} TokenArray;

/*
 * AST 노드가 어떤 종류의 literal 값을 담는지 표현한다.
 * 구조 노드(TABLE, WHERE 등)는 AST_VALUE_NONE을 사용하고,
 * 실제 값 노드만 STRING 또는 NUMBER를 사용한다.
 */
typedef enum {
    AST_VALUE_NONE,    /* 값이 없는 구조 노드 */
    AST_VALUE_STRING,  /* 문자열 literal */
    AST_VALUE_NUMBER   /* 숫자 literal */
} ASTValueType;

/*
 * 노드 기반 AST에서 사용하는 노드 종류다.
 * parser는 SQL 문장을 아래 노드들의 조합으로 바꿔서 executor에 넘긴다.
 */
typedef enum {
    NODE_SELECT,       /* SELECT 문 전체를 대표하는 루트 노드 */
    NODE_INSERT,       /* INSERT 문 전체를 대표하는 루트 노드 */
    NODE_TABLE,        /* 대상 schema/table 정보를 묶는 노드 */
    NODE_COLUMN,       /* 컬럼 이름 하나를 담는 노드 */
    NODE_COLUMN_LIST,  /* SELECT 컬럼 목록을 담는 부모 노드 */
    NODE_VALUE,        /* literal 값 하나를 담는 노드 */
    NODE_VALUE_LIST,   /* INSERT VALUES 목록을 담는 부모 노드 */
    NODE_WHERE,        /* WHERE 절 전체를 담는 노드 */
    NODE_IDENTIFIER,   /* schema 이름이나 table 이름을 담는 식별자 노드 */
    NODE_OPERATOR,     /* WHERE 비교 연산자 노드: =, !=, >, >=, <, <= */
    NODE_BETWEEN       /* BETWEEN lower / upper bound를 담는 노드 */
} NodeType;

/*
 * 노드 기반 AST의 기본 구조체다.
 * - type: 노드 종류
 * - value_type: literal 값의 종류
 * - text: 컬럼명, 테이블명, literal 값, 연산자 문자열 등
 * - first_child: 첫 번째 자식 노드
 * - next_sibling: 같은 부모 아래 다음 형제 노드
 *
 * 이 구조를 이용해:
 *   SELECT -> COLUMN_LIST -> COLUMN ...
 *   SELECT -> TABLE -> IDENTIFIER ...
 * 같은 트리를 만든다.
 */
typedef struct ASTNode {
    NodeType type;
    ASTValueType value_type;
    char *text;
    struct ASTNode *first_child;
    struct ASTNode *next_sibling;
} ASTNode;

/*
 * 메타 CSV에서 읽은 컬럼 타입이다.
 * 이번 프로젝트는 INT와 CHAR만 다룬다.
 */
typedef enum {
    COL_INT,   /* 4바이트 정수 컬럼 */
    COL_CHAR   /* 고정 길이 문자열 컬럼 */
} ColumnType;

/*
 * 컬럼 하나의 메타정보다.
 * - name: 컬럼 이름
 * - type: 컬럼 타입
 * - size: row 안에서 차지하는 바이트 수
 * - offset: row 시작 기준 이 컬럼이 시작하는 위치
 *
 * storage 계층은 offset과 size를 이용해 바이너리 row를 해석한다.
 */
typedef struct {
    char name[MAX_NAME_LEN];
    ColumnType type;
    int size;
    int offset;
} ColumnDef;

/*
 * 특정 schema.table의 메타정보 전체다.
 * - schema_name, table_name: 현재 다루는 테이블 이름
 * - columns: 컬럼 메타 배열
 * - column_count: 실제 컬럼 수
 * - row_size: 바이너리 row 전체 크기
 * - data_file_path: .dat 파일 경로
 * - meta_file_path: .schema.csv 파일 경로
 */
typedef struct {
    char schema_name[MAX_NAME_LEN];
    char table_name[MAX_NAME_LEN];
    ColumnDef columns[MAX_COLUMNS];
    int column_count;
    int row_size;
    char data_file_path[MAX_PATH_LEN];
    char meta_file_path[MAX_PATH_LEN];
} TableMeta;

/*
 * 함수 실행 성공 여부와 사용자에게 보여줄 메시지를 함께 전달하는 구조체다.
 * - ok: 성공이면 1, 실패이면 0
 * - message: 사람이 읽을 수 있는 오류 메시지
 */
typedef struct {
    int ok;
    char message[256];
} Status;

/*
 * 마지막 SQL이 어떤 경로로 실행됐는지 나타낸다.
 * 성능 로그나 디버깅 메시지에서 "인덱스 사용" / "풀스캔"을 구분할 때 쓴다.
 */
typedef enum {
    EXECUTION_PATH_UNKNOWN,   /* 아직 실행 경로를 결정하지 못한 상태 */
    EXECUTION_PATH_INSERT,    /* INSERT 실행 경로 */
    EXECUTION_PATH_INDEXED,   /* B+ 트리 인덱스를 사용한 SELECT */
    EXECUTION_PATH_FULL_SCAN  /* 바이너리 파일 전체를 선형 탐색한 SELECT */
} ExecutionPath;

/*
 * B+ 트리의 실제 노드 구조다.
 * - is_leaf: 리프 노드인지 여부
 * - key_count: 현재 key 개수
 * - keys: 정렬된 key 배열
 * - offsets: leaf 노드에서 key와 매칭되는 row offset
 * - children: internal 노드에서 자식 포인터
 * - next: leaf 노드끼리의 연결 포인터
 *
 * 이번 구현은 id 전용 인덱스라서 key는 int, value는 row offset(long)이다.
 */
typedef struct BPTreeNode {
    int is_leaf;
    int key_count;
    int keys[BPTREE_ORDER];
    long offsets[BPTREE_ORDER];
    struct BPTreeNode *children[BPTREE_ORDER + 1];
    struct BPTreeNode *next;
} BPTreeNode;

/*
 * 현재 테이블의 id 인덱스를 담는 B+ 트리 핸들이다.
 * root만 알면 하위 노드를 따라가며 탐색/삽입할 수 있다.
 */
typedef struct {
    BPTreeNode *root;
} BPTree;

/*
 * 현재 실행 중인 테이블 상태를 메모리에 들고 있는 컨텍스트다.
 * - meta: 현재 테이블 메타정보
 * - id_index: id -> row_offset B+ 트리
 * - record_count: 현재 data 파일 row 수
 * - is_loaded: 컨텍스트가 유효하게 로드되었는지 여부
 * - last_execution_path: 마지막 SQL이 실제로 사용한 실행 경로
 */
typedef struct {
    TableMeta meta;
    BPTree id_index;
    int record_count;
    int is_loaded;
    ExecutionPath last_execution_path;
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

/* Lexer / Parser 진입점 */
TokenArray lex_sql(const char *sql, Status *status);
void free_tokens(TokenArray *tokens);
int parse_statement(const TokenArray *tokens, ASTNode **root, Status *status);

/* B+ 트리 API */
void bptree_init(BPTree *tree);
void bptree_free(BPTree *tree);
int bptree_insert(BPTree *tree, int key, long offset, Status *status);
int bptree_search(const BPTree *tree, int key, long *offset);

/* 메타 / 저장 / 실행 계층 API */
int load_table_meta(const char *schema_name, const char *table_name, TableMeta *meta, Status *status);
int ensure_parent_directory(const char *file_path, Status *status);
int build_id_index_from_data(ExecutionContext *context, Status *status);
int append_binary_row(ExecutionContext *context, ASTNode *root, int *inserted_id, Status *status);
int execute_select(ExecutionContext *context, ASTNode *root, Status *status);
int prepare_execution_context_for_table(const char *schema_name, const char *table_name, Status *status);
int prepare_execution_context(ASTNode *root, Status *status);
int execute_statement(ASTNode *root, Status *status);
ExecutionPath get_last_execution_path(void);
const char *execution_path_to_text(ExecutionPath path);

#endif
