# B+ Tree Mini DBMS

This project implements a small C-based MySQL-style CLI database for the
`users` table.

Storage layout:

- `data/users.schema.csv`: column schema and primary-key metadata
- `data/users.indexes.csv`: index metadata
- `data/users.data`: fixed-size binary records
- `data/users.meta`: record count and auto-increment metadata

The program always uses the project-local `data/` directory as its default
database directory. On startup it loads schema, index metadata, binary records,
and table metadata from those files, then rebuilds the in-memory B+ tree
indexes.

Source layout:

- `src/main.c`: process entry point only
- `src/ast.c`, `src/ast.h`: SQL command AST classification
- `src/engine.c`, `src/engine.h`: REPL loop and app entry point
- `src/executor.c`: SQL command execution
- `src/query.c`: WHERE parsing, result buffers, table output, range scan helpers
- `src/storage.c`: schema CSV, binary data files, table metadata, index metadata
- `src/bptree.c`: in-memory B+ tree insert/search/range traversal primitives
- `src/util.c`: shared string, key, column, timing, and error utilities
- `src/engine_internal.h`: internal structs and cross-module function declarations

Build:

```sh
make
```

Run:

```sh
./mysql_bptree
```

Run with Docker on Ubuntu:

```sh
docker build -t mysql-bptree .
make docker-run
```

`make docker-run` mounts the project `data/` directory to `/app/data` inside
the Ubuntu container, so inserted or benchmark-generated records remain visible
from the host machine.

Equivalent raw Docker command:

```sh
mkdir -p data
docker run --rm -it -v "$PWD/data:/app/data" mysql-bptree
```

Useful commands:

```sql
INSERT INTO users (name, age, email) VALUES ('kim', 20, 'kim@test.com');
SELECT * FROM users WHERE id = 1;
SELECT * FROM users WHERE id > 100;
SELECT * FROM users WHERE id <= 1000;
SELECT * FROM users WHERE id BETWEEN 100 AND 200;
SELECT * FROM users FORCE INDEX (PRIMARY) WHERE id = 1;
SELECT * FROM users FORCE INDEX (PRIMARY) WHERE id BETWEEN 100 AND 200;
SELECT * FROM users IGNORE INDEX (PRIMARY) WHERE id = 1;
CREATE INDEX idx_users_name ON users (name);
CREATE UNIQUE INDEX idx_users_email_unique ON users (email);
ALTER TABLE users MODIFY PRIMARY KEY (email);
SHOW INDEX FROM users;
EXPLAIN SELECT * FROM users WHERE id = 1;
SAVE;
LOAD SCHEMA 'data/users.schema.csv' INTO TABLE users;
LOAD DATA BINARY 'data/users.data' INTO TABLE users REPLACE;
BENCHMARK 1000000 INDEX (id, name, age);
EXIT;
```

Run smoke tests:

```sh
make test
```
