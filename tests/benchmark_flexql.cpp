#include <chrono>
#include <thread>
#include <string>
#include <vector>
#include <sstream>
#include <iostream>
#include <algorithm>
#include <functional>
#include "common/flexql.h"

using namespace std;
using namespace std::chrono;

static const int INSERT_BATCH_SIZE = 10000;
static const long long DEFAULT_INSERT_ROWS = 100000LL;

struct FlexQLError
{
    char *msg = nullptr;
    char **ptr() { return &msg; }
    const char *get() const { return msg ? msg : "Unknown Error Occured"; }

    ~FlexQLError()
    {
        if (msg)
            flexql_free(msg);
    }
};

static int count_rows_callback(void *data, int argc, char **argv, char **azColName)
{
    (void)argc;
    (void)argv;
    (void)azColName;
    QueryStats *stats = static_cast<QueryStats *>(data);
    if (stats)
    {
        stats->rows++;
    }
    return 0;
}

static bool run_exec(FlexQL *db, const string &sql, const string &label)
{
    char *errMsg = nullptr;
    auto start = high_resolution_clock::now();
    int rc = flexql_exec(db, sql.c_str(), nullptr, nullptr, &errMsg);
    auto end = high_resolution_clock::now();
    long long elapsed = duration_cast<milliseconds>(end - start).count();

    if (rc != FLEXQL_OK)
    {
        cout << "[FAIL] " << label << " -> " << (errMsg ? errMsg : "unknown error") << "\n";
        if (errMsg)
        {
            flexql_free(errMsg);
        }
        return false;
    }

    cout << "[PASS] " << label << " (" << elapsed << " ms)\n";
    return true;
}

static bool run_query(FlexQL *db, const string &sql, const string &label)
{
    QueryStats stats;
    char *errMsg = nullptr;
    auto start = high_resolution_clock::now();
    int rc = flexql_exec(db, sql.c_str(), count_rows_callback, &stats, &errMsg);
    auto end = high_resolution_clock::now();
    long long elapsed = duration_cast<milliseconds>(end - start).count();

    if (rc != FLEXQL_OK)
    {
        cout << "[FAIL] " << label << " -> " << (errMsg ? errMsg : "unknown error") << "\n";
        if (errMsg)
        {
            flexql_free(errMsg);
        }
        return false;
    }

    cout << "[PASS] " << label << " | rows=" << stats.rows << " | " << elapsed << " ms\n";
    return true;
}

static bool query_rows(FlexQL *db, const string &sql, vector<string> &out_rows)
{
    struct Collector
    {
        vector<string> rows;
    } collector;

    auto cb = [](void *data, int argc, char **argv, char **azColName) -> int
    {
        (void)azColName;
        Collector *c = static_cast<Collector *>(data);
        string row;
        for (int i = 0; i < argc; ++i)
        {
            if (i > 0)
            {
                row += "|";
            }
            row += (argv[i] ? argv[i] : "NULL");
        }
        c->rows.push_back(row);
        return 0;
    };

    char *errMsg = nullptr;
    auto start = high_resolution_clock::now();
    int rc = flexql_exec(db, sql.c_str(), cb, &collector, &errMsg);
    auto end = high_resolution_clock::now();
    long long elapsed = duration_cast<milliseconds>(end - start).count();

    if (rc != FLEXQL_OK)
    {
        cout << "[FAIL] " << sql << " -> " << (errMsg ? errMsg : "unknown error") << "\n";
        if (errMsg)
        {
            flexql_free(errMsg);
        }
        return false;
    }

    cout << "[INFO] " << sql << " | rows=" << collector.rows.size() << " | " << elapsed << " ms\n";
    out_rows = collector.rows;
    return true;
}

static bool assert_rows_equal(const string &label, const vector<string> &actual, const vector<string> &expected)
{
    if (actual == expected)
    {
        cout << "[PASS] " << label << "\n";
        return true;
    }

    cout << "[FAIL] " << label << "\n";
    cout << "Expected (" << expected.size() << "):\n";
    for (const auto &r : expected)
    {
        cout << "  " << r << "\n";
    }
    cout << "Actual (" << actual.size() << "):\n";
    for (const auto &r : actual)
    {
        cout << "  " << r << "\n";
    }
    return false;
}

