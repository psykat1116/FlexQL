# FlexQL Design Document

FlexQL is a custom, multithreaded relational database management system (RDBMS) written in C++17. It operates on a client-server architecture using TCP sockets and a custom binary wire protocol. The engine supports standard SQL operations (DDL and DML), custom row-level expiration (TTL), B+ Tree indexing for primary keys, an application-level LRU page cache, and AND-chained WHERE predicates with full index acceleration for range queries.

---

## 1. High-Level Architecture

```bash
┌─────────────────────────────────────────────────────────────────┐
│                        FlexQL Server                            │
│                                                                 │
│  ┌──────────┐    ┌─────────────────────────────────────────┐    │
│  │ TCP Port │    │          DatabaseManager                │    │
│  │  :3000   │    │  ┌──────────┐  ┌──────────┐             │    │
│  └────┬─────┘    │  │  DB "A"  │  │  DB "B"  │  ...        │    │
│       │          │  └────┬─────┘  └──────────┘             │    │
│  ┌────▼─────┐    │       │                                 │    │
│  │ accept() │    │  ┌────▼──────────────────────────┐      │    │
│  └────┬─────┘    │  │         Database              │      │    │
│       │          │  │  ┌──────────┐  ┌──────────┐   │      │    │
│  ┌────▼─────┐    │  │  │ Table-1  │  │ Table-2  │   │      │    │
│  │ detach   │    │  │  └──────────┘  └──────────┘   │      │    │
│  │ thread   │    │  └───────────────────────────────┘      │    │
│  └────┬─────┘    └─────────────────────────────────────────┘    │
│       │                                                         │
│  ┌────▼────────────────────────────────────────────────────┐    │
│  │                  Client Handler Thread                  │    │
│  │  TCP recv → Lexer → Parser → Executor → TCP send        │    │
│  └─────────────────────────────────────────────────────────┘    │
└─────────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────────┐
│                       FlexQL Client                             │
│  flexql_open() → connect → flexql_exec() → callback per row     │
└─────────────────────────────────────────────────────────────────┘
```

---

## 2. Query Execution Workflow
Every query follows a strict pipeline from client → network → parse → execute → storage → response.

