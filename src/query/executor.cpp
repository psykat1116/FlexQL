#include "parser/lexer.h"
#include "query/executor.h"
#include "expiration/expiry.h"

#include <fstream>
#include <cstring>
#include <stdexcept>
#include <climits>
#include <algorithm>
#include <unordered_map>

#include <dirent.h>
#include <sys/stat.h>

using namespace std;

bool Field_Matches(const Field &f, const string &op, const string &val)
{
    if (f.is_null)
        return false;

    switch (f.column_type)
    {
    case ColType::INT:
    {
        int32_t v = stoi(val);
        if (op == "=")
            return f.integer_value == v;
        if (op == "!=")
            return f.integer_value != v;
        if (op == "<")
            return f.integer_value < v;
        if (op == ">")
            return f.integer_value > v;
        if (op == "<=")
            return f.integer_value <= v;
        if (op == ">=")
            return f.integer_value >= v;

        break;
    }
    case ColType::DECIMAL:
    {
        double v = stod(val);
        if (op == "=")
            return f.double_value == v;
        if (op == "!=")
            return f.double_value != v;
        if (op == "<")
            return f.double_value < v;
        if (op == ">")
            return f.double_value > v;
        if (op == "<=")
            return f.double_value <= v;
        if (op == ">=")
            return f.double_value >= v;

        break;
    }
    case ColType::VARCHAR:
    {
        if (op == "=")
            return f.string_value == val;
        if (op == "!=")
            return f.string_value != val;
        if (op == "<")
            return f.string_value < val;
        if (op == ">")
            return f.string_value > val;
        if (op == "<=")
            return f.string_value <= val;
        if (op == ">=")
            return f.string_value >= val;
        break;
    }
    case ColType::BOOLEAN:
    {
        bool v = (val == "1" || val == "true" || val == "TRUE" || val == "yes");
        if (op == "=")
            return f.boolean_value == v;
        if (op == "!=")
            return f.boolean_value != v;

        break;
    }
    }

    return false;
}

ResultRow Project_Row(const Row &row, const TableSchema &sc, const vector<ColumnReference> &sel_cols, bool sel_star, const string &tbl_prefix)
{
    ResultRow res;
    if (sel_star)
    {
        for (uint32_t i = 0; i < sc.num_cols; i++)
        {
            string cn = sc.cols[i].name;
            if (!tbl_prefix.empty())
                cn = tbl_prefix + "." + cn;
            res.column_names.push_back(cn);
            res.values.push_back(row.fields[i].Convert_To_String());
        }
    }
    else
    {
        for (auto &cr : sel_cols)
        {
            if (!cr.tbl.empty() && !tbl_prefix.empty())
            {
                string a = cr.tbl, b = tbl_prefix;
                if (a != b)
                    continue;
            }

            int idx = sc.Find_Column(cr.col);
            if (idx < 0)
                continue;

            res.column_names.push_back(cr.col);
            res.values.push_back(row.fields[idx].Convert_To_String());
        }
    }

    return res;
}

Database::Database(const string &root_dir, const string &db_name) : database_name(db_name), database_directory(root_dir + "/" + db_name)
{
    mkdir(database_directory.c_str(), 0755);
}

void Database::Flush_DB()
{
    shared_lock lk(tables_mutex);
    for (auto &[n, t] : tables)
    {
        lock_guard tl(t->table_mutex);
        t->Flush_Table();
    }
}

int Database::Purge_Expired()
{
    shared_lock lk(tables_mutex);
    int total = 0;
    for (auto &[n, t] : tables)
    {
        lock_guard tl(t->table_mutex);
        total += t->Purge_Expired();
    }
    return total;
}

TableManager *Database::Get_Table(const string &name)
{
    {
        shared_lock lk(tables_mutex);
        auto it = tables.find(name);
        if (it != tables.end())
            return it->second.get();
    }

    ifstream sf(database_directory + "/" + name + ".schema", ios::binary);
    if (!sf.is_open())
        return nullptr;

    TableSchema sc;
    sf.read(reinterpret_cast<char *>(&sc), sizeof(sc));

    unique_lock lk(tables_mutex);
    auto it = tables.find(name);
    if (it != tables.end())
        return it->second.get();

    tables[name] = make_unique<TableManager>(database_directory, name, sc, false);
    return tables[name].get();
}

