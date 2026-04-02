# FlexQL Design Document
FlexQL is a custom, multithreaded relational database management system (RDBMS) written in C++17. It operates on a client-server architecture using TCP sockets and a custom binary wire protocol. The engine supports standard SQL operations (DDL and DML), custom row-level expiration (TTL), B+ Tree indexing for primary keys, and an application-level LRU page cache.

---

## 2. Architecture & Major Design Decisions

### 2.1. Network & Client-Server Model
* **Thread-per-Client:** The server (`main.cpp`) listens on a TCP socket (default port 9000). For every accepted connection, it detaches a new `std::thread` to handle the client session.
* **Custom Wire Protocol:** Defined in `flexql_protocol.h`, the protocol uses a length-prefixed message header structure. Message types include:
  * `MSG_QUERY`: Client sending SQL to the server.
  * `MSG_ROW_HEADER`: Server sending column definitions.
  * `MSG_ROW`: Server sending serialized row data.
  * `MSG_STATUS`: Server sending the final success/error message.
* **No Nagle's Algorithm:** `TCP_NODELAY` is set to send row results instantly without batching latency, supported by 1MB socket buffers for high throughput.

### 2.2. Storage Engine & Page Layout
* **Slotted Pages:** Data is stored in fixed-size blocks defined by `PAGE_SIZE = 16384` (16KB). The `Page` struct (`page.h`) uses a slotted page architecture where the page header grows forwards and records (rows) grow backwards from the end of the page.
* **Binary Serialization:** Rows are serialized into dense binary formats (`page.cpp`).
  * Fixed-length types (`INT`, `DECIMAL`) use 8 bytes (cast to `double` internally for numeric uniformity).
  * `BOOLEAN` is stored as a 1-byte integer.
  * `VARCHAR` is stored with a 2-byte length prefix followed by character data.
  * **Null handling:** A 1-byte sentinel `0xFF` represents `NULL`.
* **File Separation:** A single table is backed by three distinct files managed by `TableManager`:
  * `<table_name>.tbl`: The raw data pages.
  * `<table_name>.idx`: The serialized B+ Tree index.
  * `<table_name>.schema`: The serialized table schema metadata.

### 2.3. Caching & Indexing
* **LRU Cache:** Implemented in `lru_cache.h`, the cache stores up to 4,096 pages (64 MB per table). When capacity is reached, the Least Recently Used (LRU) page is evicted. If marked `dirty`, it flushes to disk via `TableManager::Write_To_Disk`.
* **B+ Tree Primary Index:** The engine enforces a B+ Tree index automatically on the primary key (`bplus_tree.h`). It maps the Primary Key (`int32_t`) to an `IndexRecord` containing `(page_id, slot_idx)`. Point lookups (`WHERE ID = X`) route directly through this index rather than scanning the table.

### 2.4. Concurrency Control
* **Coarse-Grained Locking:** Concurrency is handled primarily at the database and table levels using `std::shared_mutex` and `std::mutex` (`executor.h`, `table_manager.h`).
  * Reading schema/metadata uses `shared_lock` (Readers).
  * Modifications (Insert, Delete, DB creation) use `unique_lock` or `lock_guard` (Writers).

### 2.5. Special Features
* **Row Expiration (TTL):** Rows optionally carry an `expiry_ts` (via `EXPIRY N` in the SQL `INSERT`). When pages are read, `Has_Expired()` (`expiry.h`) is evaluated. Expired rows are dynamically skipped during `Scan()`, functioning as an automatic TTL feature.

---

## 3. Query Execution Workflow

The lifecycle of a query from the client application to the server's disk and back follows this path:

### Step 1: Client Request (`flexql_client.cpp`)
1. The client calls `flexql_exec(db, "SELECT ...", callback)`.
2. The query string is packaged with a `MsgHeader` (`MSG_QUERY`) and sent over the TCP socket.

### Step 2: Server Routing (`main.cpp`)
1. The server's client thread receives the `MSG_QUERY` payload.
2. The string is passed to the parser pipeline.

### Step 3: Lexical Analysis & Parsing (`lexer.cpp` -> `parser.cpp`)
1. **Lexer:** Scans the raw SQL string, skipping whitespace, and converting characters into a `std::vector<Token>` of types like `TT::SELECT`, `TT::IDENT`, `TT::LIT_INT`.
2. **Parser:** Uses a recursive-descent approach to consume tokens and produce a `ParsedQuery` struct representing the AST. It validates syntax, extracts column references (`ColRef`), WHERE clauses (`WhereCl`), and JOIN conditions.

### Step 4: Execution (`executor.cpp`)
The `Database::Execute()` method routes the `ParsedQuery` based on its type:
* **DDL (CREATE TABLE/DB):** Instantiates `TableSchema`, creates the corresponding `.tbl`, `.idx`, and `.schema` files on disk via the `TableManager`.
* **INSERT:** Normalizes input strings into `Field` objects, applies the TTL timestamp, and passes the `Row` to `TableManager::Insert_Row()`. The cache is marked dirty, and the B+ Tree is updated.
* **SELECT:** * If it's a primary key point-lookup (e.g., `WHERE id = 5`), it skips the full scan and queries the B+ Tree.
  * Otherwise, it triggers a `tm->scan()`, iterating through all cached/disk pages.
  * Filters (`WHERE`), joins (`INNER JOIN` uses a nested-loop strategy), and sorting (`ORDER BY` using `std::stable_sort`) are applied in memory.
  * Projections (selected columns) are mapped to a `ResultRow`.

### Step 5: Returning Results (`main.cpp` -> `net_utils.cpp`)
1. As the Executor evaluates valid `ResultRow` objects, it triggers a callback.
2. The server packages the column names into a `MSG_ROW_HEADER` (sent once).
3. Each valid row is packed using `net_packFields` (null-byte delimited strings) and sent as `MSG_ROW`.
4. Once execution finishes, a final `MSG_STATUS` (e.g., "OK: 5 row(s) returned.") is sent.

### Step 6: Client Delivery
The client's `flexql_exec` loop reads the TCP stream, unpacks the C-strings (`unpackToC`), and invokes the user-provided C-callback function for each row until `MSG_STATUS` is received.

---

## 4. Client API (Driver)
The client library provides a standard C-style driver interface (`flexql.h`):

```c
// Opens a socket connection and sets up the FlexQL handle
int flexql_open(const char* host, int port, FlexQL** db);

// Executes a SQL statement. Rows are yielded via the provided callback.
int flexql_exec(
    FlexQL* db,
    const char* sql,
    int       (*callback)(void*, int, char**, char**), // (arg, col_count, values, col_names)
    void* arg,
    char** errmsg
);

// Closes the connection and cleans up
int flexql_close(FlexQL* db);