```bash
Client Application
       │
       │  flexql_exec(db, "SELECT * FROM users WHERE id > 10 AND id < 20", callback)
       │
       ▼
┌─────────────────────────────────────────────────────┐
│  Step 1 · Wire Protocol (flexql_protocol.h)         │
│                                                     │
│  ┌──────────────────────────────┐                   │
│  │ MsgHeader                    │                   │
│  │  type   : MSG_QUERY (1 byte) │                   │
│  │  length : payload size       │                   │
│  └──────────────────────────────┘                   │
│       + raw SQL string                              │
└────────────────────┬────────────────────────────────┘
                     │  TCP socket
                     ▼
┌─────────────────────────────────────────────────────┐
│  Step 2 · Lexer  (lexer.cpp)                        │
│                                                     │
│  "SELECT * FROM users WHERE id > 10 AND id < 20"    │
│       │                                             │
│       ▼                                             │
│  [SELECT] [*] [FROM] [IDENT:"users"] [WHERE]        │
│  [IDENT:"id"] [GT] [LIT_INT:10]                     │
│  [AND] [IDENT:"id"] [LT] [LIT_INT:20]               │
└────────────────────┬────────────────────────────────┘
                     │  std::vector<Token>
                     ▼
┌─────────────────────────────────────────────────────┐
│  Step 3 · Parser  (parser.cpp)                      │
│                                                     │
│  Recursive-descent → ParsedQuery struct             │
│  ┌──────────────────────────────────────┐           │
│  │ type       : SELECT                  │           │
│  │ from_table : "users"                 │           │
│  │ select_star: true                    │           │
│  │ where_list : [                       │           │
│  │   { col:"id", op:">",  val:"10" },   │           │
│  │   { col:"id", op:"<",  val:"20" }    │           │
│  │ ]                                    │           │
│  └──────────────────────────────────────┘           │
└────────────────────┬────────────────────────────────┘
                     │  ParsedQuery
                     ▼
┌─────────────────────────────────────────────────────┐
│  Step 4 · Executor  (executor.cpp)                  │
│                                                     │
│  Analyse where_list against PK column               │
│       │                                             │
│    ┌──┴──────────────┐                              │
│    │                 │                              │
│  All conditions    Not all on PK                    │
│  on INT PK?        (non-PK col, != op,              │
│    │               no WHERE)                        │
│    │                 │                              │
│  ┌─┴──────────┐      ▼                              │
│  │op == "=" ? │   Full table scan via tm->Scan()    │
│  └─┬──────┬───┘   (page-by-page, Row_Matches_All)   │
│  YES      NO                                        │
│    │       │                                        │
│    ▼       ▼                                        │
│  Point   Range scan via tm->Find_By_Range()         │
│  lookup  btree.Find_Range(lo, hi)                   │
│  O(logN) O(logN + K), K = matching rows             │
│    │       │                                        │
│    └──┬────┘                                        │
│       │                                             │
│       ▼   Apply ORDER BY (std::stable_sort)         │
│       │   Apply JOIN (nested-loop)                  │
│       │   Map to ResultRow                          │
└────────────────────┬────────────────────────────────┘
                     │  ResultRow callback
                     ▼
┌─────────────────────────────────────────────────────┐
│  Step 5 · Response (main.cpp + net_utils.cpp)       │
│                                                     │
│  ① MSG_ROW_HEADER  →  column names (sent once)      │
│  ② MSG_ROW         →  one per result row            │
│     Fields packed as null-byte-delimited C-strings  │
│  ③ MSG_STATUS      →  "OK: N row(s) returned.       │
│                        (0.001s)"                    │
│     Wall-clock execution time appended to every     │
│     OK status message (measured in main.cpp using   │
│     std::chrono::steady_clock).                     │
└────────────────────┬────────────────────────────────┘
                     │  TCP socket
                     ▼
             Client callback invoked
             for each row until MSG_STATUS
```

---

## 3. Storage Engine & Binary Layout

### 3.1 On-Disk File Structure
Each table is backed by **three files** stored in the data directory:

```bash
./data/
  └── <database_name>/
        ├── <table_name>.tbl       ← raw data pages (slotted pages)
        ├── <table_name>.idx       ← serialized B+ Tree index
        └── <table_name>.schema    ← serialized TableSchema metadata
```

### 3.2 Page Layout (16 KB slotted page)
Every `.tbl` file is a sequence of fixed-size 16 KB pages. Each page uses a **slotted-page architecture**: the header and slot directory grow from the front, while records grow backwards from the end.

```bash
┌────────────────────────── PAGE_SIZE = 16384 bytes ───────────────────┐
│                                                                      │
│  Offset 0                                          Offset PAGE_SIZE-1│
│  ┌──────────┬────────────────────┬────────────────────────────────┐  │
│  │PageHeader│  Slot Directory    │  Free Space  │  Row Data       │  │
│  │ 12 bytes │  (grows →)         │              │  (← grows)      │  │
│  └──────────┴────────────────────┴────────────────────────────────┘  │
│                                                                      │
│  PageHeader (packed, 12 bytes):                                      │
│  ┌──────────┬──────────┬──────────┬──────────────┬──────┬──────┐     │
│  │ page_id  │next_page │num_slots │free_space_off│flags │ pad  │     │
│  │ uint32   │ uint32   │ uint16   │   uint16     │uint8 │ [3]  │     │
│  └──────────┴──────────┴──────────┴──────────────┴──────┴──────┘     │
│                                                                      │
│  SlotEntry (per row, 4 bytes each):                                  │
│  ┌──────────────┬──────────────┐                                     │
│  │   offset     │    length    │    → points into row data area      │
│  │   uint16     │   uint16     │                                     │
│  └──────────────┴──────────────┘                                     │
│                                                                      │
│  Row Data (packed bytes, grows from end of page):                    │
│  ┌───┬───────────────────────────────────────────────────────────┐   │
│  │ … │  Row N  │  Row N-1  │  …  │  Row 1  │  Row 0              │   │
│  └───┴───────────────────────────────────────────────────────────┘   │
└──────────────────────────────────────────────────────────────────────┘
```

