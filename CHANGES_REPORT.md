# FlexQL Changes

Summary of everything changed this session: one added feature area (REPL history), one correctness fix that unlocked an optimization, three code-review bug fixes, and a JOIN rewrite (algorithmic optimization + a new auto-detect feature). Ordered roughly as the work happened, since later items build on earlier ones.

---

## 1. Added feature: command history + arrow-key editing in the REPL client

**File:** `src/client/repl.cpp`

The interactive client (`bin/flexql-client`) previously read input with plain `getline` no Up/Down history recall, no in-line cursor editing. Replaced it with a small raw-mode line editor:

- Terminal is switched to raw mode (`termios`) only for interactive (TTY) sessions; piped input (as used by `tests/concurrency_test.sh`) still goes through plain `getline`, unchanged.
- **Up/Down** recall previous statements; **Left/Right** move the cursor; Backspace edits in place.
- History (`g_history`) is a plain in-process `vector<string>` scoped to that one client process's memory, so concurrently running client processes never share or interleave each other's history (no shared file, no locking needed).

This is also where the async-signal-safety and Escape-key bugs fixed in section 3 originated.

---

## 2. Optimization: enabled the B+ Tree index for DECIMAL primary keys

**File:** `src/query/executor.cpp`

**Before:** the point-lookup/range fast path (`Find_By_FK` / `Find_By_Range`) only activated when a primary key's column type was `ColType::INT`. But `.help` only documents `DECIMAL`, `VARCHAR(n)`, `TEXT` as usable types there's no user-facing `INT` so every table anyone actually created with a `PRIMARY KEY` fell straight through to a full table `Scan`, silently ignoring the index. Benchmarked: a PK point lookup on a 1,000,000-row table took **763 ms**, *slower per-row than a full scan*.

**Fix:** extended the fast-path eligibility check to include `DECIMAL` (the B+ Tree was already correctly populated for `DECIMAL` PKs `page.cpp` stores `integer_value` for both `INT` and `DECIMAL` fields only the executor was refusing to use it).

**Result:** the same 1,000,000-row point lookup dropped to **78 ms** (~10x faster), and at a smaller (300k-row, warm-cache) scale, to **0 ms**. This fix is what exposed the truncation bug described next.

---

## 3. Code-review bug fixes

Three bugs surfaced by `/code-review`, all tracing back to sections 1–2 above.

### 3a. DECIMAL primary-key collisions could return the wrong row

**File:** `src/query/executor.cpp`, **Severity:** Correctness, silent data corruption on read.

The B+ Tree index stores keys as truncated `int32_t` (`(int32_t)double_value`, `src/storage/page.cpp:21-23`). Enabling the fast path for `DECIMAL` PKs (section 2) didn't account for this truncation:

- **Insert collision:** `INSERT (1.5, 'a'), (1.9, 'b')`, both rows truncate to key `1`, so the second insert overwrites the first row's index entry. Row `'a'` stays on disk but becomes unreachable through the index.
- **Wrong-row read:** `SELECT * FROM T WHERE ID = 1.5` truncates to key `1` and returns whichever row currently owns that key — silently returning `'b'` for a query asking for `1.5`.
- **Range boundary drift:** the same truncation could shift range boundaries, e.g. `WHERE ID > 1.5` becoming `lo = 1, lo_inc = false`, wrongly excluding a real row at `1.9`.

**Fix:**

- Equality queries verify the fetched row's actual `double_value` against the query value before trusting it; on a mismatch (or a miss) they fall through to the full-scan path instead of trusting the index, since a collision can hide the real row behind a different key.
- Range queries (`>`, `>=`, `<`, `<=`) on a `DECIMAL` PK bail out of the fast path entirely and use the full scan there's no cheap way to verify a *missing* row after the fact.
- `INT` PKs are unaffected (no truncation possible).

**Verified:** `INSERT (1.5,'a'), (1.9,'b')` then `WHERE ID = 1.5` / `WHERE ID = 1.9` each now return the correct row (previously `1.5` returned `'b'`).

### 3b. Standalone Escape keypress hung and swallowed the next character

**File:** `src/client/repl.cpp`, **Severity:** UX bug, REPL appears to freeze.

On seeing `0x1b` (ESC), the reader unconditionally did a **blocking** `getchar()` expecting the `[` of an arrow-key sequence. A real arrow key sends `ESC [ A` as a burst; a standalone Escape sends only the one byte. Pressing Escape alone blocked until the user's *next* keystroke, which then got consumed as part of a bogus escape sequence instead of inserted into the line.

