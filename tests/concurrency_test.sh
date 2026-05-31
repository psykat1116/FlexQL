#!/usr/bin/env bash
# tests/concurrency_test.sh — FlexQL threadpool concurrency test
#
# Usage: bash tests/concurrency_test.sh [num_clients]
#   num_clients  concurrent clients per test scenario (default: 8)
#
# Requires server + client to be built first (or pass --build to auto-build).

set -uo pipefail

# ── Config ───────────────────────────────────────────────────────────────────
N=${1:-8}
PORT=3099                      # dedicated port — won't clash with dev server
DB_DIR=$(mktemp -d /tmp/flexql_conctest_XXXXXX)
LOG_DIR=$(mktemp -d /tmp/flexql_logs_XXXXXX)
SERVER_BIN=./bin/flexql-server
CLIENT_BIN=./bin/flexql-client
SERVER_PID=""
PASS=0
FAIL=0

# ── Colour helpers ───────────────────────────────────────────────────────────
RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'
CYAN='\033[0;36m'; BOLD='\033[1m'; NC='\033[0m'

hdr()  { echo -e "\n${YELLOW}${BOLD}[$1]${NC} $2"; }
info() { echo -e "  ${CYAN}$*${NC}"; }
ok()   { echo -e "  ${GREEN}✓ PASS${NC}: $*"; PASS=$((PASS + 1)); }
err()  { echo -e "  ${RED}✗ FAIL${NC}: $*"; FAIL=$((FAIL + 1)); }

# Row count: parses "  N row(s) in set." from REPL output (last occurrence).
row_count() {
    grep -oE '[0-9]+ row\(s\) in set' "$1" 2>/dev/null | awk '{print $1}' | tail -1
}

# Elapsed ms between two date +%s%3N timestamps.
elapsed_ms() { echo $(( $2 - $1 )); }

# ── Cleanup ──────────────────────────────────────────────────────────────────
cleanup() {
    if [[ -n "$SERVER_PID" ]]; then
        kill "$SERVER_PID" 2>/dev/null || true
        wait "$SERVER_PID" 2>/dev/null || true
    fi
    # Keep logs on failure so the user can inspect them.
    if [[ $FAIL -eq 0 ]]; then
        rm -rf "$DB_DIR" "$LOG_DIR"
    else
        echo -e "\n  Logs preserved at: ${CYAN}$LOG_DIR${NC}"
    fi
}
trap cleanup EXIT

# ── Build ────────────────────────────────────────────────────────────────────
hdr "BUILD" "Compiling server and client..."
if ! make -j"$(nproc)" bin/flexql-server bin/flexql-client -s 2>&1; then
    echo "Build failed — aborting." >&2
    exit 1
fi
info "Build OK"

# ── Start server ─────────────────────────────────────────────────────────────
hdr "SETUP" "Starting FlexQL server on port $PORT..."
$SERVER_BIN $PORT "$DB_DIR" >/dev/null 2>&1 &
SERVER_PID=$!

# Poll until the server accepts connections (up to 5 s).
ready=false
for _ in $(seq 1 10); do
    sleep 0.5
    if printf 'SHOW DATABASES;\n' | $CLIENT_BIN 127.0.0.1 $PORT >/dev/null 2>&1; then
        ready=true; break
    fi
done
if ! $ready; then
    echo "Server did not come up — aborting." >&2
    exit 1
fi
info "Server ready  (PID $SERVER_PID, DB dir: $DB_DIR)"

# ─────────────────────────────────────────────────────────────────────────────
# TEST 1 — Concurrent CREATE DATABASE (same name)
#
# Goal: exercise the database_mutex fix. All N clients simultaneously issue
#       "CREATE DATABASE racedb;". Exactly one should succeed; the rest should
#       get "already exists". Neither outcome should crash or hang the server.
# ─────────────────────────────────────────────────────────────────────────────
hdr "TEST 1" "Concurrent CREATE DATABASE — same name, $N clients"
info "All $N clients simultaneously: CREATE DATABASE racedb;"

pids=()
for i in $(seq 1 $N); do
    printf 'CREATE DATABASE racedb;\n' \
        | $CLIENT_BIN 127.0.0.1 $PORT >"$LOG_DIR/t1_$i.log" 2>&1 &
    pids+=($!)
done

