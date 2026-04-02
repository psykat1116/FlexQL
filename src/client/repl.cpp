#include <vector>
#include <cstdio>
#include <string>
#include <cstring>
#include <cstdlib>
#include <iostream>
#include <algorithm>
#include "common/flexql.h"

using namespace std;

typedef struct
{
    vector<string> col_names;
    vector<vector<string>> rows;

    void print() const
    {
        if (col_names.empty())
            return;
        size_t ncols = col_names.size();
        vector<size_t> widths(ncols);
        for (size_t i = 0; i < ncols; i++)
            widths[i] = col_names[i].size();
        for (auto &row : rows)
            for (size_t i = 0; i < row.size() && i < ncols; i++)
                widths[i] = max(widths[i], row[i].size());

        auto sep = [&]()
        {
            printf("+");
            for (size_t i = 0; i < ncols; i++)
            {
                for (size_t j = 0; j < widths[i] + 2; j++)
                    printf("-");
                printf("+");
            }
            printf("\n");
        };
        auto printRow = [&](const vector<string> &vals)
        {
            printf("|");
            for (size_t i = 0; i < ncols; i++)
            {
                string v = (i < vals.size()) ? vals[i] : "";
                printf(" %-*s |", (int)widths[i], v.c_str());
            }
            printf("\n");
        };

        sep();
        printRow(col_names);

        sep();
        for (auto &row : rows)
            printRow(row);

        sep();
        printf("  %zu row(s) in set.\n\n", rows.size());
    }
} TablePrinter;

typedef struct
{
    TablePrinter tp;
    bool header_set = false;
} CallbackState;

static int collectRow(void *arg, int col_count, char **values, char **col_names)
{
    auto *s = static_cast<CallbackState *>(arg);
    if (!s->header_set)
    {
        for (int i = 0; i < col_count; i++)
            s->tp.col_names.push_back(col_names ? col_names[i] : "?");
        s->header_set = true;
    }

    vector<string> row;
    for (int i = 0; i < col_count; i++)
        row.push_back(values && values[i] ? values[i] : "NULL");
    s->tp.rows.push_back(row);

    return 0;
}

static string readSQL(const string &db_prompt)
{
    string buffer;
    bool first = true;
    while (true)
    {
        if (first)
            printf("flexql [%s]> ", db_prompt.c_str());
        else
            printf("         ---> ");
        fflush(stdout);
        string line;
        if (!getline(cin, line))
            return ".exit";
        if (!buffer.empty())
            buffer += ' ';
        buffer += line;
        if (!buffer.empty() && buffer[0] == '.')
            return buffer;
        if (buffer.find(';') != string::npos)
            return buffer;
        first = false;
    }
}

int main(int argc, char *argv[])
{
    const char *host = (argc > 1) ? argv[1] : "127.0.0.1";
    int port = (argc > 2) ? stoi(argv[2]) : 3000;

    printf("\nFlexQL Client  —  connecting to %s:%d ...\n", host, port);

    FlexQL *db = nullptr;
    if (flexql_open(host, port, &db) != FLEXQL_OK)
    {
        fprintf(stderr, "ERROR: Cannot connect to FlexQL server at %s:%d\n", host, port);
        return 1;
    }

    printf("Connected to FlexQL server.\n");
    printf("Type SQL ending with ';'  |  .help  |  .exit\n");
    printf("\nQuick start:\n");
    printf("  SHOW DATABASES;           -- list existing databases\n");
    printf("  CREATE DATABASE mydb;     -- create a new database\n");
    printf("  USE mydb;                 -- select it before any table operations\n\n");

    string current_db = "none";
    while (true)
    {
        string input = readSQL(current_db);
        size_t s = input.find_first_not_of(" \t\r\n");
        if (s == string::npos)
            continue;

        input = input.substr(s);
        if (input == ".exit" || input == ".quit")
        {
            printf("Goodbye.\n");
            break;
        }
        if (input == ".help")
        {
            printf(
                "\n  Database management:\n"
                "    CREATE DATABASE name;                — create a new database\n"
                "    SHOW DATABASES;                      — list all databases\n"
                "    USE name;                            — select active database\n"
                "    SHOW TABLES;                         — list tables in current db\n"
                "\n  Table commands (require USE first):\n"
                "    CREATE TABLE t (col TYPE [PK] [NOT NULL], ...);  — create table\n"
                "    CREATE TABLE IF NOT EXISTS t (...);              — safe create\n"
                "    INSERT INTO t VALUES (v1,v2,...);                — insert row\n"
                "    INSERT INTO t VALUES (...),(...);                — batch insert\n"
                "    INSERT INTO t VALUES (...) EXPIRY N;             — row expires in N seconds\n"
                "    DELETE FROM t;                                   — delete all rows\n"
                "    DELETE FROM t WHERE col op val;                  — conditional delete\n"
                "    SELECT * FROM t;                                 — select all\n"
                "    SELECT col1,col2 FROM t;                         — select columns\n"
                "    SELECT * FROM t WHERE col op val;                — filter rows\n"
                "    SELECT * FROM t ORDER BY col [ASC|DESC];         — ordered results\n"
                "    SELECT * FROM a INNER JOIN b ON a.x = b.y;\n"
                "\n  Types:     DECIMAL   VARCHAR(n)   TEXT\n"
                "  Operators: =   !=   <   >   <=   >=\n"
                "\n  Dot-commands:  .help   .exit\n\n");
            continue;
        }

        string upper_check = input;
        transform(upper_check.begin(), upper_check.end(), upper_check.begin(), ::toupper);
        size_t u = upper_check.find_first_not_of(" \t");
        bool is_use = (u != string::npos && upper_check.substr(u, 4) == "USE ");

        CallbackState state;
        char *errmsg = nullptr;
        int rc = flexql_exec(db, input.c_str(), collectRow, &state, &errmsg);
        if (rc != FLEXQL_OK)
        {
            fprintf(stderr, "  %s\n\n", errmsg ? errmsg : "Unknown error");
            flexql_free(errmsg);
        }
        else
        {
            if (state.header_set)
                state.tp.print();
            else
                printf("  Query OK.\n\n");

            if (is_use)
            {
                string tmp = input;
                transform(tmp.begin(), tmp.end(), tmp.begin(), ::toupper);
                size_t pos = tmp.find("USE ");
                if (pos != string::npos)
                {
                    string after = input.substr(pos + 4);
                    size_t a = after.find_first_not_of(" \t");
                    size_t b = after.find_last_not_of("; \t\r\n");
                    if (a != string::npos && b != string::npos)
                        current_db = after.substr(a, b - a + 1);

                    transform(current_db.begin(), current_db.end(), current_db.begin(), ::toupper);
                }
            }
        }
    }

    flexql_close(db);
    return 0;
}
