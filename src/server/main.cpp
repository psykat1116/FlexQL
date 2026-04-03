#include <thread>
#include <atomic>
#include <string>
#include <chrono>
#include <csignal>
#include <iomanip>
#include <iostream>
#include <stdexcept>

#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

#include "parser/lexer.h"
#include "parser/parser.h"
#include "query/executor.h"
#include "network/net_utils.h"
#include "common/flexql_protocol.h"

using namespace std;
using namespace chrono;

static atomic<bool> global_stop = false;
static DatabaseManager *global_manager = nullptr;

static void onSignal(int)
{
    cout << "\n[server] Flushing and shutting down...\n";

    global_stop = true;
    if (global_manager)
        global_manager->Flush_DB_Manager();

    exit(0);
}

static void handleClient(int cfd, DatabaseManager &manager)
{
    int flag = 1;
    ::setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));

    int bufsize = 1 << 20;
    ::setsockopt(cfd, SOL_SOCKET, SO_SNDBUF, &bufsize, sizeof(bufsize));
    ::setsockopt(cfd, SOL_SOCKET, SO_RCVBUF, &bufsize, sizeof(bufsize));

    Database *current_db = nullptr;
    while (true)
    {
        MessageHeader hdr;
        if (!Net_Receive_All(cfd, &hdr, sizeof(hdr)))
            break;
        if (hdr.type != MSG_QUERY)
            break;

        string sql(hdr.length, '\0');
        if (hdr.length > 0 && !Net_Receive_All(cfd, sql.data(), hdr.length))
            break;

        string status;
        bool header_sent = false;
        auto t_start = steady_clock::now();
        try
        {
            Lexer lx(sql);
            auto toks = lx.Tokenize();
            Parser pr(move(toks));
            ParsedQuery q = pr.Parse();

            if (q.type == QueryType::CREATE_DATABASE)
            {
                status = manager.Create_Database(q.table_name);
            }
            else if (q.type == QueryType::SHOW_DATABASES)
            {
                auto dbs = manager.List_Databases();

                Net_Send_Message(cfd, MSG_ROW_HEADER, Net_Pack_Fields({"Database"}));
                header_sent = true;
                for (auto &d : dbs)
                    Net_Send_Message(cfd, MSG_ROW, Net_Pack_Fields({d}));

                status = "OK: " + to_string(dbs.size()) + " database(s).";
            }
            else if (q.type == QueryType::USE_DB)
            {
                Database *db = manager.Get_Database(q.table_name);
                if (!db)
                    status = "ERROR: Database '" + q.table_name + "' not found. Create it first with: CREATE DATABASE " + q.table_name + ";";
                else
                {
                    current_db = db;
                    status = "OK: Database changed to '" + q.table_name + "'.";
                }
            }
            else if (q.type == QueryType::SHOW_TABLES)
            {
                if (!current_db)
                {
                    status = "ERROR: No database selected. "
                             "Use 'SHOW DATABASES;' to list available databases, "
                             "'CREATE DATABASE name;' to create one, "
                             "then 'USE name;' to select it.";
                }
                else
                {
                    auto tbls = current_db->Table_Names();
                    Net_Send_Message(cfd, MSG_ROW_HEADER, Net_Pack_Fields({"Tables_in_" + current_db->Get_Name()}));

                    header_sent = true;
                    for (auto &t : tbls)
                        Net_Send_Message(cfd, MSG_ROW, Net_Pack_Fields({t}));

                    status = "OK: " + to_string(tbls.size()) + " table(s).";
                }
            }
            else
            {
                if (!current_db)
                {
                    status = "ERROR: No database selected. "
                             "Use 'SHOW DATABASES;' to list available databases, "
                             "'CREATE DATABASE name;' to create one, "
                             "then 'USE name;' to select it.";
                }
                else
                {
                    status = current_db->Execute(
                        sql, [&](const ResultRow &row) -> bool
                        {
                            if (!header_sent) {
                                Net_Send_Message(cfd, MSG_ROW_HEADER, Net_Pack_Fields(row.column_names));
                                header_sent = true;
                            }

                            Net_Send_Message(cfd, MSG_ROW, Net_Pack_Fields(row.values));
                            return true; });
                }
            }
        }
        catch (exception &e)
        {
            status = string("ERROR: ") + e.what();
        }

        auto t_end = steady_clock::now();
        double elapsed = duration<double>(t_end - t_start).count();
        if (status.rfind("OK", 0) == 0)
            status += " (" + to_string(elapsed).substr(0, to_string(elapsed).find('.') + 4) + "s)";

        Net_Send_Message(cfd, MSG_STATUS, status);
    }

    ::close(cfd);
}

int main(int argc, char *argv[])
{
    int port = (argc > 1) ? stoi(argv[1]) : 3000;
    string data_dir = (argc > 2) ? argv[2] : "./database";

    mkdir(data_dir.c_str(), 0755);

    DatabaseManager mgr(data_dir);
    global_manager = &mgr;

    signal(SIGINT, onSignal);
    signal(SIGTERM, onSignal);
    signal(SIGPIPE, SIG_IGN);

    int sfd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (sfd < 0)
    {
        cerr << "[server] socket() failed\n";
        return 1;
    }

    int opt = 1;
    ::setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons((uint16_t)port);

    if (::bind(sfd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0)
    {
        cerr << "[server] bind() failed on port " << port << "\n";
        return 1;
    }
    ::listen(sfd, 128);

    cout << "\n+------------------------------------------+\n";
    cout << "|  FlexQL Server - PORT " << left << setw(19) << port << "|\n";
    cout << "|  Multithreaded  |  Ctrl + C to stop.     |\n";
    cout << "+------------------------------------------+\n\n";

    while (!global_stop)
    {
        sockaddr_in ca{};
        socklen_t cl = sizeof(ca);
        int cfd = ::accept(sfd, reinterpret_cast<sockaddr *>(&ca), &cl);
        if (cfd < 0)
        {
            if (!global_stop)
                cerr << "[server] accept() failed\n";

            continue;
        }

        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &ca.sin_addr, ip, sizeof(ip));
        cout << "[server] Client " << ip << ":" << ntohs(ca.sin_port) << "  (fd = " << cfd << ")\n";

        thread([cfd, &mgr]()
               { handleClient(cfd, mgr); })
            .detach();
    }

    ::close(sfd);
    return 0;
}
