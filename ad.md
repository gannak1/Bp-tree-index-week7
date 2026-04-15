ASTNode
 * 노드 기반 AST의 기본 구조체다.
 * text에는 컬럼명, 테이블명, 실제 literal 값, 연산자 문자열 등이 들어간다.
 * first_child는 첫 자식 노드, next_sibling은 같은 부모를 가진 다음 노드다.

runsql

lex_sql로 각 글자를 토큰화

parse_statement로 적절한 파서로 보내기

execute_statement로 명령어 실행

lex_sql
if가 총 4개로 구성되며 해당 if마다 true면 토큰을 만들어서 배열에 넣고 아니면 free를 하여 다시 빈 배열을 리턴함
push_token 이라는 걸로 token_array에 token을 할당하는데 *,.()=><; =< => != 이면 각각 맞는 토큰 타입으로 토큰 배열에 들어가게 됨, 이때 토큰 배열은 realloc으로 되어서 크기가 변하더라도 동적으로 대응이 가능하도록 설계 되어 있음
만약 '을 만난다면 해당 인덱스부터 천천히 순회하여 \0 또는 \'이 나올 때 까지 순회, 만약 \0이면 중지하고 malloc을 해제하여 빈 배열을 리턴함 
string으로 인식하여 토큰 타입을 스트링 타입으로 만든 다음 토큰 배열에 넣어짐
만약 숫자면 위의 스트링과 마찬가지로 처음부터 숫자가 안나올때 까지 모두 검사하고 토큰에 넣음, 처음에 +, -를 인식하여 숫자의 음수인지 양수인지 확인
아무것도 아닌 채로 글자, _만 적혀져 있으면 테이블, 스키마 등으로 인식하면 TOKEN_IDENTIFIER로, keyword_type로 예약어인지 확인하여 적절한 타입으로 token array에 넣어짐
오류 없이 끝나면 token_eof 타입을 넣고 배열이 리턴



parse_statement
Parse struct로 parse가 선언되어 있는데 token의 각 array를 index로 확인하는 용도, index는 현재 index, tokens는 토큰 모음
토큰이 아무것도 없으면 리턴하여 에러 메세지 출력
current_token으로 현재 토큰이 뭔지 확인, &parse->tokens->items[parse->index]
첫번째 TOKEN이 TOKEN_KEYWORD_INSERT면 parse_insert 실행
첫번째 TOKEN이 TOKEN_KEYWORD_SELECT면 parse_select 실행
이것도 아니면 에러 리턴
루트 없으면 에러 리턴

루트 구조
(select) -> asdas
(insert) -> table_node ->



parse_insert
expect 함수로 함수가 해당하는 타입이 맞는지 검사, 검사를 하고 맞으면 index가 1씩 올라가는 구조 아니면 메세지와 0을 리턴
parse의 타입이 TOKEN_KEYWORD_INSERT -> TOKEN_KEYWORD_INTO (select, into)를 검사하고 테이블 노드를 만듬
그다음 TOKEN_KEWORD_VALUES -> TOKEN_LPAREN (value, ( )
parse_value_list_node으로 value_list_node를 만들고
TOKEN_RPAREN -> TOKEN_SEMICOLON ( ), ;) 타입이 맞는지 검사
마지막으로 TOKEN_EOF인지 검사하고 맞으면 다음으로 create_ast_node로 INSERT  노드를 root 변수로 생성, value_list_node, table_node를 각각 first_child, next_sibling으로 연결


parse_select
expect 함수로 함수가 해당하는 타입이 맞는지 검사, 검사를 하고 맞으면 index가 1씩 올라가는 구조 아니면 메세지와 0을 리턴
parse의 타입이 TOKEN_KEYWORD_SELECT (select) 검사하고 parse_column_list_node로 column_list_node를 만듬
TOKEN_KEYWORD_FROM (from)을  검사하고 table_node를 parse_table_node로 만듬
TOKEN_KEYWORD_WHERE (where)을 검사하고 있으면 parse_where_node로 where_node를 생성
match로 TOKEN_SEMICOLON인지 확인하고 ;이면 넘겨버림
마지막으로 TOKEN_EOF인지 검사하고 맞으면 다음으로 create_ast_node로 SELECT 노드를 root 변수로 생성, column_list_node, table_node, where_node를 각각 first_child, next_sibling으로 연결