static bool expect_query_failure(FlexQL *db, const string &sql, const string &label)
{
    char *errMsg = nullptr;
    int rc = flexql_exec(db, sql.c_str(), nullptr, nullptr, &errMsg);
    if (rc == FLEXQL_OK)
    {
        cout << "[FAIL] " << label << " (expected failure, got success)\n";
        return false;
    }
    if (errMsg)
    {
        flexql_free(errMsg);
    }
    cout << "[PASS] " << label << "\n";
    return true;
}

static bool assert_row_count(const string &label, const vector<string> &rows, size_t expected_count)
{
    if (rows.size() == expected_count)
    {
        cout << "[PASS] " << label << "\n";
        return true;
    }

    cout << "[FAIL] " << label << " (expected " << expected_count << ", got " << rows.size() << ")\n";
    return false;
}

static bool run_data_level_unit_tests(FlexQL *db)
{
    cout << "\n[[...Running Unit Tests...]]\n\n";

    bool all_ok = true;
    int total_tests = 0;
    int failed_tests = 0;
    int total_weight = 0;
    int earned_weight = 0;

    auto record = [&](bool result, int weight)
    {
        total_tests++;
        total_weight += weight;
        if (!result)
        {
            all_ok = false;
            failed_tests++;
        }
        else
        {
            earned_weight += weight;
        }
    };

    record(run_exec(db, "CREATE TABLE TEST_USERS(ID DECIMAL, NAME VARCHAR(64), BALANCE DECIMAL, EXPIRES_AT DECIMAL);", "CREATE TABLE TEST_USERS"), 1);
    auto insert_test_user = [&](long long id, const string &name, long long balance, long long expires_at) -> bool
    {
        stringstream ss;
        ss << "INSERT INTO TEST_USERS VALUES ("
           << id << ", '" << name << "', " << balance << ", " << expires_at << ");";
        return run_exec(db, ss.str(), "INSERT TEST_USERS ID=" + to_string(id));
    };

    record(insert_test_user(1, "Eve", 300, 1893456000), 1);
    record(insert_test_user(2, "Frank", 1800, 1893456000), 1);
    record(insert_test_user(3, "Grace", 950, 1893456000), 1);
    record(insert_test_user(4, "Henry", 3500, 1893456000), 1);
    record(insert_test_user(5, "Iris", 600, 1893456000), 1);

    vector<string> rows;

    query_rows(db, "SELECT * FROM TEST_USERS;", rows);
    record(assert_rows_equal("Basic SELECT * validation", rows, {"1|Eve|300|1893456000", "2|Frank|1800|1893456000", "3|Grace|950|1893456000", "4|Henry|3500|1893456000", "5|Iris|600|1893456000"}), 1);

    query_rows(db, "SELECT NAME, BALANCE FROM TEST_USERS WHERE ID = 1;", rows);
    record(assert_rows_equal("Single-row value validation", rows, {"Eve|300"}), 1);

    query_rows(db, "SELECT NAME FROM TEST_USERS WHERE BALANCE > 1500;", rows);
    record(assert_rows_equal("Filtered rows validation", rows, {"Frank", "Henry"}), 1);

    query_rows(db, "SELECT ID FROM TEST_USERS WHERE BALANCE < 100;", rows);
    record(assert_row_count("Empty result-set validation", rows, 0), 1);

    query_rows(db, "SELECT NAME, BALANCE FROM TEST_USERS WHERE BALANCE >= 950;", rows);
    record(assert_rows_equal("High-balance users validation", rows, {"Frank|1800", "Grace|950", "Henry|3500"}), 2);

    query_rows(db, "SELECT NAME FROM TEST_USERS WHERE BALANCE < 700;", rows);
    record(assert_rows_equal("Low-balance users validation", rows, {"Eve", "Iris"}), 2);

    record(run_exec(
               db,
               "CREATE TABLE TEST_ORDERS(ORDER_ID DECIMAL, USER_ID DECIMAL, AMOUNT DECIMAL, EXPIRES_AT DECIMAL);",
               "CREATE TABLE TEST_ORDERS"),
           1);

    record(run_exec(
               db,
               "INSERT INTO TEST_ORDERS VALUES (201, 2, 75, 1893456000);",
               "INSERT TEST_ORDERS ORDER_ID=201"),
           1);

    record(run_exec(
               db,
               "INSERT INTO TEST_ORDERS VALUES (202, 4, 300, 1893456000);",
               "INSERT TEST_ORDERS ORDER_ID=202"),
           1);

    record(run_exec(
               db,
               "INSERT INTO TEST_ORDERS VALUES (203, 4, 1200, 1893456000);",
               "INSERT TEST_ORDERS ORDER_ID=203"),
           1);

    query_rows(
        db,
        "SELECT TEST_USERS.NAME, TEST_ORDERS.AMOUNT "
        "FROM TEST_USERS INNER JOIN TEST_ORDERS ON TEST_USERS.ID = TEST_ORDERS.USER_ID "
        "WHERE TEST_ORDERS.AMOUNT >= 300;",
        rows);
    record(assert_rows_equal("Join with matching rows validation", rows, {"Henry|300", "Henry|1200"}), 4);

    query_rows(
        db,
        "SELECT TEST_USERS.NAME, TEST_ORDERS.AMOUNT "
        "FROM TEST_USERS INNER JOIN TEST_ORDERS ON TEST_USERS.ID = TEST_ORDERS.USER_ID;",
        rows);
    record(assert_rows_equal("Join all matched rows validation", rows, {"Frank|75", "Henry|300", "Henry|1200"}), 4);

    record(expect_query_failure(db, "SELECT GHOST_COL FROM TEST_USERS;", "Invalid column should fail"), 2);
    record(expect_query_failure(db, "SELECT * FROM NO_SUCH_TABLE;", "Missing table should fail"), 2);

    // ── TTL / EXPIRY ─────────────────────────────────────────────────────
    // Verifies the "INSERT ... EXPIRY N" clause: a row must be visible
    // before its TTL elapses and disappear from query results afterwards.
    // This is the same Has_Expired() check the background reaper relies
    // on to find sweep candidates, exercised here purely at the SQL level.
    record(run_exec(db, "CREATE TABLE TEST_TTL(ID DECIMAL, VAL VARCHAR(32));", "CREATE TABLE TEST_TTL"), 1);
    record(run_exec(db, "INSERT INTO TEST_TTL VALUES (1, 'short_lived') EXPIRY 1;", "INSERT TEST_TTL short-lived (EXPIRY 1s)"), 1);
    record(run_exec(db, "INSERT INTO TEST_TTL VALUES (2, 'long_lived');", "INSERT TEST_TTL long-lived (no EXPIRY)"), 1);

    query_rows(db, "SELECT ID FROM TEST_TTL;", rows);
    record(assert_rows_equal("TTL row visible before expiry", rows, {"1", "2"}), 2);

    this_thread::sleep_for(seconds(2));

    query_rows(db, "SELECT ID FROM TEST_TTL;", rows);
    record(assert_rows_equal("TTL row hidden after expiry", rows, {"2"}), 2);

    int passed_tests = total_tests - failed_tests;
    cout << "\nUnit Test Summary: " << passed_tests << "/" << total_tests << " passed, "
         << failed_tests << " failed.\n";
    cout << "Total Marks: " << earned_weight << "/" << total_weight << "\n\n";

    return all_ok;
}

