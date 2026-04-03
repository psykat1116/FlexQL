# FlexQL
A custom multithreaded relational database engine written in C++17, featuring a client-server architecture over TCP, a custom binary wire protocol, slotted-page storage, LRU page cache, B+ Tree primary-key indexing, and row-level TTL expiration.

---

## Requirements

- C++17-compatible compiler (GCC 9+ or Clang 10+)
- POSIX system (Linux or macOS)
- `make` or direct `g++` invocation
- No external dependencies — the entire engine is self-contained

---

## Building

### Server
Compile all server-side translation units together:

```bash
make
```

---

## Running
### Start the server
```bash
./bin/flexql_server [port] [data directory]
```

On startup you will see:

```bash
  FlexQL Server - PORT 3000
  Multithreaded | Ctrl+C to stop.
```

Press **Ctrl+C** to initiate a graceful shutdown. All dirty cache pages are flushed to disk before exit.

### Interactive REPL
With the server running, open a second terminal and launch the REPL. Default the host is `127.0.0.1` and port is `3000`

```bash
./bin/flexql-client [host] [port]
```

The REPL connects to `localhost:3000` by default. You can then type SQL statements directly:
```sql
CREATE DATABASE mydb;
USE mydb;
CREATE TABLE users (id INT PRIMARY KEY, name VARCHAR(100), score DECIMAL);
INSERT INTO users VALUES (1, 'Alice', 98.5);
INSERT INTO users VALUES (2, 'Bob', 72.0) EXPIRY 3600;
SELECT * FROM users;
SELECT * FROM users WHERE id = 1;
SELECT name, score FROM users WHERE score > 80 ORDER BY score;
```

Type `.exit` or `.quit` to close the REPL session.

### Run benchmark

```bash
./bin/flexql_benchmark
```

---

## SQL Reference
### DDL

```sql
CREATE DATABASE <name>;
USE <name>;
SHOW DATABASES;
SHOW TABLES;

CREATE TABLE <name> (
    <col> INT [PRIMARY KEY] [NOT NULL],
    <col> DECIMAL,
    <col> VARCHAR(<n>),
    <col> BOOLEAN
);
```

### DML

```sql
INSERT INTO <table> VALUES (val1, val2, ...) [EXPIRY <seconds>];
SELECT [*|col1,col2,...] FROM <table>
    [WHERE <col> <op> <val> [AND <col> <op> <val> ...]]
    [ORDER BY <col> [ASC|DESC]];
SELECT ... FROM <t1> INNER JOIN <t2> ON <t1.col> = <t2.col>;
DELETE FROM <table> WHERE <col> <op> <val>;
```

`EXPIRY <seconds>` is an optional FlexQL extension that marks the row for automatic expiration N seconds after insertion. Expired rows are silently skipped on reads.

---

## Data directory layout
```
./data/
  └── <database_name>/
        ├── <table_name>.tbl       ← slotted-page data file
        ├── <table_name>.idx       ← B+ Tree primary-key index
        └── <table_name>.schema    ← serialized table schema
```

Each `.tbl` file is a flat sequence of 16 KB pages. The server loads this data lazily through the LRU page cache (up to 4,096 pages = 64 MB per table in memory at once).

---

## Configuration summary
| Parameter        | Default     | How to change                        |
|------------------|-------------|--------------------------------------|
| Port             | `3000`      | First argument to `flexql_server`    |
| Data directory   | `./data`    | Second argument to `flexql_server`   |
| Page size        | `16384` B   | Compile-time constant in `types.h`   |
| LRU cache size   | `4096` pages| Compile-time constant in `lru_cache.h`|
| Max columns      | `32`        | `MAX_COLS` in `types.h`              |
| Max name length  | `64` chars  | `MAX_NAME` in `types.h`              |

---

## Troubleshooting
**`bind() failed on port 3000`** — the port is already in use. Either stop the existing process or start the server on a different port: `./flexql_server 3001`.

**`Connection failed` in REPL or client** — make sure the server is running before launching the client. Check that firewall rules allow the chosen port.

**Data not persisting after restart** — confirm the data directory path is the same across invocations. The server prints the active data directory on startup.

**`ERROR: No database selected`** — you must issue `USE <dbname>;` before running table-level queries. Use `SHOW DATABASES;` to list available databases.

---