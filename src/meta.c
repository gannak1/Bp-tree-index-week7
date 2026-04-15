#include "sql_processor.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/*
 * 운영체제별 디렉터리 생성 API와 경로 구분자를 맞춘다.
 * - Windows: _mkdir, '\'
 * - Linux/macOS: mkdir, '/'
 *
 * 도커 / dev-container 안에서도 같은 소스를 그대로 빌드하기 위해 분기한다.
 */
#ifdef _WIN32
#include <direct.h>
#define SP_MKDIR(path) _mkdir(path)
#define SP_PATH_SEP '\\'
#define SP_PATH_SEP_STR "\\"
#else
#include <sys/stat.h>
#include <sys/types.h>
#define SP_MKDIR(path) mkdir(path, 0775)
#define SP_PATH_SEP '/'
#define SP_PATH_SEP_STR "/"
#endif

/*
 * 메타 CSV의 타입 문자열을 내부 ColumnType enum으로 변환한다.
 *
 * 예:
 * - "INT"  -> COL_INT
 * - "CHAR" -> COL_CHAR
 */
static int parse_column_type(const char *text, ColumnType *type, Status *status) {
    if (equals_ignore_case(text, "INT")) {
        *type = COL_INT;
        return 1;
    }
    if (equals_ignore_case(text, "CHAR")) {
        *type = COL_CHAR;
        return 1;
    }

    snprintf(status->message, sizeof(status->message), "Schema error: unsupported type '%s'", text);
    return 0;
}

/*
 * CSV 한 줄을 쉼표 기준으로 제자리에서 분리한다.
 *
 * 입력:
 * - line: "id,INT,4" 같은 수정 가능한 문자열
 * - parts: 분리 결과를 담을 포인터 배열
 * - expected_parts: 기대하는 조각 수
 *
 * 반환:
 * - 실제 분리된 조각 수
 *
 * 동작:
 * - 쉼표를 '\0'으로 바꿔서 원본 문자열 안에 조각 문자열을 만든다.
 */
static int parse_csv_line(char *line, char **parts, int expected_parts) {
    int count = 0;      /* 지금까지 채운 조각 수 */
    char *cursor = line;/* 현재 탐색 중인 위치 */

    while (count < expected_parts) {
        parts[count++] = cursor;
        cursor = strchr(cursor, ',');
        if (cursor == NULL) {
            break;
        }
        *cursor = '\0';
        cursor++;
    }

    return count;
}

/*
 * meta/<schema>/<table>.schema.csv 파일을 읽어 TableMeta를 채운다.
 *
 * 입력:
 * - schema_name, table_name: 찾고 싶은 테이블 이름
 * - meta: 결과를 채울 구조체
 *
 * 결과:
 * - meta 안의 columns, row_size, 파일 경로가 모두 채워진다.
 *
 * 어디에 쓰나:
 * - executor가 SQL 실행 전에 어떤 컬럼 구조인지 알기 위해 호출한다.
 */
