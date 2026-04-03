#include <chrono>
#include <string>
#include <vector>
#include <sstream>
#include <iostream>
#include "common/flexql.h"

using namespace std;
using namespace chrono;

struct FlexQLError
{
    char *msg = nullptr;
    char **ptr() { return &msg; }
    const char *get() const { return msg ? msg : "unknown error"; }

    ~FlexQLError()
    {
        if (msg)
            flexql_free(msg);
    }
};

static constexpr int INSERT_BATCH_SIZE = 10000;
static constexpr long long DEFAULT_INSERT_ROWS = 10000000LL;

static int Row_Counter_Callback(void *data, int /*argc*/, char ** /*argv*/, char ** /*azColName*/)
{
    if (auto *stats = static_cast<QueryStats *>(data))
        stats->rows++;

    return 0;
}

static bool Run_Execution(FlexQL *db, const string &sql, const string &label)
{
    FlexQLError err;

    auto start = high_resolution_clock::now();
    int rc = flexql_exec(db, sql.c_str(), nullptr, nullptr, err.ptr());
    auto end = high_resolution_clock::now();

    if (rc != FLEXQL_OK)
    {
        cout << "[FAIL] " << label << " -> " << err.get() << "\n";
        return false;
    }

    auto elapsed = duration_cast<milliseconds>(end - start).count();
    cout << "[PASS] " << label << " (" << elapsed << " ms)\n";

    return true;
}

static bool Run_Query(FlexQL *db, const string &sql, const string &label)
{
    FlexQLError err;
    QueryStats stats;

    auto start = high_resolution_clock::now();
    int rc = flexql_exec(db, sql.c_str(), Row_Counter_Callback, &stats, err.ptr());
    auto end = high_resolution_clock::now();

    if (rc != FLEXQL_OK)
    {
        cout << "[FAIL] " << label << " -> " << err.get() << "\n";
        return false;
    }

    auto elapsed = duration_cast<milliseconds>(end - start).count();
    cout << "[PASS] " << label << " | rows=" << stats.rows << " | " << elapsed << " ms\n";

    return true;
}

static bool Query_Rows(FlexQL *db, const string &sql, vector<string> &out_rows)
{
    out_rows.clear();
    auto cb = [](void *data, int argc, char **argv, char ** /*azColName*/) -> int
    {
        auto *rows = static_cast<vector<string> *>(data);
        string row;
        for (int i = 0; i < argc; ++i)
        {
            if (i > 0)
                row += "|";
            row += (argv[i] ? argv[i] : "NULL");
        }

        rows->push_back(move(row));
        return 0;
    };

    FlexQLError err;
    int rc = flexql_exec(db, sql.c_str(), cb, &out_rows, err.ptr());
    if (rc != FLEXQL_OK)
    {
        cout << "[FAIL] " << sql << " -> " << err.get() << "\n";
        return false;
    }

    return true;
}

static bool Assert_Rows_Equal(const string &label, const vector<string> &actual, const vector<string> &expected)
{
    if (actual == expected)
    {
        cout << "[PASS] " << label << "\n";
        return true;
    }

    cout << "[FAIL] " << label << "\n";
    cout << "Expected (" << expected.size() << "):\n";
    for (const auto &r : expected)
        cout << "  " << r << "\n";

    cout << "Actual (" << actual.size() << "):\n";
    for (const auto &r : actual)
        cout << "  " << r << "\n";

    return false;
}

static bool Expect_Query_Failure(FlexQL *db, const string &sql, const string &label)
{
    FlexQLError err;
    int rc = flexql_exec(db, sql.c_str(), nullptr, nullptr, err.ptr());
    if (rc == FLEXQL_OK)
    {
        cout << "[FAIL] " << label << " (expected failure, got success)\n";
        return false;
    }

    cout << "[PASS] " << label << "\n";
    return true;
}

static bool Assert_Row_Count(const string &label, const vector<string> &rows, size_t expected_count)
{
    if (rows.size() == expected_count)
    {
        cout << "[PASS] " << label << "\n";
        return true;
    }

    cout << "[FAIL] " << label << " (expected " << expected_count << ", got " << rows.size() << ")\n";
    return false;
}