### 3.3 Row Binary Serialization
Rows are serialized into dense binary representations. Each field is encoded as follows:

```
For each column in schema order:

  NULL field:
  ┌───────┐
  │ 0xFF  │   1 byte sentinel
  └───────┘

  INT field:
  ┌───────┬───────────────────────────────┐
  │ 0x00  │  value (cast to double, 8 B)  │
  └───────┴───────────────────────────────┘

  DECIMAL field:
  ┌───────┬───────────────────────────────┐
  │ 0x00  │  double (8 bytes, IEEE 754)   │
  └───────┴───────────────────────────────┘

  BOOLEAN field:
  ┌───────┬───────┐
  │ 0x00  │ 0/1   │   2 bytes total
  └───────┴───────┘

  VARCHAR field:
  ┌───────┬──────────┬──────────────────────────────┐
  │ 0x00  │  length  │  character data (no NUL term)│
  │       │ (2 bytes)│  (length bytes)              │
  └───────┴──────────┴──────────────────────────────┘

  Row trailer:
  ┌─────────────────────────┐
  │  expiry_timestamp       │   uint64_t, 8 bytes
  │  (0 = never expires)    │
  └─────────────────────────┘
```

### 3.4 LRU Page Cache
The cache sits between the executor and raw disk I/O, holding up to **4,096 pages (64 MB per table)** in memory.

```
Executor calls tm->Get_Page(page_id)
             │
             ▼
     ┌────────────────┐
     │  LRU Cache     │   (lru_cache.h)
     │  capacity=4096 │
     │                │
     │  key: page_id  │
     │  val: Page*    │
     │                │
     │  hit? ─────────┼──→  return cached Page (O(1))
     │                │
     │  miss? ────────┼──→  load from .tbl file
     │                │         │
     │  evict LRU ────┼──→  if dirty → Write_To_Disk()
     └────────────────┘         │
                                ▼
                          Insert into cache,
                          return Page*
```

---

## 4. B+ Tree Indexing
The engine maintains a **B+ Tree** on every table's primary key, stored in the `.idx` file. It maps `int32_t` primary key → `IndexRecord { page_id, slot_idx }`.

### 4.1 Tree Structure

```bash
                    ┌─────────────────────┐
                    │     Internal Node   │
                    │      [30 | 60]      │
                    └──────┬──────┬───────┘
                           │      │
              ┌────────────┘      └────────────┐
              ▼                                ▼
  ┌───────────────────┐              ┌───────────────────┐
  │   Internal Node   │              │   Internal Node   │
  │   [10 | 20]       │              │   [45 | 55]       │
  └──┬────────┬───────┘              └──┬────────┬───────┘
     │        │                         │        │
     ▼        ▼                         ▼        ▼
  ┌──────┐  ┌──────┐               ┌──────┐  ┌──────┐
  │Leaf  │  │Leaf  │  ←── linked ──│Leaf  │  │Leaf  │
  │1,2,3 │→ │10..20│               │30..44│→ │45..59│
  └──────┘  └──────┘               └──────┘  └──────┘
     Leaf nodes are linked for range scans →
     Each leaf entry: { key: int32, page_id: uint32, slot_idx: uint16 }
```

### 4.2 Index Lookup Paths
The executor analyses the `where_list` before choosing a path. There are three mutually exclusive routes:

**Path 1 — Point lookup** (`WHERE pk = X`): a single `std::map::find` in O(log N). Only one page is fetched.

```sql
SELECT * FROM orders WHERE id = 42
                │
                ▼
   all_on_pk=true, has_eq=true
                │
                ▼
   btree.Find_Key(42) → IndexRecord { page_id=7, slot_idx=3 }
                │
                ▼
   Fetch_Page(7) → page->getRow(3) → Deserialize → ResultRow
```

**Path 2 — Range scan** (`WHERE pk OP val [AND pk OP val ...]`): uses `std::map::lower_bound` / `upper_bound` to walk only the matching leaf entries. Cost is O(log N + K) where K is the number of matching rows. Zero pages outside the result set are read.

```sql
SELECT * FROM users WHERE id > 10 AND id < 20
                │
                ▼
   all_on_pk=true, has_eq=false
   bounds derived: lo=10 (exclusive), hi=20 (exclusive)
                │
                ▼
   btree.Find_Range(10, false, 20, false)
     → iterator from upper_bound(10) to first key >= 20
     → returns 9 IndexRecords (ids 11..19)
                │
                ▼
   For each IndexRecord: Fetch_Page(pid) → getRow(slot) → Deserialize
   Only 9 pages touched (likely 1 if rows are contiguous)

SELECT * FROM users WHERE id < 50
                │
                ▼
   lo=INT32_MIN (open lower bound), hi=50 (exclusive)
   Condition (lo != INT32_MIN || hi != INT32_MAX) → TRUE → index fires
                │
                ▼
   btree.Find_Range(INT32_MIN, true, 50, false)
     → iterator from map::begin() to first key >= 50
     → returns 49 IndexRecords
```

**Path 3 — Full scan**: used only when the WHERE clause references a non-PK column, uses `!=`, or there is no WHERE clause at all. Iterates every page via `tm->Scan()` and applies `Row_Matches_All()` to check all AND conditions.

```sql
SELECT * FROM orders WHERE status = 'open'
                │
                ▼
   all_on_pk=false  →  full scan
                │
                ▼
   tm->Scan(): page 0, 1, 2 … N
     for each slot: Deserialize → Row_Matches_All(where_list) → collect
                │
                ▼
   Apply ORDER BY (std::stable_sort in memory)
   Return ResultRows to client
```

### 4.3 Index Persistence and Auto-Rebuild
The B+ Tree is saved to the `.idx` file on every clean shutdown via `Flush_Table()`. If the server is killed before the first flush (e.g. during initial bulk load), the `.idx` file will not exist on the next startup.

On `TableManager` construction the server checks for the `.idx` file:

```bash
TableManager opens existing table
             │
             ▼
     Does .idx file exist?
       │            │
      YES            NO
       │            │
  Load_Tree()   Rebuild_Index()
                  scan all .tbl pages
                  re-insert every live PK
                  into btree in-memory
                     │
                     ▼
                  Save_Tree()   ← persisted immediately
                                   so next startup is fast
```

---

## 5. Architecture & Major Design Decisions

### 5.1 Network & Client-Server Model
**Thread-per-Client** — the server (`main.cpp`) listens on TCP port 3000 (configurable). For every accepted connection it detaches a new `std::thread` to handle the full session lifecycle (receive → parse → execute → respond → loop).

**Custom Wire Protocol** — defined in `flexql_protocol.h`. All messages use a length-prefixed `MsgHeader`:

| Message Type    | Direction       | Payload                                        |
|-----------------|-----------------|------------------------------------------------|
| `MSG_QUERY`     | Client → Server | Raw SQL string                                 |
| `MSG_ROW_HEADER`| Server → Client | Null-byte-delimited column names               |
| `MSG_ROW`       | Server → Client | Null-byte-delimited field values               |
| `MSG_STATUS`    | Server → Client | `"OK: … (0.001s)"` or `"ERROR: …"` final msg  |

