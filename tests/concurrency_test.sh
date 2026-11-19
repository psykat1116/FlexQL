#!/usr/bin/env bash
#
# FlexQL threadpool concurrency test
# tests/concurrency_test.sh
#
# Usage: bash tests/concurrency_test.sh [num_clients]
# num_clients is concurrent clients per test scenario [default: 32]

set -uo pipefail

# ── Config ──
PASS=0
FAIL=0
N=${1:-32}

# dedicated port, won't clash with dev server
PORT=3099

# fast TTL reaper interval, just for this test run
REAPER_SECS=2

SERVER_PID=""
SERVER_BIN=./bin/flexql-server
CLIENT_BIN=./bin/flexql-client
LOG_DIR=$(mktemp -d /tmp/flexql_logs_XXXXXX)
DB_DIR=$(mktemp -d /tmp/flexql_conctest_XXXXXX)

# ────────────────
# [Colour Helpers]
# ────────────────
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

# ─────────
# [Cleanup]
# ─────────
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

# ───────
# [Build]
# ───────
hdr "BUILD" "Compiling server and client..."
if ! make -j"$(nproc)" bin/flexql-server bin/flexql-client -s 2>&1; then
    echo "Build failed, aborting." >&2
    exit 1
fi
info "Build OK"

# ────────────
# Start server
# ────────────
hdr "SETUP" "Starting FlexQL server on port $PORT (TTL reaper every ${REAPER_SECS}s)..."
stdbuf -oL -eL $SERVER_BIN $PORT "$DB_DIR" "$REAPER_SECS" >"$LOG_DIR/server.log" 2>&1 &
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
    echo "Server did not come up, aborting." >&2
    exit 1
fi
info "Server ready  (PID $SERVER_PID, DB dir: $DB_DIR)"

# ─────────────────────────────────────────────────
# [TEST 1] [Concurrent CREATE DATABASE] [Same Name]
# ─────────────────────────────────────────────────
hdr "TEST 1" "Concurrent CREATE DATABASE, same name, $N clients"
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
    ok "Server alive after $responded/$N concurrent CREATE DATABASE calls no crash/deadlock"
else
    err "Server unreachable after concurrent CREATE DATABASE ($responded/$N responded)"
fi

# ──────────────────────────────────────
# [TEST 2] [Concurrent isolated writes] 
# [each client owns a separate database]
# ──────────────────────────────────────
hdr "TEST 2" "Concurrent isolated writes, $N clients, each own database"
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
        info "client $i: expected $ROWS rows, got '${cnt:-none}' see $LOG_DIR/t2_$i.log"
        t2_fail=$((t2_fail + 1))
    fi
done

if [[ $t2_fail -eq 0 ]]; then
    ok "$N/$N clients got correct $ROWS rows  ($(elapsed_ms $t_start $t_end) ms wall time)"
else
    err "$t2_fail/$N clients produced wrong row counts"
fi

# ─────────────────────────────────────────────
# [TEST 3] [Concurrent reads on a shared table]
# ─────────────────────────────────────────────
hdr "TEST 3" "Concurrent SELECT $N readers on shared table [100 rows]"
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

info "Pre-population done launching $N concurrent readers..."
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
        info "reader $i: expected $SHARED_ROWS rows, got '${cnt:-none}' see $LOG_DIR/t3_$i.log"
        t3_fail=$((t3_fail + 1))
    fi
done

if [[ $t3_fail -eq 0 ]]; then
    ok "$N/$N readers got correct $SHARED_ROWS rows  ($(elapsed_ms $t_start $t_end) ms wall time)"
else
    err "$t3_fail/$N readers got wrong row counts"
fi

# ───────────────────────────────────────────────
# [TEST 4] [Mixed concurrent read/write workload]
# ───────────────────────────────────────────────
WRITERS=$((N / 2))
READERS=$((N - WRITERS))   # handles odd N
WRITE_ROWS=25

hdr "TEST 4" "Mixed workload $WRITERS writers + $READERS readers concurrently"
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

# ──────────────────────────────────────
# [TEST 5] [TTL reaper background sweep]
# ──────────────────────────────────────
TTL_SECS=1
WAIT_SECS=$((REAPER_SECS * 2 + TTL_SECS + 1))

hdr "TEST 5" "TTL reaper background sweep of expired rows"
info "INSERT 1 row with EXPIRY ${TTL_SECS}s + 1 row with no expiry into ttldb.ttl_data"