static bool run_batch_insert(FlexQL *db, const string &table, long long target_rows, const function<string(long long)> &row_values, const string &label)
{
    cout << "\nStarting " << label << " for " << target_rows << " rows into " << table << "...\n";
    auto bench_start = high_resolution_clock::now();

    long long inserted = 0;
    long long progress_step = target_rows / 10;
    if (progress_step <= 0)
    {
        progress_step = 1;
    }
    long long next_progress = progress_step;

    while (inserted < target_rows)
    {
        stringstream ss;
        ss << "INSERT INTO " << table << " VALUES ";

        int in_batch = 0;
        while (in_batch < INSERT_BATCH_SIZE && inserted < target_rows)
        {
            long long id = inserted + 1;
            ss << row_values(id);
            inserted++;
            in_batch++;
            if (in_batch < INSERT_BATCH_SIZE && inserted < target_rows)
            {
                ss << ",";
            }
        }
        ss << ";";

        char *errMsg = nullptr;
        if (flexql_exec(db, ss.str().c_str(), nullptr, nullptr, &errMsg) != FLEXQL_OK)
        {
            cout << "[FAIL] " << label << " batch -> " << (errMsg ? errMsg : "unknown error") << "\n";
            if (errMsg)
            {
                flexql_free(errMsg);
            }
            return false;
        }

        if (inserted >= next_progress || inserted == target_rows)
        {
            cout << "Progress: " << inserted << "/" << target_rows << "\n";
            next_progress += progress_step;
        }
    }

    auto bench_end = high_resolution_clock::now();
    long long elapsed = duration_cast<milliseconds>(bench_end - bench_start).count();
    long long throughput = (elapsed > 0) ? (target_rows * 1000LL / elapsed) : target_rows;

    cout << "[PASS] " << label << " complete\n";
    cout << "Rows inserted: " << target_rows << "\n";
    cout << "Elapsed: " << elapsed << " ms\n";
    cout << "Throughput: " << throughput << " rows/sec\n";

    return true;
}