parse_column_list_node
list_node----col1 first_child
               L---col2 next_sibling 
	       L---col3 next_sibling
select 노드의 컬럼을 담는 노드
먼저 list_node를 만듬, 이때 text는 빈 내용
토큰이 TOKEN_START(*)이면 text가 *인 column_node 노드를 만듬
column_node에 에러가 생겨서 NULL이면 리턴
list_node에 first_child를 column_node를 붙이고 list_node 리턴
그것 외 없으면
while문을 돌리게 됨
parse_identifier_text로 column_name을 얻고, 이름이 NULL이면 에러
얻은 이름으로 create_ast_node로 column_node를 만들고 에러 생기면 NULL 리턴하고 각각 list_node에 first_child, next_sibling에 적절하게 배치



parse_table_node
table_node----schema
               L---table
노드 구조 만들려고 함
이 SQL이 어떤 테이블을 대상으로 하는지 를 AST 안에 담는 역할입니다.
first_identifier로 첫번째 텍스트 값이 뭔지를 받고 만약 NULL이면 종료
그다음 table_node라는 것을 만들고 create_ast_node로 노드를 만듬
table_node가 NULL로 나오면 해제하고 메세지 출력
만약 parser가 TOKEN_DOT이면 table_text에 parse_identifier_text 할당
table_text가 NULL이면 에러 출력 
schema.table 와 같은 형태면 -> table 이름을 table_name_node에, schema를 schema_node에 넣음 create_ast_node로 각각 만들고 이전(first_child)를 schema, 형제(next_sibling)를 schema_node에 넣는다.
점이 없으면 schema_node에 기본 schema 이름(school)을 넣는다.
그리고 table_node를 리턴



parse_where_node
parse_identifier_text로 column 인지 판별 후 column_name에 할당, 또한 연산자도 parse_operator_text로 판별, 값도 parse_value_node로 판별
NULL이면 오류로 리턴
text가 NULL인 where_node, text가 column_name인 column_node, text가 operator_text 인 operator_node 생성
wehre_node에 column_node, operator_node, value_node를 각각 first_child, next_sibling으로 연결


create_ast_node
노드 하나 생성하는 함수, ASTNode 타입으로 node를 생성하고 type, text, value_type를 받아서 각각 값에 할당, text는 sp_strdup로 복사해서 할당, 메모리 오류로 malloc이 오류나거나 text가 NULL이 아니고 node의 텍스트가 null이면 NULL 리턴 node의 text가 null이면 NULL 반환, first_child, next_sibling을 NULL로 할당 후 node 리턴



parse_identifier_text
TOKEN_IDENTIFIER가 아니면 NULL 리턴, 맞으면 copy로 text를 모두 복사하고 parser의 index를 하나 더하고 copy를 리턴, 현재 테이블이 TOKEN_IDENTIFIER로 되어있음



sp_strdup
text를 copy로 새로 복사해서 리턴


parse_operator_text
현재 토큰이 비교 연산자면 해당 연산자 문자열을 반환한다., switch, case로 paser의 text로 구분



parse_value_list_node
list_node를 NODE_VALUE_LIST로 만듬
while문으로 계속해서 확인하면서 TOKEN_COMMA가 없을 때 까지 확인하면서 value_node를 parse_value_node로 할당
NULL이면 오류
그리고 list_node에 value_node를 first_child 또는 next_sibling에 추가
list_node 리턴



parse_value_node
node, value_type
현재 타입이 TOKEN_STRING, TOKEN_NUMBER가 아니면 NULL로 리턴
현재 TOKEN의 text를 가진 NODE_VALUE인 값으로 create_ast_node로 node를 생성
node에 오류 있으면 NULL과 오류메세지 리턴, 아니면 index를 높이고 node를 리턴


execute_statement
처음에 talbe_node를 찾는데 fine_child로 찾음
extract_table_names로 schema_name, table_name 찾기
load_table_meta로 메타 파일 열어서


extract_table_names
table_node의 first_child를 schema_node, schema_node의 next_sibling을 table_name_node로 리턴