vector<string> Database::Table_Names()
{
    shared_lock lk(tables_mutex);
    vector<string> names;
    DIR *d = opendir(database_directory.c_str());
    if (!d)
        return names;

    struct dirent *e;
    while ((e = readdir(d)) != nullptr)
    {
        string fn = e->d_name;
        if (fn.size() > 7 && fn.substr(fn.size() - 7) == ".schema")
            names.push_back(fn.substr(0, fn.size() - 7));
    }

    closedir(d);
    sort(names.begin(), names.end());

    return names;
}

string Database::Execute_Create(const ParsedQuery &q)
{
    string tn = q.table_name;
    {
        shared_lock lk(tables_mutex);
        if (tables.count(tn))
        {
            if (q.if_not_exists)
                return "OK: Table '" + tn + "' already exists (ignored).";
            return "ERROR: Table '" + tn + "' already exists.";
        }
    }
    ifstream sf(database_directory + "/" + tn + ".schema", ios::binary);
    if (sf.is_open())
    {
        if (q.if_not_exists)
            return "OK: Table '" + tn + "' already exists (ignored).";
        return "ERROR: Table '" + tn + "' already exists on disk.";
    }

    TableSchema sc;
    memset(&sc, 0, sizeof(sc));
    strncpy(sc.table_name, tn.c_str(), MAX_NAME - 1);
    sc.num_cols = (uint32_t)q.col_defs.size();
    for (uint32_t i = 0; i < sc.num_cols; i++)
    {
        auto &cd = q.col_defs[i];
        strncpy(sc.cols[i].name, cd.name.c_str(), MAX_NAME - 1);
        sc.cols[i].is_primary_key = cd.pk;
        sc.cols[i].not_null = cd.notnull;
        if (cd.type == "INT")
        {
            sc.cols[i].type = ColType::INT;
            sc.cols[i].max_size = 8;
        }
        else if (cd.type == "DECIMAL")
        {
            sc.cols[i].type = ColType::DECIMAL;
            sc.cols[i].max_size = 8;
        }
        else if (cd.type == "BOOLEAN")
        {
            sc.cols[i].type = ColType::BOOLEAN;
            sc.cols[i].max_size = 1;
        }
        else if (cd.type == "TEXT")
        {
            sc.cols[i].type = ColType::VARCHAR;
            sc.cols[i].max_size = 65535;
        }
        else
        {
            sc.cols[i].type = ColType::VARCHAR;
            sc.cols[i].max_size = cd.vsz > 0 ? cd.vsz : 255;
        }
    }

    unique_lock lk(tables_mutex);
    tables[tn] = make_unique<TableManager>(database_directory, tn, sc, true);
    return "OK: Table '" + tn + "' created.";
}

string Database::Execute_Insert(const ParsedQuery &q)
{
    string tn = q.table_name;
    TableManager *tm = Get_Table(tn);
    if (!tm)
        return "ERROR: Table '" + tn + "' not found.";

    auto &sc = tm->schema;
    lock_guard tl(tm->table_mutex);

    int inserted = 0;
    for (auto &vals : q.inserted_rows)
    {
        if ((uint32_t)vals.size() != sc.num_cols)
            return "ERROR: Expected " + to_string(sc.num_cols) + " values, got " + to_string(vals.size()) + ".";

        Row row;
        for (uint32_t i = 0; i < sc.num_cols; i++)
            row.fields.push_back(Make_Field(vals[i], sc.cols[i]));

        row.expiry_timestamp = Make_Expiry(q.expiry_secs);
        tm->Insert_Row(row);
        inserted++;
    }

    return "OK: " + to_string(inserted) + " row(s) inserted.";
}

static bool Row_Matches_All(const Row &row, const TableSchema &sc, const vector<WhereClause> &wlist)
{
    for (const auto &w : wlist)
    {
        int ci = sc.Find_Column(w.col.col);
        if (ci < 0)
            return false;
        if (!Field_Matches(row.fields[ci], w.op, w.val))
            return false;
    }
    return true;
}

