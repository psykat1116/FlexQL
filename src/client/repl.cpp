#include <vector>
#include <chrono>
#include <cstdio>
#include <string>
#include <iomanip>
#include <cstring>
#include <cstdlib>
#include <csignal>
#include <iostream>
#include <algorithm>
#include <unistd.h>
#include <poll.h>
#include <termios.h>
#include "common/flexql.h"
#include "common/term_colors.h"

using namespace std;
using namespace term;

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
            printf("%s+", c(BLUE));
            for (size_t i = 0; i < ncols; i++)
            {
                for (size_t j = 0; j < widths[i] + 2; j++)
                    printf("-");
                printf("+");
            }
            printf("%s\n", c(RESET));
        };
        auto isNumber = [](const string &s)
        {
            if (s.empty())
                return false;
            char *end = nullptr;
            strtod(s.c_str(), &end);
            return end && *end == '\0';
        };
        auto printRow = [&](const vector<string> &vals, bool header)
        {
            const char *bar = c(BLUE), *rst = c(RESET);
            printf("%s|%s", bar, rst);
            for (size_t i = 0; i < ncols; i++)
            {
                string v = (i < vals.size()) ? vals[i] : "";
                const char *col = header           ? c(BCYAN)
                                  : v == "NULL"    ? c(DIM)
                                  : isNumber(v)    ? c(YELLOW)
                                                   : "";
                printf(" %s%-*s%s %s|%s", col, (int)widths[i], v.c_str(), rst, bar, rst);
            }
            printf("\n");
        };

        sep();
        printRow(col_names, true);

        sep();
        for (auto &row : rows)
            printRow(row, false);

        sep();
        printf(" %s%zu row(s) in set.%s\n\n", c(DIM), rows.size(), c(RESET));
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

static vector<string> g_history;
static struct termios g_orig_termios;
static bool g_raw_mode_active = false;
static volatile sig_atomic_t g_interrupted = 0;

static void disableRawMode()
{
    if (g_raw_mode_active)
    {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_orig_termios);
        g_raw_mode_active = false;
    }
}

static void enableRawMode()
{
    tcgetattr(STDIN_FILENO, &g_orig_termios);
    struct termios raw = g_orig_termios;
    raw.c_lflag &= ~(ECHO | ICANON);
    raw.c_iflag &= ~(IXON | ICRNL);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
    g_raw_mode_active = true;
    setvbuf(stdin, nullptr, _IONBF, 0);
}

static void handleSigint(int)
{
    g_interrupted = 1;
}

static int waitAndGetchar(int timeout_ms)
{
    struct pollfd pfd{STDIN_FILENO, POLLIN, 0};
    int r = poll(&pfd, 1, timeout_ms);
    if (r <= 0)
        return -1;
    return getchar();
}