**Fix:** added `waitAndGetchar(timeout_ms)`, using `poll()` with a 50 ms timeout instead of a blocking read for the byte(s) after ESC. A real sequence arrives well inside that window; a standalone Escape times out and is ignored — no hang, no dropped character. Also added `setvbuf(stdin, nullptr, _IONBF, 0)` so `poll()`'s view of the fd and `getchar()`'s buffered reads stay consistent.

**Verified:** pty-driven test typed `SH`, pressed a standalone Escape, waited past the timeout, typed `OW DATABASES;` the full statement assembled correctly and executed.

### 3c. SIGINT handler called non-async-signal-safe functions

**File:** `src/client/repl.cpp` — **Severity:** Reliability, possible deadlock on Ctrl+C.

`handleSigint()` called `printf()` and (via `disableRawMode()`) `tcsetattr()` directly from signal-handler context, neither is POSIX async-signal-safe. If SIGINT arrived while the main thread held libc's internal stdio lock (e.g. mid-`redraw()`), re-entering `printf` from the handler risked deadlock. (Raw mode leaves `ISIG` enabled, so Ctrl+C genuinely delivers `SIGINT` via the tty driver this path is reachable in normal use, not just theoretical.)

**Fix:** `handleSigint()` now only sets a `volatile sig_atomic_t` flag. The main read loop blocks on `poll()` instead of raw `getchar()`; POSIX guarantees `poll()`/`select()` return `EINTR` on a caught signal regardless of `SA_RESTART`. Cleanup (`disableRawMode()`, `printf`, `exit(130)`) now runs from ordinary post-`poll()` control flow, not from inside the handler.

**Verified:** pty-driven test sent partial input, then `SIGINT` mid-line; process exited cleanly with status 130, confirmed via `waitpid()` with a timeout that would flag a deadlock.

---

## 4. Optimization: replaced nested-loop JOIN with a hash join

**File:** `src/query/executor.cpp`

**Before:** `INNER JOIN` did a full nested-loop scan for every row in the left table, it rescanned the *entire* right table looking for matches: `O(n×m)`. Measured at ~2.7M row-pair comparisons/sec regardless of table size. A join between two 1,000,000-row tables would have taken **roughly 10 hours**.

**Fix:** scan the right-hand table once into an `unordered_map<string, vector<Row>>` keyed by the join column's value, then do a single pass over the left table with O(1)-average lookups into that map. Cost drops from `O(n×m)` to `O(n+m)`.

**Verified:**

- 50,000 × 50,000 join: **666 ms** (was minutes-to-unusable before).
- **1,000,000 × 1,000,000 join (measured directly, not estimated): 13.1 seconds**, all 1,000,000 expected 1:1 matches correct. That's roughly a **2,700x speedup** over the old nested-loop cost for the same input.
- Edge cases (empty right table, a WHERE filter matching nothing) correctly return 0 rows.
- Regression: unit tests 20/20, `concurrency_test.sh` 4/4.

Note on true Cartesian products (every row matches every row): no join algorithm, hash join included, can avoid `O(n×m)` when the *output itself* is `O(n×m)` rows that cost is constructing/emitting the result set, not finding matches. The hash join only helps when the join is reasonably selective, which is the common case.

---

## 5. Added feature: `INNER JOIN` without `ON` auto-detects the join column(s)

**Files:** `include/parser/parser.h`, `src/parser/parser.cpp`, `src/query/executor.cpp`

`ON` was previously mandatory in the grammar, `INNER JOIN <table>` with nothing else was a hard parse error. Made `ON` optional; when omitted, the executor auto-detects join columns by matching column names shared between the two tables' schemas (case-insensitive, same rule `Find_Column` already uses):

- **No shared column** → `ERROR: No common column found between 'X' and 'Y' to join on. Specify ON explicitly.`
- **One shared column** → joins on it, equivalent to an explicit `ON`.
- **Multiple shared columns** → ANDs all of them together (true `NATURAL JOIN` semantics) via a composite hash key (each row's matched-column values concatenated with a `\x1f` separator), not just the first match.

**Verified:**

- Zero-match case correctly errors instead of silently returning nothing.
- Single-match case matches the equivalent explicit-`ON` query exactly.
- Multi-match case confirmed as a true AND: a row agreeing on `ID` but disagreeing on `NAME` was correctly excluded, not included via an accidental OR.
- Explicit `ON` path re-verified unchanged after the refactor needed to share code with the auto-detect path.

## Overall regression status

All changes verified together against a clean database, after the `make clean` rebuild:

- `./bin/flexql-benchmark --unit-test` ~ **20/20 passed, 30/30 marks**
- `bash tests/concurrency_test.sh 4` ~ **all 4 scenarios passed**
- Full 1,000,000-row command benchmark all steps `[PASS]`
- 1,000,000 × 1,000,000 JOIN — measured 13.1s, all matches correct

No regressions found. All changes are currently uncommitted.