**Execution timing** — `main.cpp` starts a `std::chrono::steady_clock` timer immediately before the `try` block that runs the query pipeline. The elapsed time in seconds is appended to every `OK` status message before it is sent as `MSG_STATUS`. Example output: `OK: 9 row(s) returned. [B+ Tree range scan] (0.001s)`.

**Socket tuning** — `TCP_NODELAY` is set to disable Nagle's algorithm (eliminates batching latency for streaming row results). Send/receive buffers are enlarged to 1 MB to reduce syscall count on large result sets.

### 5.2 Concurrency Control
Concurrency is coarse-grained, locking at the database and table manager levels:

```bash
Read operations (schema, metadata):
    shared_lock  →  multiple readers allowed concurrently

Write operations (INSERT, DELETE, CREATE TABLE):
    unique_lock / lock_guard  →  exclusive access
```

This is a deliberate simplicity trade-off: no deadlock complexity, no MVCC overhead, suitable for moderate concurrency loads.

### 5.3 Row Expiration (TTL)
Rows optionally carry an `expiry_timestamp` (set via `EXPIRY N` in INSERT). The expiry check is evaluated lazily at read time using `Has_Expired()` in `expiry.h`. Expired rows are transparently skipped during `Scan()` and `Find_By_Range()` — no background reaping thread is needed. This provides automatic TTL semantics without write amplification.

```sql
INSERT INTO sessions (id, token) VALUES (1, 'abc') EXPIRY 3600;
    │
    ▼
Row stored with expiry_timestamp = now() + 3600 seconds

Later SELECT:
  Deserialize row → check Has_Expired(expiry_ts)
    → expired? skip silently
    → live?    return to client
```

### 5.4 Compound WHERE Predicates
The parser collects all AND-chained conditions into `ParsedQuery::where_list` (a `vector<WhereClause>`). The executor iterates the full list when deciding the query path and when filtering rows in the full-scan path via `Row_Matches_All()`. This means queries like `WHERE id > 10 AND id < 20` correctly derive both bounds and use a single index range scan rather than two separate passes.

### 5.5 Schema Serialization
The `TableSchema` struct (max 32 columns, column names up to 64 chars) is stored as a flat binary blob in the `.schema` file. It is loaded once at database startup and held in memory for the lifetime of the connection.

---

## 6. Client API (Driver)
The client library (`flexql.h`) exposes a C-style API modeled after SQLite's interface:

```c
int flexql_open(const char* host, int port, FlexQL** db);

int flexql_exec(
    FlexQL* db,
    const char* sql,
    int (*callback)(void*, int, char**, char**),
    void* arg,
    char** errmsg
);

int flexql_close(FlexQL* db);
```

The callback is invoked once per result row as the client reads `MSG_ROW` messages from the TCP stream, until `MSG_STATUS` signals completion. Values are delivered as null-terminated C strings, regardless of the underlying column type.

---
## 7. Component Dependency Map

```bash
main.cpp
 ├── lexer.h / lexer.cpp          (tokenization, incl. AND keyword)
 ├── parser.h / parser.cpp        (AST production, where_list)
 ├── executor.h / executor.cpp    (query routing & execution)
 │    ├── table_manager.h/.cpp    (file I/O, insert, scan, range lookup)
 │    │    ├── page.h/.cpp        (slotted page read/write)
 │    │    ├── lru_cache.h/.cpp   (page cache)
 │    │    └── bplus_tree.h/.cpp  (PK index: Find_Key, Find_Range)
 │    ├── expiry.h                (TTL evaluation)
 │    └── types.h                 (Field, Row, ResultRow, TableSchema)
 ├── net_utils.h/.cpp             (send/recv helpers, field packing)
 └── flexql_protocol.h            (MsgHeader, message type constants)

flexql_client.cpp                 (client driver: open/exec/close)
 └── flexql.h                     (public C API)
```