string Database::Execute_Select(const ParsedQuery &q, RowCB cb)
{
    string tn = q.from_table;
    TableManager *tm = Get_Table(tn);
    if (!tm)
        return "ERROR: Table '" + tn + "' not found.";

    if (!q.select_star && !q.has_join)
    {
        for (auto &cr : q.selected_columns)
        {
            if (!cr.tbl.empty())
                continue;

            int idx = tm->schema.Find_Column(cr.col);
            if (idx < 0)
                return "ERROR: Unknown column '" + cr.col +
                       "' in table '" + tn + "'.";
        }
    }

    auto applyOrder = [&](vector<ResultRow> &rows)
    {
        if (!q.has_order)
            return;
        bool desc = q.order.desc;
        string col = q.order.col.col;
        stable_sort(rows.begin(), rows.end(), [&](const ResultRow &a, const ResultRow &b)
                    {
            int idx = -1;
            for (int i = 0; i < (int)a.column_names.size(); i++)
            {
                string cn = a.column_names[i];
                if (cn == col || cn.find('.' + col) != string::npos)
                {
                    idx = i;
                    break;
                }
            }
            if (idx < 0)
                return false;
                        
            const string &va = a.values[idx];
            const string &vb = b.values[idx];
            try
            {
                double da = stod(va), db = stod(vb);
                return desc ? da > db : da < db;
            }
            catch (...){}

            return desc ? va > vb : va < vb; });
    };

    if (q.has_join)
    {
        string jn = q.join.jtbl;
        TableManager *jt = Get_Table(jn);
        if (!jt)
            return "ERROR: Table '" + jn + "' not found.";

        vector<ResultRow> results;

        lock_guard tl(tm->table_mutex);
        lock_guard jl(jt->table_mutex);

        vector<int> left_idx, right_idx;
        if (q.join.has_on)
        {
            int li = tm->schema.Find_Column(q.join.left.col);
            int ri = jt->schema.Find_Column(q.join.right.col);
            if (li >= 0 && ri >= 0)
            {
                left_idx.push_back(li);
                right_idx.push_back(ri);
            }
        }
        else
        {
            for (uint32_t i = 0; i < tm->schema.num_cols; i++)
            {
                int ri = jt->schema.Find_Column(tm->schema.cols[i].name);
                if (ri >= 0)
                {
                    left_idx.push_back((int)i);
                    right_idx.push_back(ri);
                }
            }
            if (left_idx.empty())
                return "ERROR: No common column found between '" + tn + "' and '" + jn + "' to join on. Specify ON explicitly.";
        }

        if (!left_idx.empty())
        {
            auto make_key = [](const Row &row, const vector<int> &idxs) -> string
            {
                string k;
                for (int idx : idxs)
                {
                    k += row.fields[idx].Convert_To_String();
                    k += '\x1f';
                }
                return k;
            };

            unordered_map<string, vector<Row>> right_index;
            jt->Scan([&](const Row &rr) -> bool
                     {
                right_index[make_key(rr, right_idx)].push_back(rr);
                return true; });

            tm->Scan([&](const Row &lr) -> bool
                     {
                auto it = right_index.find(make_key(lr, left_idx));
                if (it == right_index.end())
                    return true;

                for (const Row &rr : it->second)
                {
                    if (q.has_where)
                    {
                        bool ok = true;
                        for (const auto &w : q.where_list)
                        {
                            string wt   = w.col.tbl;
                            string wcol = w.col.col;
                            int ci = tm->schema.Find_Column(wcol);
                            if (ci >= 0 && (wt == tn || wt.empty()))
                                if (!Field_Matches(lr.fields[ci], w.op, w.val)) { ok = false; break; }
                            ci = jt->schema.Find_Column(wcol);
                            if (ci >= 0 && (wt == jn || wt.empty()))
                                if (!Field_Matches(rr.fields[ci], w.op, w.val)) { ok = false; break; }
                        }
                        if (!ok)
                            continue;
                    }

                    ResultRow res;
                    auto rl  = Project_Row(lr, tm->schema, q.selected_columns, q.select_star, tn);
                    auto rr2 = Project_Row(rr, jt->schema, q.selected_columns, q.select_star, jn);

                    for (auto &c : rl.column_names)  res.column_names.push_back(c);
                    for (auto &v : rl.values)        res.values.push_back(v);
                    for (auto &c : rr2.column_names) res.column_names.push_back(c);
                    for (auto &v : rr2.values)       res.values.push_back(v);

                    results.push_back(move(res));
                }

                return true; });
        }

        applyOrder(results);
        for (auto &r : results)
            if (!cb(r))
                break;

        return "OK: " + to_string(results.size()) + " row(s) returned.";
    }

    int rows = 0;
    int pk_col = tm->schema.Primary_Key_Column();
    const char *pk_name = tm->schema.cols[pk_col].name;

    struct SortRow
    {
        string sort_key;
        bool sort_key_numeric = false;
        double sort_key_dbl = 0.0;
        ResultRow rr;
    };

    auto emit_sorted = [&](vector<SortRow> &buf)
    {
        if (q.has_order)
        {
            bool desc = q.order.desc;
            stable_sort(buf.begin(), buf.end(), [&](const SortRow &a, const SortRow &b)
                        {
                if (a.sort_key_numeric && b.sort_key_numeric)
                    return desc ? a.sort_key_dbl > b.sort_key_dbl
                                : a.sort_key_dbl < b.sort_key_dbl;
                return desc ? a.sort_key > b.sort_key : a.sort_key < b.sort_key; });
        }
        for (auto &sr : buf)
        {
            rows++;
            if (!cb(sr.rr))
                break;
        }
    };

    int sort_col_idx = -1;
    if (q.has_order)
        sort_col_idx = tm->schema.Find_Column(q.order.col.col);

    auto make_sort_row = [&](const Row &row) -> SortRow
    {
        SortRow sr;
        sr.rr = Project_Row(row, tm->schema, q.selected_columns, q.select_star);
        if (sort_col_idx >= 0 && sort_col_idx < (int)row.fields.size())
        {
            sr.sort_key = row.fields[sort_col_idx].Convert_To_String();
            try
            {
                sr.sort_key_dbl = stod(sr.sort_key);
                sr.sort_key_numeric = true;
            }
            catch (...)
            {
            }
        }
        return sr;
    };

    int32_t lo = INT32_MIN, hi = INT32_MAX;
    bool lo_inc = true, hi_inc = true;
    bool has_eq = false;
    double eq_val = 0.0;
    ColType pk_type = tm->schema.cols[pk_col].type;
    bool pk_is_decimal = (pk_type == ColType::DECIMAL);
    bool all_on_pk = q.has_where && !q.where_list.empty() && (pk_type == ColType::INT || pk_is_decimal);

    if (all_on_pk)
    {
        for (const auto &w : q.where_list)
        {
            if (!w.col.tbl.empty() && strcasecmp(w.col.tbl.c_str(), tn.c_str()) != 0)
            {
                all_on_pk = false;
                break;
            }
            if (strcasecmp(w.col.col.c_str(), pk_name) != 0)
            {
                all_on_pk = false;
                break;
            }

            double vd = stod(w.val);
            int32_t v = (int32_t)vd;
            if (w.op == "=")
            {
                has_eq = true;
                eq_val = vd;
                lo = hi = v;
                lo_inc = hi_inc = true;
                break;
            }
            else if (w.op == "!=")
            {
                all_on_pk = false;
                break;
            }
            else if (pk_is_decimal)
            {
                all_on_pk = false;
                break;
            }
            else if (w.op == ">")
            {
                if (v >= lo)
                {
                    lo = v;
                    lo_inc = false;
                }
            }
            else if (w.op == ">=")
            {
                if (v > lo)
                {
                    lo = v;
                    lo_inc = true;
                }
            }
            else if (w.op == "<")
            {
                if (v <= hi)
                {
                    hi = v;
                    hi_inc = false;
                }
            }
            else if (w.op == "<=")
            {
                if (v < hi)
                {
                    hi = v;
                    hi_inc = true;
                }
            }
        }
    }

    if (all_on_pk && has_eq && !q.has_order)
    {
        lock_guard tl(tm->table_mutex);
        Row row;
        bool found = tm->Find_By_FK(lo, row);
        bool trustworthy = found && (!pk_is_decimal || row.fields[pk_col].double_value == eq_val);
        if (trustworthy)
        {
            rows++;
            cb(Project_Row(row, tm->schema, q.selected_columns, q.select_star));
            return "OK: " + to_string(rows) + " row(s) returned. [B+ Tree point lookup]";
        }
        if (!pk_is_decimal)
        {
            return "OK: 0 row(s) returned.";
        }
    }

    if (all_on_pk && !has_eq && (lo != INT32_MIN || hi != INT32_MAX))
    {
        vector<SortRow> buf;
        {
            lock_guard tl(tm->table_mutex);
            for (Row &row : tm->Find_By_Range(lo, lo_inc, hi, hi_inc))
                buf.push_back(make_sort_row(row));
        }
        emit_sorted(buf);
        return "OK: " + to_string(rows) + " row(s) returned. [B+ Tree range scan]";
    }

    {
        vector<SortRow> buf;
        {
            lock_guard tl(tm->table_mutex);
            tm->Scan([&](const Row &row) -> bool
                     {
                if (q.has_where && !Row_Matches_All(row, tm->schema, q.where_list))
                    return true;
                buf.push_back(make_sort_row(row));
                return true; });
        }

        emit_sorted(buf);
    }

    return "OK: " + to_string(rows) + " row(s) returned.";
}