static bool readLineRaw(const string &prompt, string &out)
{
    if (!isatty(STDIN_FILENO))
    {
        printf("%s", prompt.c_str());
        fflush(stdout);
        return (bool)getline(cin, out);
    }

    printf("%s", prompt.c_str());
    fflush(stdout);
    enableRawMode();

    size_t cursor = 0;
    string buf, saved_current;
    int hist_pos = (int)g_history.size();

    auto redraw = [&]()
    {
        printf("\r\x1b[K%s%s", prompt.c_str(), buf.c_str());
        for (size_t i = buf.size(); i > cursor; i--)
            printf("\b");
        fflush(stdout);
    };

    bool eof = false;
    while (true)
    {
        int c = waitAndGetchar(-1);
        if (c < 0)
        {
            if (g_interrupted)
            {
                // Safe to do real cleanup here: this is ordinary control
                // flow after poll() returned, not signal-handler context.
                disableRawMode();
                printf("\n");
                exit(130);
            }
            eof = true;
            break;
        }
        if (c == EOF)
        {
            eof = true;
            break;
        }
        if (c == '\r' || c == '\n')
        {
            printf("\n");
            break;
        }
        else if (c == 127 || c == 8) // backspace
        {
            if (cursor > 0)
            {
                buf.erase(cursor - 1, 1);
                cursor--;
                redraw();
            }
        }
        else if (c == 27) // ESC sequence, or a standalone Escape keypress
        {
            int c1 = waitAndGetchar(50);
            if (c1 == '[')
            {
                int c2 = waitAndGetchar(50);
                if (c2 == 'A') // up
                {
                    if (hist_pos > 0)
                    {
                        if (hist_pos == (int)g_history.size())
                            saved_current = buf;
                        hist_pos--;
                        buf = g_history[hist_pos];
                        cursor = buf.size();
                        redraw();
                    }
                }
                else if (c2 == 'B') // down
                {
                    if (hist_pos < (int)g_history.size())
                    {
                        hist_pos++;
                        buf = (hist_pos == (int)g_history.size()) ? saved_current : g_history[hist_pos];
                        cursor = buf.size();
                        redraw();
                    }
                }
                else if (c2 == 'C') // right
                {
                    if (cursor < buf.size())
                    {
                        cursor++;
                        redraw();
                    }
                }
                else if (c2 == 'D') // left
                {
                    if (cursor > 0)
                    {
                        cursor--;
                        redraw();
                    }
                }
                else if (c2 >= '0' && c2 <= '9')
                {
                    waitAndGetchar(50);
                }
            }
        }
        else if (c >= 32 && c < 127) // printable
        {
            buf.insert(cursor, 1, (char)c);
            cursor++;
            redraw();
        }
    }

    disableRawMode();
    out = buf;
    return !eof;
}

static string readSQL(const string &db_prompt)
{
    string buffer;
    bool first = true;
    while (true)
    {
        string db_col = (db_prompt == "none") ? c(DIM) : c(BMAGENTA);
        string prompt = first
                            ? (string(c(BGREEN)) + "flexql" + c(RESET) + " [" + db_col + db_prompt + c(RESET) + "]" + c(BOLD) + "> " + c(RESET))
                            : (string(c(DIM)) + "         ---> " + c(RESET));
        string line;
        if (!readLineRaw(prompt, line))
            return ".exit";
        if (!buffer.empty())
            buffer += ' ';
        buffer += line;
        bool done = (buffer[0] == '.') || (buffer.find(';') != string::npos);
        if (done)
        {
            if (g_history.empty() || g_history.back() != buffer)
                g_history.push_back(buffer);
            return buffer;
        }
        first = false;
    }
}

