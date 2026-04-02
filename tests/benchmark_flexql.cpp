#include <chrono>
#include <string>
#include <vector>
#include <sstream>
#include <iostream>
#include "common/flexql.h"

using namespace std;
using namespace std::chrono;

static const int INSERT_BATCH_SIZE = 10000;
static const long long DEFAULT_INSERT_ROWS = 10000000LL;

typedef struct
{
    long long rows = 0;
} QueryStats;

static int count_rows_callback(void *data, int argc, char **argv, char **azColName)
{
    (void)argc;
    (void)argv;
    (void)azColName;

    QueryStats *stats = static_cast<QueryStats *>(data);
    if (stats)
        stats->rows++;

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
            flexql_free(errMsg);
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
            flexql_free(errMsg);
        return false;
    }

    cout << "[PASS] " << label << " | rows=" << stats.rows << " | " << elapsed << " ms\n";
    return true;
}

static bool run_insert_benchmark(FlexQL *db, long long target_rows)
{
    if (!run_exec(db,
                  "CREATE TABLE users("
                  "id DECIMAL, name VARCHAR(64), "
                  "email VARCHAR(64), balance DECIMAL, expires_at DECIMAL);",
                  "CREATE TABLE users"))
        return false;

    cout << "\nStarting insertion benchmark for " << target_rows << " rows...\n";
    auto bench_start = high_resolution_clock::now();

    long long inserted = 0;
    long long progress_step = target_rows / 10;
    if (progress_step <= 0)
        progress_step = 1;
    long long next_progress = progress_step;

    while (inserted < target_rows)
    {
        stringstream ss;
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
                ss << ",";
        }
        ss << ";";

        char *errMsg = nullptr;
        if (flexql_exec(db, ss.str().c_str(), nullptr, nullptr, &errMsg) != FLEXQL_OK)
        {
            cout << "[FAIL] INSERT users batch -> "
                 << (errMsg ? errMsg : "unknown error") << "\n";
            if (errMsg)
                flexql_free(errMsg);
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
        insert_rows = atoll(argv[1]);
        if (insert_rows <= 0)
        {
            cout << "Invalid row count. Use a positive integer.\n";
            return 1;
        }
    }

    if (flexql_open("127.0.0.1", 3000, &db) != FLEXQL_OK)
    {
        cout << "Cannot open FlexQL\n";
        return 1;
    }

    cout << "Connected to FlexQL\n";
    {
        char *errMsg = nullptr;
        flexql_exec(db, "CREATE DATABASE flexqldb;", nullptr, nullptr, &errMsg);
        if (errMsg)
            flexql_free(errMsg);
        errMsg = nullptr;
        if (flexql_exec(db, "USE flexqldb;", nullptr, nullptr, &errMsg) != FLEXQL_OK)
        {
            cout << "Failed to select benchmark database\n";
            if (errMsg)
            {
                cout << errMsg << "\n";
                flexql_free(errMsg);
            }
            flexql_close(db);
            return 1;
        }

        if (errMsg)
            flexql_free(errMsg);
    }

    if (!run_insert_benchmark(db, insert_rows))
    {
        flexql_close(db);
        return 1;
    }

    flexql_close(db);
    return 0;
}