responded=0
for pid in "${pids[@]}"; do
    wait "$pid" 2>/dev/null && responded=$((responded + 1)) \
                            || responded=$((responded + 1))
done

# Server liveness probe.
if printf 'SHOW DATABASES;\n' | $CLIENT_BIN 127.0.0.1 $PORT >/dev/null 2>&1; then
    ok "Server alive after $responded/$N concurrent CREATE DATABASE calls — no crash/deadlock"
else
    err "Server unreachable after concurrent CREATE DATABASE ($responded/$N responded)"
fi

# ─────────────────────────────────────────────────────────────────────────────
# TEST 2 — Concurrent isolated writes (each client owns a separate database)
#
# Goal: verify N connections work independently and produce correct data.
#       Each client: CREATE DB → CREATE TABLE → 20 INSERTs → SELECT *
#       Expected: every client sees exactly 20 rows.
# ─────────────────────────────────────────────────────────────────────────────
hdr "TEST 2" "Concurrent isolated writes — $N clients, each own database"
ROWS=20
info "Each client: CREATE DB + CREATE TABLE + ${ROWS} INSERTs + SELECT *"

pids=()
t_start=$(date +%s%3N)
for i in $(seq 1 $N); do
    {
        echo "CREATE DATABASE isoDb_${i};"
        echo "USE isoDb_${i};"
        echo "CREATE TABLE data(id DECIMAL, val VARCHAR(64), expires_at DECIMAL);"
        for j in $(seq 1 $ROWS); do
            echo "INSERT INTO data VALUES ($j, 'c${i}_r${j}', 1893456000);"
        done
        echo "SELECT * FROM data;"
    } | $CLIENT_BIN 127.0.0.1 $PORT >"$LOG_DIR/t2_$i.log" 2>&1 &
    pids+=($!)
done

for pid in "${pids[@]}"; do wait "$pid" 2>/dev/null || true; done
t_end=$(date +%s%3N)

t2_fail=0
for i in $(seq 1 $N); do
    cnt=$(row_count "$LOG_DIR/t2_$i.log")
    if [[ "$cnt" != "$ROWS" ]]; then
        info "client $i: expected $ROWS rows, got '${cnt:-none}' — see $LOG_DIR/t2_$i.log"
        t2_fail=$((t2_fail + 1))
    fi
done

if [[ $t2_fail -eq 0 ]]; then
    ok "$N/$N clients got correct $ROWS rows  ($(elapsed_ms $t_start $t_end) ms wall time)"
else
    err "$t2_fail/$N clients produced wrong row counts"
fi

# ─────────────────────────────────────────────────────────────────────────────
# TEST 3 — Concurrent reads on a shared table
#
# Goal: verify that concurrent SELECT threads (using shared_lock on
#       tables_mutex) return consistent results and don't corrupt each other.
#       Pre-load 100 rows once, then fire N readers simultaneously.
# ─────────────────────────────────────────────────────────────────────────────
hdr "TEST 3" "Concurrent SELECT — $N readers on shared table (100 rows)"
SHARED_ROWS=100

info "Pre-populating shareddb.snap with $SHARED_ROWS rows..."
{
    echo "CREATE DATABASE shareddb;"
    echo "USE shareddb;"
    echo "CREATE TABLE snap(id DECIMAL, val VARCHAR(64), expires_at DECIMAL);"
    for j in $(seq 1 $SHARED_ROWS); do
        echo "INSERT INTO snap VALUES ($j, 'row${j}', 1893456000);"
    done
} | $CLIENT_BIN 127.0.0.1 $PORT >"$LOG_DIR/t3_setup.log" 2>&1

info "Pre-population done — launching $N concurrent readers..."
pids=()
t_start=$(date +%s%3N)
for i in $(seq 1 $N); do
    {
        echo "USE shareddb;"
        echo "SELECT * FROM snap;"
    } | $CLIENT_BIN 127.0.0.1 $PORT >"$LOG_DIR/t3_$i.log" 2>&1 &
    pids+=($!)
done

for pid in "${pids[@]}"; do wait "$pid" 2>/dev/null || true; done
t_end=$(date +%s%3N)

t3_fail=0
for i in $(seq 1 $N); do
    cnt=$(row_count "$LOG_DIR/t3_$i.log")
    if [[ "$cnt" != "$SHARED_ROWS" ]]; then
        info "reader $i: expected $SHARED_ROWS rows, got '${cnt:-none}' — see $LOG_DIR/t3_$i.log"
        t3_fail=$((t3_fail + 1))
    fi