string Database::Execute_Delete(const ParsedQuery &q)
{
    string tn = q.table_name;
    TableManager *tm = Get_Table(tn);
    if (!tm)
        return "ERROR: Table '" + tn + "' not found.";

    lock_guard tl(tm->table_mutex);
    if (!q.has_where)
    {
        tm->Delete_All();
        return "OK: All rows deleted.";
    }

    int deleted = tm->Delete_Where([&](const Row &row) -> bool
                                   {
        int ci = tm->schema.Find_Column(q.where.col.col);
        if (ci < 0) return false;
        return Field_Matches(row.fields[ci], q.where.op, q.where.val); });

    return "OK: " + to_string(deleted) + " row(s) deleted.";
}

string Database::Execute(const string &sql, RowCB cb)
{
    try
    {
        Lexer lx(sql);
        auto toks = lx.Tokenize();
        Parser pr(move(toks));
        ParsedQuery q = pr.Parse();
        if (!cb)
            cb = [](const ResultRow &)
            { return true; };

        switch (q.type)
        {
        case QueryType::CREATE_TABLE:
            return Execute_Create(q);
        case QueryType::INSERT:
            return Execute_Insert(q);
        case QueryType::SELECT:
            return Execute_Select(q, cb);
        case QueryType::DELETE_ROWS:
            return Execute_Delete(q);
        default:
            return "ERROR: Query not valid in this context.";
        }
    }
    catch (exception &e)
    {
        return string("ERROR: ") + e.what();
    }
}