load_table_meta
schema_name, table_name, meta_file_path, data_file_path 를 미리 받은 값으로 전부 적절히 할당
fopen으로 파일을 열어서 file에 할당,
그리고 fget으로 line을 한줄씩 읽는데, 첫줄은 오류 검사에만 쓰고 버림
2번쨰 줄부터 while 문을 이용하여 column의 형태를 csv로 받음, 이때 fget으로 line에 할당
  최대 column의 개수를 넘으면 에러
  \r, \n 일 경우 \0으로 바꿔서 정지
  \0밖에 없으면 다음으로
  parse_csv_line으로 쉼표를 분리
  part_count의 개수가 3개가 아니면 오류로 리턴
  part 0~2 인덱스까지 양 옆 whilespace를 trim_whitespace로 제거하고 name, type_text, size_text로 할당
  column을 TableMeta 타입의 meta의 columns의 배열에 할당
  column의 name을 name으로 할당
  텍스트 타입이 하려고 했던 타입인 COL_INT와 COL_CHAR 인지 확인하고 아니면 에러를 리턴
  column의 size를 size_text의 정수 값으로 할당
  column의 offset에 데이터 누적하는 데이터 크기, 시작하는 데이터 바이트 위치 할당
  다음 쿨룽으로 진입
모두 마치면 meta의 column_count를 column_count로 넣기
마찬가지로 meta의 row_size를 offset을 넣어 총 크기 넣기



parse_csv_line
count로 값을 계속 올림
cursor의 초기값을 line으로 지정.
while로 cursor가 NULL이 될때 까지 순회
parts에 count에 해당하는 index에 cursor를 할당
cursor를 strchr로 다음 ","가 있는 곳으로 주소를 변경
그리고 ,가 있는 곳을 '\0'으로 변경
cursor의 주소를 다음 주소로 변경 후 count 리턴



append_binary_row
row(1024개)
find_child를 이용하여 root부터 NODE_VALUE_LIST를 가져와 value_list에 할당한다.
row를 모두 0으로 초기화
for문으로 meta 데이터의 column을 모두 확인
  value_node가 NULL이면 에러 리턴
  row에 column에 해당하는 데이터를 모두 넣어서 하나의 열로 만듬
  value_node를 다음 next_sibling으로 넘겨서 그 다음 열에 대해서 넣을 데이터를 row에 넣음
ensure_parent_directory로 바이너리 부모 경로(schema)를 만듬
data_file_path에 추가로 row를 넣음


value_node

write_value로 데이터 삽입
토큰의 데이터 타입이 COL_INT이면 토큰 타입이 AST_VALUE_NUMBER가 아니거나 parse_integer_literal의 값이 0이면 에러로 리턴
parsed에 들어간 숫자를 row int에 buffer + column->offset부터 int의 크기까지 넣는다.

토큰의 데이터 타입이 COL_CHAR면 value_node의 text 값을 temp에 넣고 AST_VALUE_STRING 타입이면 strip_quotes으로 양옆의 작은 따옴표를 제거한다.

length를 strlen으로 구한 temp의 길이를 넣는다.
length가 column의 size보다 크면 에러를 리턴
아니면 memset으로 buffer + offset부터 해당 column의 사이즈까지 모두 \0으로 채워 넣고
memcpy로 buffer+column -> offset부터 temp로 길이만큼 넣는다.



parse_integer_literal
strtol을 통해서 text를 숫자로 바꾸고 text의 값이 \0이거나 end의 값이 \0이 아니면 0으로 리턴 아니면 parsed에 해당 값을 넣고 1 리턴



execute_select
NODE_COLUMN_LIST를 find_child로 찾고 column_list에 넣음
NODE_WHERE를 find_child로 찾고 column_list에 넣음
COLUMN_list가 NULL이면 에러
만약 select 이후 노드가 *인지 확인하는 is_select_all으로 true면 모든 열을 찾아서 selected_indexes의 값에 해당하는 인덱스를 넣음
아니면 for 문으로 column_list에서 first_child부터 next_sibling으로 순회하여 find_column_index를 찾고 해당하는 column을 찾고 selected_indexes에 해당하는 인덱스를 넣음

while 문과 fread를 이용하여 데이터 파일을 열어서 열들을 출력하고 row_matches_where 함수를 이용하여 맞으면 1, 틀리면 0, 오류는 -1을 보낸다. 이때 where_node를 NULL로 보내면 모두 맞는걸로 한다.
오류면 종료하고 아니면 다음으로 보낸다.
만약 맞다면 column 값을 보내고 종료한다.
free_ast, free_tokens으로 각각 초기화 하고 끝낸다.