int main(int argc, char *argv[])
{
    signal(SIGINT, handleSigint);
    atexit(disableRawMode);

    int port = (argc > 2) ? stoi(argv[2]) : 3000;
    const char *host = (argc > 1) ? argv[1] : "127.0.0.1";

    FlexQL *db = nullptr;
    if (flexql_open(host, port, &db) != FLEXQL_OK)
    {
        fprintf(stderr, "%sERROR:%s Cannot connect to FlexQL server at %s:%d\n", e(BRED), e(RESET), host, port);
        return 1;
    }

    // Border color, reset, and a few shorthands for the banner.
    const string B = c(CYAN), R = c(RESET), K = c(BYELLOW), D = c(DIM), H = c(BOLD);
    cout << "\n" << c(BGREEN) << "FlexQL Client" << R << " - connected to "
         << c(BMAGENTA) << host << ":" << port << R << endl;
    cout << B << "+-------------------------------------------------------------------------+" << R << "\n";
    cout << B << "|" << R << H << " Connected to FlexQL server.                                             " << R << B << "|" << R << "\n";
    cout << B << "|" << R << " Type SQL ending with " << K << "';'" << R << " | " << K << ".help" << R << " | " << K << ".exit" << R << "                                " << B << "|" << R << "\n";
    cout << B << "|" << R << "                                                                         " << B << "|" << R << "\n";
    cout << B << "|" << R << H << " Quick start:                                                            " << R << B << "|" << R << "\n";
    cout << B << "|" << R << "   " << K << "SHOW DATABASES;          " << R << "    " << D << "-- list existing databases               " << R << B << "|" << R << "\n";
    cout << B << "|" << R << "   " << K << "CREATE DATABASE Demo_DB; " << R << "    " << D << "-- create a new database                 " << R << B << "|" << R << "\n";
    cout << B << "|" << R << "   " << K << "USE Demo_DB;             " << R << "    " << D << "-- select it before any table operations " << R << B << "|" << R << "\n";
    cout << B << "+-------------------------------------------------------------------------+" << R << "\n\n";

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
            printf("%sGoodbye.%s\n", c(BMAGENTA), c(RESET));
            break;
        }
        if (input == ".help")
        {
            auto section = [](const char *title)
            { printf("\n  %s%s%s\n", c(BGREEN), title, c(RESET)); };
            auto cmd = [](const char *syntax, const char *desc)
            {
                printf("    %s%-49s%s", c(YELLOW), syntax, c(RESET));
                if (*desc)
                    printf(" %s— %s%s", c(DIM), desc, c(RESET));
                printf("\n");
            };

            section("Database management:");
            cmd("CREATE DATABASE name;", "create a new database");
            cmd("SHOW DATABASES;", "list all databases");
            cmd("USE name;", "select active database");
            cmd("SHOW TABLES;", "list tables in current db");
            section("Table commands (require USE first):");
            cmd("CREATE TABLE t (col TYPE [PK] [NOT NULL], ...);", "create table");
            cmd("CREATE TABLE IF NOT EXISTS t (...);", "safe create");
            cmd("INSERT INTO t VALUES (v1,v2,...);", "insert row");
            cmd("INSERT INTO t VALUES (...),(...);", "batch insert");
            cmd("INSERT INTO t VALUES (...) EXPIRY N;", "row expires in N seconds");
            cmd("DELETE FROM t;", "delete all rows");
            cmd("DELETE FROM t WHERE col op val;", "conditional delete");
            cmd("SELECT * FROM t;", "select all");
            cmd("SELECT col1,col2 FROM t;", "select columns");
            cmd("SELECT * FROM t WHERE col op val;", "filter rows");
            cmd("SELECT * FROM t ORDER BY col [ASC|DESC];", "ordered results");
            cmd("SELECT * FROM a INNER JOIN b ON a.x = b.y;", "");
            printf("\n  %sTypes:%s     %sDECIMAL   VARCHAR(n)   TEXT%s\n", c(BGREEN), c(RESET), c(CYAN), c(RESET));
            printf("  %sOperators:%s %s=   !=   <   >   <=   >=%s\n", c(BGREEN), c(RESET), c(CYAN), c(RESET));
            printf("\n  %sDot-commands:%s  %s.help   .exit%s\n\n", c(BGREEN), c(RESET), c(CYAN), c(RESET));
            continue;
        }

        string upper_check = input;
        transform(upper_check.begin(), upper_check.end(), upper_check.begin(), ::toupper);
        size_t u = upper_check.find_first_not_of(" \t");
        bool is_use = (u != string::npos && upper_check.substr(u, 4) == "USE ");

        CallbackState state;
        char *errmsg = nullptr;
        auto q_start = chrono::high_resolution_clock::now();
        int rc = flexql_exec(db, input.c_str(), collectRow, &state, &errmsg);
        auto q_end = chrono::high_resolution_clock::now();
        long long q_elapsed = chrono::duration_cast<chrono::milliseconds>(q_end - q_start).count();
        if (rc != FLEXQL_OK)
        {
            fprintf(stderr, "  %s%s%s\n\n", e(BRED), errmsg ? errmsg : "Unknown error", e(RESET));
            flexql_free(errmsg);
        }
        else
        {
            if (state.header_set)
                state.tp.print();
            else
                printf("%s-> Query OK.%s\n\n", c(BGREEN), c(RESET));

            printf("%s-> (%lld ms)%s\n\n", c(DIM), q_elapsed, c(RESET));
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