static bool Run_Unit_Test(FlexQL *db)
{
    cout << "\n[[...Running Unit Tests...]]\n\n";

    bool all_ok = true;
    int total_tests = 0;
    int failed_tests = 0;

    auto record = [&](bool result)
    {
        total_tests++;
        if (!result)
        {
            all_ok = false;
            failed_tests++;
        }
    };

    record(Run_Execution(db, "CREATE TABLE test_users(id DECIMAL, name VARCHAR(64), balance DECIMAL, expires_at DECIMAL);", "CREATE TABLE test_users"));
    auto insert_test_user = [&](long long id, const string &name, long long balance, long long expires_at) -> bool
    {
        stringstream ss;
        ss << "INSERT INTO test_users VALUES ("
           << id << ", '" << name << "', " << balance << ", " << expires_at << ");";
        return Run_Execution(db, ss.str(), "INSERT test_users id=" + to_string(id));
    };

    record(insert_test_user(1, "Alice", 1200, 1893456000));
    record(insert_test_user(2, "Bob", 450, 1893456000));
    record(insert_test_user(3, "Carol", 2200, 1893456000));
    record(insert_test_user(4, "Dave", 800, 1893456000));

    vector<string> rows;

    bool q0 = Query_Rows(db, "SELECT * FROM test_users;", rows);
    record(q0);
    if (q0)
    {
        record(Assert_Rows_Equal("Basic SELECT * validation", rows, {"1|Alice|1200|1893456000", "2|Bob|450|1893456000", "3|Carol|2200|1893456000", "4|Dave|800|1893456000"}));
    }

    bool q1 = Query_Rows(db, "SELECT name, balance FROM test_users WHERE id = 2;", rows);
    record(q1);
    if (q1)
    {
        record(Assert_Rows_Equal("Single-row value validation", rows, {"Bob|450"}));
    }

    bool q2 = Query_Rows(db, "SELECT name FROM test_users WHERE balance > 1000;", rows);
    record(q2);
    if (q2)
    {
        record(Assert_Rows_Equal("Filtered rows validation", rows, {"Alice", "Carol"}));
    }

    bool q3 = Query_Rows(db, "SELECT id FROM test_users WHERE balance > 5000;", rows);
    record(q3);
    if (q3)
    {
        record(Assert_Row_Count("Empty result-set validation", rows, 0));
    }

    record(Run_Execution(db, "CREATE TABLE test_orders(order_id DECIMAL, user_id DECIMAL, amount DECIMAL, expires_at DECIMAL);", "CREATE TABLE test_orders"));
    record(Run_Execution(db, "INSERT INTO test_orders VALUES (101, 1, 50, 1893456000);", "INSERT test_orders order_id=101"));
    record(Run_Execution(db, "INSERT INTO test_orders VALUES (102, 1, 150, 1893456000);", "INSERT test_orders order_id=102"));
    record(Run_Execution(db, "INSERT INTO test_orders VALUES (103, 3, 500, 1893456000);", "INSERT test_orders order_id=103"));

    bool q7 = Query_Rows(
        db,
        "SELECT test_users.name, test_orders.amount "
        "FROM test_users INNER JOIN test_orders ON test_users.id = test_orders.user_id "
        "WHERE test_orders.amount > 900;",
        rows);

    record(q7);
    if (q7)
    {
        record(Assert_Row_Count("Join with no matches validation", rows, 0));
    }

    record(Expect_Query_Failure(db, "SELECT UNKNOWN_COLUMN FROM test_users;", "Invalid SQL should fail"));
    record(Expect_Query_Failure(db, "SELECT * FROM MISSING_TABLE;", "Missing table should fail"));

    int passed_tests = total_tests - failed_tests;
    cout << "\nUnit Test Summary: " << passed_tests << "/" << total_tests << " passed, " << failed_tests << " failed.\n\n";

    return all_ok;
}

static bool Run_Insert_Benchmark(FlexQL *db, long long target_rows)
{
    if (!Run_Execution(
            db,
            "CREATE TABLE users("
            "id DECIMAL, name VARCHAR(64), "
            "email VARCHAR(64), balance DECIMAL, expires_at DECIMAL);",
            "CREATE TABLE users"))
        return false;

    cout << "\nStarting insertion benchmark for " << target_rows << " rows...\n";
    auto bench_start = high_resolution_clock::now();

    long long inserted = 0;
    long long progress_step = max(1LL, target_rows / 10);
    long long next_progress = progress_step;

    stringstream ss;
    while (inserted < target_rows)
    {
        ss.str("");
        ss.clear();
        ss << "INSERT INTO users VALUES ";

        int in_batch = 0;
        while (in_batch < INSERT_BATCH_SIZE && inserted < target_rows)
        {
            long long id = inserted + 1;
            ss << "(" << id
               << ", 'user" << id << "'"
               << ", 'user" << id << "@mail.com'"
               << ", " << (1000.0 + (id % 10000))
               << ", 1893456000)";

            inserted++;
            in_batch++;
            if (in_batch < INSERT_BATCH_SIZE && inserted < target_rows)
            {
                ss << ",";
            }
        }
        ss << ";";

        FlexQLError err;
        if (flexql_exec(db, ss.str().c_str(), nullptr, nullptr, err.ptr()) != FLEXQL_OK)
        {
            cout << "[FAIL] INSERT users batch -> " << err.get() << "\n";
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

    cout << "[PASS] INSERT benchmark complete\n";
    cout << "Rows inserted: " << target_rows << "\n";
    cout << "Elapsed: " << elapsed << " ms\n";
    cout << "Throughput: " << throughput << " rows/sec\n";

    return true;
}

int main(int argc, char **argv)
{
    FlexQL *db = nullptr;
    bool run_unit_tests_only = false;
    long long insert_rows = DEFAULT_INSERT_ROWS;

    if (argc > 1)
    {
        string arg1 = argv[1];
        if (arg1 == "--unit-test")
        {
            run_unit_tests_only = true;
        }
        else
        {
            insert_rows = stoll(argv[1]);
            if (insert_rows <= 0)
            {
                cout << "Invalid row count. Use a positive integer.\n";
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
        bool ok = Run_Unit_Test(db);
        flexql_close(db);
        return ok ? 0 : 1;
    }

    if (!Run_Insert_Benchmark(db, insert_rows))
    {
        flexql_close(db);
        return 1;
    }

    if (!Run_Unit_Test(db))
    {
        flexql_close(db);
        return 1;
    }

    flexql_close(db);
    return 0;
}