static string bench_user_values(long long id)
{
    stringstream ss;
    ss << "(" << id
       << ", 'user" << id << "'"
       << ", 'user" << id << "@mail.com'"
       << ", " << (1000.0 + (id % 10000))
       << ", 1893456000)";
    return ss.str();
}

static bool run_full_command_benchmark(FlexQL *db, long long target_rows)
{
    bool ok = true;

    cout << "\n[[...Running Full Command Benchmark (" << target_rows << " rows)...]]\n";

    // CREATE TABLE / CREATE TABLE IF NOT EXISTS
    const string create_users_sql = "CREATE TABLE BENCH_USERS(ID DECIMAL PRIMARY KEY, NAME VARCHAR(64), EMAIL VARCHAR(64), BALANCE DECIMAL, EXPIRES_AT DECIMAL);";
    ok &= run_exec(db, create_users_sql, "CREATE TABLE BENCH_USERS");
    ok &= run_exec(db, "CREATE TABLE IF NOT EXISTS BENCH_USERS(ID DECIMAL PRIMARY KEY, NAME VARCHAR(64), EMAIL VARCHAR(64), BALANCE DECIMAL, EXPIRES_AT DECIMAL);", "CREATE TABLE IF NOT EXISTS BENCH_USERS (safe re-create)");

    // INSERT INTO t VALUES (...),(...); batch insert, target_rows rows
    if (!run_batch_insert(db, "BENCH_USERS", target_rows, bench_user_values, "batch INSERT"))
    {
        return false;
    }
    // INSERT INTO t VALUES (v1,v2,...); single row insert
    {
        long long id = target_rows + 1;
        string sql = "INSERT INTO BENCH_USERS VALUES " + bench_user_values(id) + ";";
        ok &= run_exec(db, sql, "INSERT single row (id=" + to_string(id) + ")");
    }
    // INSERT INTO t VALUES (...) EXPIRY N; row with expiry
    {
        long long id = target_rows + 2;
        string sql = "INSERT INTO BENCH_USERS VALUES " + bench_user_values(id) + " EXPIRY 60;";
        ok &= run_exec(db, sql, "INSERT with EXPIRY 60s (id=" + to_string(id) + ")");
    }

    // SELECT * FROM t; select all
    ok &= run_query(db, "SELECT * FROM BENCH_USERS;", "SELECT * FROM BENCH_USERS (full scan)");

    // SELECT col1,col2 FROM t; select columns
    ok &= run_query(db, "SELECT NAME, EMAIL FROM BENCH_USERS;", "SELECT NAME, EMAIL FROM BENCH_USERS (projection)");

    // SELECT * FROM t WHERE col op val; point lookup + range filter
    {
        long long mid_id = target_rows / 2;
        string sql = "SELECT * FROM BENCH_USERS WHERE ID = " + to_string(mid_id) + ";";
        ok &= run_query(db, sql, "SELECT * WHERE ID = " + to_string(mid_id) + " (point lookup)");
    }
    ok &= run_query(db, "SELECT * FROM BENCH_USERS WHERE BALANCE > 5000;", "SELECT * WHERE BALANCE > 5000 (range filter)");

    // SELECT * FROM t ORDER BY col [ASC|DESC]; ordered results
    ok &= run_query(db, "SELECT * FROM BENCH_USERS ORDER BY BALANCE ASC;", "SELECT * ORDER BY BALANCE ASC");
    ok &= run_query(db, "SELECT * FROM BENCH_USERS ORDER BY BALANCE DESC;", "SELECT * ORDER BY BALANCE DESC");

    long long join_rows = target_rows;
    ok &= run_exec(db, "CREATE TABLE BENCH_ORDERS(ORDER_ID DECIMAL PRIMARY KEY, USER_ID DECIMAL, AMOUNT DECIMAL, EXPIRES_AT DECIMAL);", "CREATE TABLE BENCH_ORDERS");

    auto bench_order_values = [target_rows](long long id) -> string
    {
        long long user_id = ((id - 1) % target_rows) + 1;
        long long amount = ((id * 97) % 5000) + 1; // 97 spreads amounts across 1..5000 even for small join_rows
        stringstream ss;
        ss << "(" << id << ", " << user_id << ", " << amount << ", 1893456000)";
        return ss.str();
    };
    if (!run_batch_insert(db, "BENCH_ORDERS", join_rows, bench_order_values, "batch INSERT (join table)"))
    {
        return false;
    }

    // SELECT * FROM a INNER JOIN b ON a.x = b.y; full join + filtered join
    ok &= run_query(
        db,
        "SELECT BENCH_USERS.NAME, BENCH_ORDERS.AMOUNT FROM BENCH_USERS "
        "INNER JOIN BENCH_ORDERS ON BENCH_USERS.ID = BENCH_ORDERS.USER_ID;",
        "INNER JOIN BENCH_USERS x BENCH_ORDERS (full)");
    ok &= run_query(
        db,
        "SELECT BENCH_USERS.NAME, BENCH_ORDERS.AMOUNT FROM BENCH_USERS "
        "INNER JOIN BENCH_ORDERS ON BENCH_USERS.ID = BENCH_ORDERS.USER_ID "
        "WHERE BENCH_ORDERS.AMOUNT > 2500;",
        "INNER JOIN filtered WHERE AMOUNT > 2500");

    // DELETE FROM t WHERE col op val; conditional delete
    ok &= run_exec(db, "DELETE FROM BENCH_ORDERS WHERE AMOUNT < 2500;", "DELETE FROM BENCH_ORDERS WHERE AMOUNT < 2500 (conditional)");

    // DELETE FROM t; delete all rows
    ok &= run_exec(db, "DELETE FROM BENCH_ORDERS;", "DELETE FROM BENCH_ORDERS (all rows)");
    ok &= run_exec(db, "DELETE FROM BENCH_USERS;", "DELETE FROM BENCH_USERS (all rows)");

    return ok;
}

