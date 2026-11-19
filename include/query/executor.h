#pragma once

#include <memory>
#include <vector>
#include <string>
#include <unordered_map>
#include <mutex>
#include <shared_mutex>

#include "common/types.h"
#include "parser/parser.h"
#include "storage/table_manager.h"

using namespace std;

class Database
{
public:
    Database(const string &root_dir, const string &db_name);

    void Flush_DB();
    int Purge_Expired();
    vector<string> Table_Names();
    string Get_Name() const { return database_name; }
    TableManager *Get_Table(const string &name);
    string Execute(const string &sql, RowCB cb = nullptr);

private:
    string database_name;
    string database_directory;
    mutable shared_mutex tables_mutex;
    unordered_map<string, unique_ptr<TableManager>> tables;

    string Execute_Create(const ParsedQuery &q);
    string Execute_Insert(const ParsedQuery &q);
    string Execute_Delete(const ParsedQuery &q);
    string Execute_Select(const ParsedQuery &q, RowCB cb);
};

class DatabaseManager
{
public:
    explicit DatabaseManager(const string &root_dir);

    void Flush_DB_Manager();
    int Purge_Expired();
    vector<string> List_Databases();
    Database *Get_Database(const string &name);
    string Create_Database(const string &name);

private:
    mutex database_mutex;
    string root_directory;
    unordered_map<string, unique_ptr<Database>> databases;
};

bool Field_Matches(const Field &f, const string &op, const string &val);
ResultRow Project_Row(const Row &row, const TableSchema &sc, const vector<ColumnReference> &sel_cols, bool sel_star, const string &tbl_prefix = "");