DatabaseManager::DatabaseManager(const string &root_dir) : root_directory(root_dir)
{
    mkdir(root_directory.c_str(), 0755);
}

string DatabaseManager::Create_Database(const string &name)
{
    lock_guard lk(database_mutex);
    struct stat st;
    string path = root_directory + "/" + name;
    if (stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode))
        return "ERROR: Database '" + name + "' already exists.";

    mkdir(path.c_str(), 0755);
    return "OK: Database '" + name + "' created.";
}

vector<string> DatabaseManager::List_Databases()
{
    lock_guard lk(database_mutex);
    vector<string> names;
    DIR *d = opendir(root_directory.c_str());
    if (!d)
        return names;

    struct dirent *e;
    while ((e = readdir(d)) != nullptr)
    {
        string fn = e->d_name;
        if (fn == "." || fn == "..")
            continue;

        struct stat st;
        string full = root_directory + "/" + fn;
        if (stat(full.c_str(), &st) == 0 && S_ISDIR(st.st_mode))
            names.push_back(fn);
    }

    closedir(d);
    sort(names.begin(), names.end());

    return names;
}

Database *DatabaseManager::Get_Database(const string &name)
{
    {
        lock_guard lk(database_mutex);
        auto it = databases.find(name);
        if (it != databases.end())
            return it->second.get();
    }

    string path = root_directory + "/" + name;
    struct stat st;
    if (stat(path.c_str(), &st) != 0 || !S_ISDIR(st.st_mode))
        return nullptr;

    lock_guard lk(database_mutex);
    auto it = databases.find(name);
    if (it != databases.end())
        return it->second.get();

    databases[name] = make_unique<Database>(root_directory, name);
    return databases[name].get();
}

void DatabaseManager::Flush_DB_Manager()
{
    lock_guard lk(database_mutex);
    for (auto &[n, db] : databases)
        db->Flush_DB();
}

int DatabaseManager::Purge_Expired()
{
    lock_guard lk(database_mutex);
    int total = 0;
    for (auto &[n, db] : databases)
        total += db->Purge_Expired();
    return total;
}