int main(int argc, char **argv)
{
    FlexQL *db = nullptr;
    long long insert_rows = DEFAULT_INSERT_ROWS;
    bool run_unit_tests_only = false;

    if (argc > 1)
    {
        string arg1 = argv[1];
        if (arg1 == "--unit-test")
        {
            run_unit_tests_only = true;
        }
        else
        {
            insert_rows = atoll(argv[1]);
            if (insert_rows <= 0)
            {
                cout << "Invalid row count. Use a positive integer or --unit-test.\n";
                return 1;
            }
        }
    }

    if (flexql_open("127.0.0.1", 3000, &db) != FLEXQL_OK)
    {
        cout << "Cannot open FlexQL\n";
        return 1;
    }

    cout << "Connected to FlexQL\n";
    {
        FlexQLError err;
        flexql_exec(db, "CREATE DATABASE flexqldb;", nullptr, nullptr, err.ptr());
        if (err.msg)
        {
            flexql_free(err.msg);
            err.msg = nullptr;
        }
        if (flexql_exec(db, "USE flexqldb;", nullptr, nullptr, err.ptr()) != FLEXQL_OK)
        {
            cout << "Failed to select benchmark database\n";
            cout << err.get() << "\n";
            flexql_close(db);
            return 1;
        }
    }

    if (run_unit_tests_only)
    {
        bool ok = run_data_level_unit_tests(db);
        flexql_close(db);
        return ok ? 0 : 1;
    }

    cout << "Running full command benchmark plus unit tests...\n";
    cout << "Target row count: " << insert_rows << "\n\n";

    if (!run_full_command_benchmark(db, insert_rows))
    {
        flexql_close(db);
        return 1;
    }

    if (!run_data_level_unit_tests(db))
    {
        flexql_close(db);
        return 1;
    }

    flexql_close(db);
    return 0;
}