int load_table_meta(const char *schema_name, const char *table_name, TableMeta *meta, Status *status) {
    FILE *file;              /* 메타 CSV 파일 핸들 */
    char line[256];          /* CSV 한 줄 버퍼 */
    int column_count = 0;    /* 실제로 읽은 컬럼 수 */
    int offset = 0;          /* 현재까지 누적된 row 바이트 크기 */

    memset(meta, 0, sizeof(*meta));
    snprintf(meta->schema_name, sizeof(meta->schema_name), "%s", schema_name);
    snprintf(meta->table_name, sizeof(meta->table_name), "%s", table_name);
    snprintf(
        meta->meta_file_path,
        sizeof(meta->meta_file_path),
        "meta" SP_PATH_SEP_STR "%s" SP_PATH_SEP_STR "%s.schema.csv",
        schema_name,
        table_name
    );
    snprintf(
        meta->data_file_path,
        sizeof(meta->data_file_path),
        "data" SP_PATH_SEP_STR "%s" SP_PATH_SEP_STR "%s.dat",
        schema_name,
        table_name
    );

    file = fopen(meta->meta_file_path, "r");
    if (file == NULL) {
        snprintf(status->message, sizeof(status->message), "Table error: table '%s.%s' not found", schema_name, table_name);
        return 0;
    }

    /*
     * 첫 줄은 header(column_name,type,size)라고 가정하고 읽어 버린다.
     * 파일이 비어 있으면 메타 파일 자체가 잘못된 것으로 본다.
     */
    if (fgets(line, sizeof(line), file) == NULL) {
        fclose(file);
        snprintf(status->message, sizeof(status->message), "Schema error: empty meta file '%s'", meta->meta_file_path);
        return 0;
    }

    while (fgets(line, sizeof(line), file) != NULL) {
        char *parts[3];       /* column_name, type, size 세 조각 */
        int part_count;       /* 실제 분리된 조각 수 */
        char *name;           /* 컬럼 이름 문자열 */
        char *type_text;      /* 타입 문자열 */
        char *size_text;      /* 크기 문자열 */
        ColumnDef *column;    /* 현재 채울 컬럼 구조체 */

        if (column_count >= MAX_COLUMNS) {
            fclose(file);
            snprintf(status->message, sizeof(status->message), "Schema error: too many columns in '%s'", meta->meta_file_path);
            return 0;
        }

        line[strcspn(line, "\r\n")] = '\0';
        if (line[0] == '\0') {
            continue;
        }

        part_count = parse_csv_line(line, parts, 3);
        if (part_count != 3) {
            fclose(file);
            snprintf(status->message, sizeof(status->message), "Schema error: invalid meta row in '%s'", meta->meta_file_path);
            return 0;
        }

        name = trim_whitespace(parts[0]);
        type_text = trim_whitespace(parts[1]);
        size_text = trim_whitespace(parts[2]);

        column = &meta->columns[column_count];
        snprintf(column->name, sizeof(column->name), "%s", name);
        if (!parse_column_type(type_text, &column->type, status)) {
            fclose(file);
            return 0;
        }
        column->size = atoi(size_text);
        if (column->size <= 0) {
            fclose(file);
            snprintf(status->message, sizeof(status->message), "Schema error: invalid size '%s'", size_text);
            return 0;
        }

        /*
         * offset은 이전 컬럼들의 size를 모두 더한 위치다.
         * storage 계층은 이 offset을 이용해 row 버퍼에서 해당 컬럼을 찾는다.
         */
        column->offset = offset;
        offset += column->size;
        column_count++;
    }

    fclose(file);

    if (column_count == 0) {
        snprintf(status->message, sizeof(status->message), "Schema error: no columns in '%s'", meta->meta_file_path);
        return 0;
    }
    if (offset > MAX_ROW_SIZE) {
        snprintf(status->message, sizeof(status->message), "Schema error: row size too large");
        return 0;
    }

    meta->column_count = column_count;
    meta->row_size = offset;
    return 1;
}

/*
 * data 파일을 쓰기 전에 부모 디렉터리 경로를 생성한다.
 *
 * 예:
 * - data/school/users.dat 를 쓰기 전
 * - data, data/school 디렉터리가 없으면 순서대로 만든다.
 *
 * 입력:
 * - file_path: 최종 파일 경로
 *
 * 내부 변수:
 * - path: file_path를 복사해 수정 가능한 버퍼로 만든 것
 * - current: 현재까지 누적된 디렉터리 경로
 * - segment: path를 디렉터리 단위로 잘라가며 읽는 포인터
 * - next: 다음 경로 구분자 위치
 */
int ensure_parent_directory(const char *file_path, Status *status) {
    char path[MAX_PATH_LEN];
    char *slash;
    char current[MAX_PATH_LEN] = {0};
    char *segment;
    char *next;

    snprintf(path, sizeof(path), "%s", file_path);
    slash = strrchr(path, SP_PATH_SEP);
    if (slash == NULL) {
        return 1;
    }
    *slash = '\0';

    segment = path;
    while (*segment != '\0') {
        next = strchr(segment, SP_PATH_SEP);
        if (next != NULL) {
            *next = '\0';
        }

        if (current[0] != '\0') {
            strncat(current, SP_PATH_SEP_STR, sizeof(current) - strlen(current) - 1);
        }
        strncat(current, segment, sizeof(current) - strlen(current) - 1);

        if (SP_MKDIR(current) != 0 && errno != EEXIST) {
            snprintf(status->message, sizeof(status->message), "Execution error: cannot create directory '%s'", current);
            return 0;
        }

        if (next == NULL) {
            break;
        }
        *next = SP_PATH_SEP;
        segment = next + 1;
    }

    return 1;
}