{
    echo "CREATE DATABASE ttldb;"
    echo "USE ttldb;"
    echo "CREATE TABLE ttl_data(id DECIMAL, val VARCHAR(32));"
    echo "INSERT INTO ttl_data VALUES (1, 'short_lived') EXPIRY ${TTL_SECS};"
    echo "INSERT INTO ttl_data VALUES (2, 'long_lived');"
} | $CLIENT_BIN 127.0.0.1 $PORT >"$LOG_DIR/t5_setup.log" 2>&1

info "Waiting ${WAIT_SECS}s for the TTL to elapse and the reaper to sweep..."
sleep "$WAIT_SECS"

{
    echo "USE ttldb;"
    echo "SELECT * FROM ttl_data;"
} | $CLIENT_BIN 127.0.0.1 $PORT >"$LOG_DIR/t5_select.log" 2>&1

remaining=$(row_count "$LOG_DIR/t5_select.log")
reaper_swept=false
grep -q '\[reaper\] purged' "$LOG_DIR/server.log" 2>/dev/null && reaper_swept=true

if [[ "$remaining" == "1" && "$reaper_swept" == true ]]; then
    ok "expired row gone (1/2 remain) and server log confirms a background sweep ran"
elif [[ "$remaining" == "1" ]]; then
    err "expired row filtered from SELECT, but no '[reaper] purged' line in server log sweep didn't run"
else
    err "expected 1 remaining row, got '${remaining:-none}' see $LOG_DIR/t5_select.log"
fi

# ──────────────────────────────────────────
# [TEST 6] [Oversized query length rejected]
# ──────────────────────────────────────────
# The real client refuses to send oversized queries, so this test writes a
# raw MessageHeader over /dev/tcp: type = MSG_QUERY (1), length = 0xFFFFFFF0,
# both little-endian uint32. No payload follows.
OVERSIZE_CONNS=20
hdr "TEST 6" "Oversized query length, $OVERSIZE_CONNS raw connections"
info "Each sends a header claiming a ~4 GiB query; server must reply ERROR, close, and not leak fds"

send_oversized() {
    # Prints whatever the server sends back before closing (or times out).
    timeout 5 bash -c '
        exec 3<>/dev/tcp/127.0.0.1/'"$PORT"' || exit 1
        printf "\x01\x00\x00\x00\xf0\xff\xff\xff" >&3
        # Skip the 8-byte binary reply header; keep the status text.
        cat <&3 | tail -c +9
    '
}

fd_count() { ls "/proc/$SERVER_PID/fd" 2>/dev/null | wc -l; }

# Let earlier tests' connections finish closing before the baseline.
sleep 1
fds_before=$(fd_count)

reply=$(send_oversized)
send_rc=$?
for _ in $(seq 2 $OVERSIZE_CONNS); do
    send_oversized >/dev/null
done
sleep 1
fds_after=$(fd_count)

if [[ $send_rc -eq 124 ]]; then
    err "server did not close the connection within 5s"
elif [[ "$reply" != *"ERROR: Query too large"* ]]; then
    err "expected 'ERROR: Query too large' reply, got: '${reply:-<nothing>}'"
else
    ok "server replied '${reply%%(*}...' and closed the connection"
fi

if [[ "$fds_after" -le "$fds_before" ]]; then
    ok "no fd leak ($fds_before open fds before, $fds_after after $OVERSIZE_CONNS rejected connections)"
else
    err "fd leak: $fds_before open fds before, $fds_after after $OVERSIZE_CONNS rejected connections"
fi

if printf 'SHOW DATABASES;\n' | $CLIENT_BIN 127.0.0.1 $PORT >/dev/null 2>&1; then
    ok "server still serves normal clients after oversized requests"
else
    err "server unreachable after oversized requests"
fi

# ─────── 
# Summary 
# ───────
TOTAL=$((PASS + FAIL))
echo -e "\n${YELLOW}${BOLD}══════════════════════════════════════════${NC}"
if [[ $FAIL -eq 0 ]]; then
    echo -e "  ${GREEN}${BOLD}All $TOTAL tests passed.${NC}"
else
    echo -e "  ${RED}${BOLD}$FAIL/$TOTAL tests FAILED.${NC}"
fi
echo -e "${YELLOW}${BOLD}══════════════════════════════════════════${NC}\n"

exit $((FAIL > 0 ? 1 : 0))