done

if [[ $t3_fail -eq 0 ]]; then
    ok "$N/$N readers got correct $SHARED_ROWS rows  ($(elapsed_ms $t_start $t_end) ms wall time)"
else
    err "$t3_fail/$N readers got wrong row counts"
fi

# ─────────────────────────────────────────────────────────────────────────────
# TEST 4 — Mixed concurrent read/write workload
#
# Goal: N/2 writers (each inserts into its own table inside mixdb) run at the
#       same time as N/2 readers (SELECT on the pre-existing shareddb.snap).
#       Tests that table-level locks don't deadlock across databases and that
#       readers still see a consistent snapshot during concurrent writes.
# ─────────────────────────────────────────────────────────────────────────────
WRITERS=$((N / 2))
READERS=$((N - WRITERS))   # handles odd N
WRITE_ROWS=25

hdr "TEST 4" "Mixed workload — $WRITERS writers + $READERS readers concurrently"
info "Writers: INSERT $WRITE_ROWS rows into own table in mixdb"
info "Readers: SELECT * FROM shareddb.snap (expect $SHARED_ROWS rows)"

{
    echo "CREATE DATABASE mixdb;"
    echo "USE mixdb;"
} | $CLIENT_BIN 127.0.0.1 $PORT >"$LOG_DIR/t4_setup.log" 2>&1

pids=()
t_start=$(date +%s%3N)

for i in $(seq 1 $WRITERS); do
    {
        echo "USE mixdb;"
        echo "CREATE TABLE wtbl_${i}(id DECIMAL, val VARCHAR(64), expires_at DECIMAL);"
        for j in $(seq 1 $WRITE_ROWS); do
            echo "INSERT INTO wtbl_${i} VALUES ($j, 'w${i}_r${j}', 1893456000);"
        done
        echo "SELECT * FROM wtbl_${i};"
    } | $CLIENT_BIN 127.0.0.1 $PORT >"$LOG_DIR/t4_w_$i.log" 2>&1 &
    pids+=($!)
done

for i in $(seq 1 $READERS); do
    {
        echo "USE shareddb;"
        echo "SELECT * FROM snap;"
    } | $CLIENT_BIN 127.0.0.1 $PORT >"$LOG_DIR/t4_r_$i.log" 2>&1 &
    pids+=($!)
done

for pid in "${pids[@]}"; do wait "$pid" 2>/dev/null || true; done
t_end=$(date +%s%3N)

w_fail=0
for i in $(seq 1 $WRITERS); do
    cnt=$(row_count "$LOG_DIR/t4_w_$i.log")
    if [[ "$cnt" != "$WRITE_ROWS" ]]; then
        info "writer $i: expected $WRITE_ROWS rows, got '${cnt:-none}'"
        w_fail=$((w_fail + 1))
    fi
done

r_fail=0
for i in $(seq 1 $READERS); do
    cnt=$(row_count "$LOG_DIR/t4_r_$i.log")
    if [[ "$cnt" != "$SHARED_ROWS" ]]; then
        info "reader $i: expected $SHARED_ROWS rows, got '${cnt:-none}'"
        r_fail=$((r_fail + 1))
    fi
done

if [[ $w_fail -eq 0 && $r_fail -eq 0 ]]; then
    ok "$WRITERS/$WRITERS writers and $READERS/$READERS readers correct  ($(elapsed_ms $t_start $t_end) ms wall time)"
else
    [[ $w_fail -gt 0 ]] && err "$w_fail/$WRITERS writers got wrong results"
    [[ $r_fail -gt 0 ]] && err "$r_fail/$READERS readers got wrong results"
fi

# ── Summary ──────────────────────────────────────────────────────────────────
TOTAL=$((PASS + FAIL))
echo -e "\n${YELLOW}${BOLD}══════════════════════════════════════════${NC}"
if [[ $FAIL -eq 0 ]]; then
    echo -e "  ${GREEN}${BOLD}All $TOTAL tests passed.${NC}"
else
    echo -e "  ${RED}${BOLD}$FAIL/$TOTAL tests FAILED.${NC}"
fi
echo -e "${YELLOW}${BOLD}══════════════════════════════════════════${NC}\n"

exit $((FAIL > 0 ? 1 : 0))
