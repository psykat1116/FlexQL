#include "storage/page.h"
#include "parser/lexer.h"
#include "parser/parser.h"
#include "query/executor.h"
#include "cache/lru_cache.h"
#include "index/bplus_tree.h"
#include "expiration/expiry.h"

#include <cassert>
#include <cstring>
#include <iostream>
#include <sys/stat.h>

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, msg)                            \
    do                                              \
    {                                               \
        if (cond)                                   \
        {                                           \
            g_pass++;                               \
            std::cout << "  PASS: " << msg << "\n"; \
        }                                           \
        else                                        \
        {                                           \
            g_fail++;                               \
            std::cerr << "  FAIL: " << msg << "\n"; \
        }                                           \
    } while (0)

static TableSchema makeStudentSchema()
{
    TableSchema sc;
    memset(&sc, 0, sizeof(sc));
    strncpy(sc.table_name, "STUDENT", MAX_NAME - 1);
    sc.num_cols = 3;

    strncpy(sc.cols[0].name, "ID", MAX_NAME - 1);
    sc.cols[0].type = ColType::DECIMAL;
    sc.cols[0].max_size = 8;
    sc.cols[0].is_primary_key = true;
    sc.cols[0].not_null = true;

    strncpy(sc.cols[1].name, "NAME", MAX_NAME - 1);
    sc.cols[1].type = ColType::VARCHAR;
    sc.cols[1].max_size = 50;

    strncpy(sc.cols[2].name, "GPA", MAX_NAME - 1);
    sc.cols[2].type = ColType::DECIMAL;
    sc.cols[2].max_size = 8;

    return sc;
}

static Row makeRow(int id, const std::string &name, double gpa, uint64_t expiry = 0)
{
    TableSchema sc = makeStudentSchema();

    Row row;
    row.expiry_timestamp = expiry;
    row.fields.push_back(Make_Field(std::to_string(id), sc.cols[0]));
    row.fields.push_back(Make_Field(name, sc.cols[1]));
    row.fields.push_back(Make_Field(std::to_string(gpa), sc.cols[2]));

    return row;
}

static void testPage()
{
    std::cout << "\n[ Page serialisation ]\n";
    Page pg;
    pg.Initialize_Page(0);
    TableSchema sc = makeStudentSchema();

    Row r1 = makeRow(1, "Alice", 9.1);
    Row r2 = makeRow(2, "Bob", 8.5);
    Row r3 = makeRow(3, "Christopher", 7.9);

    auto b1 = Serialize_Row(r1, sc);
    auto b2 = Serialize_Row(r2, sc);
    auto b3 = Serialize_Row(r3, sc);

    int s0 = pg.Insert_Row(b1.data(), (uint16_t)b1.size());
    int s1 = pg.Insert_Row(b2.data(), (uint16_t)b2.size());
    int s2 = pg.Insert_Row(b3.data(), (uint16_t)b3.size());

    CHECK(s0 == 0, "Alice at slot 0");
    CHECK(s1 == 1, "Bob at slot 1");
    CHECK(s2 == 2, "Christopher at slot 2");
    CHECK(pg.Number_Of_Slots() == 3, "Page has 3 slots");

    Row out0 = Deserialize_Row(pg.getRow(0), sc);
    Row out1 = Deserialize_Row(pg.getRow(1), sc);
    Row out2 = Deserialize_Row(pg.getRow(2), sc);

    CHECK(out0.fields[0].integer_value == 1, "Row 0 ID == 1");
    CHECK(out0.fields[1].string_value == "Alice", "Row 0 NAME == Alice");
    CHECK(out1.fields[1].string_value == "Bob", "Row 1 NAME == Bob");
    CHECK(out2.fields[1].string_value == "Christopher", "Row 2 NAME == Christopher");
    CHECK(b3.size() > b2.size(), "Christopher row larger than Bob row");
}

// ── Test 2: LRU Cache ─────────────────────────────────────────
static void testLRUCache()
{
    std::cout << "\n[ LRU Cache ]\n";
    LRUCache cache(3);
    int flushed = 0;
    auto flush_fn = [&](uint32_t, const Page &)
    { flushed++; };

    Page pg;
    pg.Initialize_Page(0);
    cache.Put_Page(0, pg, false, flush_fn);
    cache.Put_Page(1, pg, false, flush_fn);
    cache.Put_Page(2, pg, false, flush_fn);

    CHECK(cache.Get_Page(0) != nullptr, "Page 0 in cache");
    CHECK(cache.Get_Page(1) != nullptr, "Page 1 in cache");
    CHECK(cache.Get_Page(2) != nullptr, "Page 2 in cache");

    cache.Get_Page(0);                     // touch 0 → MRU
    cache.Put_Page(3, pg, true, flush_fn); // evict 1 (LRU after 0 touched)

    CHECK(cache.Get_Page(1) == nullptr, "Page 1 evicted");
    CHECK(cache.Get_Page(0) != nullptr, "Page 0 still cached");
    CHECK(flushed == 0, "No dirty evictions yet");

    cache.Mark_Dirty(2);
    cache.Put_Page(4, pg, false, flush_fn);
    CHECK(flushed == 1, "Dirty page 2 flushed on eviction");
}

// ── Test 3: B+ Tree ───────────────────────────────────────────
static void testBPlusTree()
{
    std::cout << "\n[ B+ Tree index ]\n";
    BPlusTree tree;
    tree.Insert_Key(1, 0, 0);
    tree.Insert_Key(5, 0, 1);
    tree.Insert_Key(42, 1, 0);
    tree.Insert_Key(99, 2, 3);

    IndexRecord r;
    CHECK(tree.Find_Key(1, r) && r.page_id == 0 && r.slot_idx == 0, "Find key 1");
    CHECK(tree.Find_Key(42, r) && r.page_id == 1 && r.slot_idx == 0, "Find key 42");
    CHECK(tree.Find_Key(99, r) && r.page_id == 2 && r.slot_idx == 3, "Find key 99");
    CHECK(!tree.Find_Key(7, r), "Key 7 not found");

    const std::string idx_path = "/tmp/flexql_test.idx";
    tree.Save_Tree(idx_path);
    BPlusTree tree2;
    tree2.Load_Tree(idx_path);
    CHECK(tree2.Find_Key(42, r) && r.page_id == 1, "Index persists across save/load");
    CHECK(tree2.Get_Size() == 4, "All 4 entries reloaded");
}

// ── Test 4: Lexer ─────────────────────────────────────────────
static void testLexer()
{
    std::cout << "\n[ Lexer ]\n";
    Lexer lx("SELECT * FROM STUDENT WHERE ID = 42;");
    auto toks = lx.Tokenize();

    CHECK(toks[0].type == TT::SELECT, "Token 0 = SELECT");
    CHECK(toks[1].type == TT::STAR, "Token 1 = *");
    CHECK(toks[2].type == TT::FROM, "Token 2 = FROM");
    CHECK(toks[3].type == TT::IDENT && toks[3].val == "STUDENT", "Token 3 = STUDENT");
    CHECK(toks[4].type == TT::WHERE, "Token 4 = WHERE");
    CHECK(toks[7].type == TT::LIT_INT && toks[7].val == "42", "Token 7 = 42");

    Lexer lx2("INSERT INTO T VALUES (1,'Alice',9.10);");
    auto toks2 = lx2.Tokenize();
    bool found_str = false;
    for (auto &t : toks2)
        if (t.type == TT::LIT_STR && t.val == "Alice")
            found_str = true;
    CHECK(found_str, "String literal 'Alice' Tokenized");

    // New operators
    Lexer lx3("SELECT * FROM T WHERE X >= 5;");
    auto toks3 = lx3.Tokenize();
    bool found_gte = false;
    for (auto &t : toks3)
        if (t.type == TT::GTE)
            found_gte = true;
    CHECK(found_gte, ">= operator Tokenized");

    Lexer lx4("SELECT * FROM T WHERE X <= 5;");
    auto toks4 = lx4.Tokenize();
    bool found_lte = false;
    for (auto &t : toks4)
        if (t.type == TT::LTE)
            found_lte = true;
    CHECK(found_lte, "<= operator Tokenized");
}

// ── Test 5: Parser ────────────────────────────────────────────
static void testParser()
{
    std::cout << "\n[ Parser ]\n";

    // CREATE TABLE
    {
        Lexer lx("CREATE TABLE STUDENT(ID DECIMAL PRIMARY KEY NOT NULL,NAME VARCHAR(50));");
        Parser pr(lx.Tokenize());
        ParsedQuery q = pr.Parse();

        CHECK(q.type == QueryType::CREATE_TABLE, "CREATE detected");
        CHECK(q.table_name == "STUDENT", "Table name = STUDENT");
        CHECK(q.col_defs.size() == 2, "2 columns");
        CHECK(q.col_defs[0].pk, "ID is primary key");
        CHECK(q.col_defs[1].vsz == 50, "VARCHAR(50) size");
    }

    // CREATE TABLE IF NOT EXISTS
    {
        Lexer lx("CREATE TABLE IF NOT EXISTS T(ID DECIMAL PRIMARY KEY);");
        Parser pr(lx.Tokenize());
        ParsedQuery q = pr.Parse();

        CHECK(q.type == QueryType::CREATE_TABLE && q.if_not_exists, "CREATE TABLE IF NOT EXISTS detected");
    }

    // Batch INSERT
    {
        Lexer lx("INSERT INTO T VALUES(1,'Bob',8.5),(2,'Carol',9.0) EXPIRY 3600;");
        Parser pr(lx.Tokenize());
        ParsedQuery q = pr.Parse();

        CHECK(q.type == QueryType::INSERT, "INSERT detected");
        CHECK(q.inserted_rows.size() == 2, "2 rows in batch");
        CHECK(q.inserted_rows[0][1] == "Bob", "Row 0 name = Bob");
        CHECK(q.inserted_rows[1][1] == "Carol", "Row 1 name = Carol");
        CHECK(q.expiry_secs == 3600, "EXPIRY = 3600");
    }

    // SELECT with WHERE >=
    {
        Lexer lx("SELECT NAME FROM STUDENT WHERE GPA >= 9.0;");
        Parser pr(lx.Tokenize());
        ParsedQuery q = pr.Parse();

        CHECK(q.type == QueryType::SELECT, "SELECT detected");
        CHECK(q.has_where, "Has WHERE");
        CHECK(q.where.op == ">=", "WHERE op = >=");
    }

    // SELECT with ORDER BY
    {
        Lexer lx("SELECT NAME FROM STUDENT ORDER BY GPA DESC;");
        Parser pr(lx.Tokenize());
        ParsedQuery q = pr.Parse();

        CHECK(q.has_order && q.order.desc, "ORDER BY DESC detected");
    }

    // Double-quoted strings
    {
        Lexer lx2("INSERT INTO T VALUES(1, \"Saikat Samanta\", 99);");
        bool found = false;
        auto toks2 = lx2.Tokenize();
        for (auto &t : toks2)
            if (t.type == TT::LIT_STR && t.val == "Saikat Samanta")
                found = true;

        CHECK(found, "Double-quoted string literal Tokenized");
    }

    // New types: TEXT, BOOLEAN, FLOAT, REAL
    {
        Lexer lx3("CREATE TABLE T2(A TEXT, B BOOLEAN, C FLOAT, D REAL);");
        Parser pr3(lx3.Tokenize());
        ParsedQuery q3 = pr3.Parse();

        CHECK(q3.col_defs[0].type == "TEXT", "TEXT type parsed");
        CHECK(q3.col_defs[1].type == "BOOLEAN", "BOOLEAN type parsed");
        CHECK(q3.col_defs[2].type == "DECIMAL", "FLOAT maps to DECIMAL");
        CHECK(q3.col_defs[3].type == "DECIMAL", "REAL maps to DECIMAL");
    }

    // TRUE/FALSE boolean literals
    {
        Lexer lx4("INSERT INTO T VALUES(1, TRUE, FALSE);");
        Parser pr4(lx4.Tokenize());
        ParsedQuery q4 = pr4.Parse();

        CHECK(q4.inserted_rows[0][1] == "1", "TRUE Tokenized as 1");
        CHECK(q4.inserted_rows[0][2] == "0", "FALSE Tokenized as 0");
    }

    // DELETE
    {
        Lexer lx("DELETE FROM STUDENT;");
        Parser pr(lx.Tokenize());
        ParsedQuery q = pr.Parse();

        CHECK(q.type == QueryType::DELETE_ROWS, "DELETE detected");
        CHECK(!q.has_where, "No WHERE on DELETE *");
    }

    // SELECT with INNER JOIN
    {
        Lexer lx("SELECT * FROM A INNER JOIN B ON A.ID = B.AID;");
        Parser pr(lx.Tokenize());
        ParsedQuery q = pr.Parse();
        CHECK(q.has_join, "Has JOIN");
        CHECK(q.join.jtbl == "B", "JOIN table = B");
        CHECK(q.join.left.tbl == "A", "Left table = A");
        CHECK(q.join.right.tbl == "B", "Right table = B");
    }
}

// ── Test 6: Expiry ────────────────────────────────────────────
static void testExpiry()
{
    std::cout << "\n[ Expiration timestamps ]\n";
    CHECK(!Has_Expired(0), "0 = never expires");
    CHECK(!Has_Expired(Make_Expiry(3600)), "Future expiry not expired");
    CHECK(Has_Expired(1), "Past timestamp expired");
}

static void testDatabase()
{
    std::cout << "\n[ Database engine (end-to-end) ]\n";

    const std::string root = "/tmp/flexql_tests";
    system("rm -rf /tmp/flexql_tests");
    mkdir(root.c_str(), 0755);

    Database db(root, "testdb");

    auto s = db.Execute(
        "CREATE TABLE STUDENT("
        "  ID DECIMAL PRIMARY KEY NOT NULL,"
        "  NAME VARCHAR(50) NOT NULL,"
        "  GPA DECIMAL"
        ");");
    CHECK(s.rfind("OK", 0) == 0, "CREATE TABLE ok");

    // Duplicate table rejected
    auto s2 = db.Execute("CREATE TABLE STUDENT(ID DECIMAL PRIMARY KEY);");
    CHECK(s2.rfind("ERROR", 0) == 0, "Duplicate CREATE rejected");

    // IF NOT EXISTS ignored
    auto s3 = db.Execute("CREATE TABLE IF NOT EXISTS STUDENT(ID DECIMAL PRIMARY KEY);");
    CHECK(s3.rfind("OK", 0) == 0, "IF NOT EXISTS silently ignored");

    // Batch INSERT
    db.Execute("INSERT INTO STUDENT VALUES(1,'Alice',9.10),(2,'Bob',8.50),(3,'Carol',9.80);");
    db.Execute("INSERT INTO STUDENT VALUES(4,'Dave',7.20),(5,'Eva',9.90);");

    // SELECT *
    int count = 0;
    db.Execute("SELECT * FROM STUDENT;", [&](const ResultRow &)
               { count++; return true; });
    CHECK(count == 5, "SELECT * returns 5 rows");

    // WHERE = on PK → B+ Tree
    count = 0;
    std::string found_name;
    db.Execute("SELECT * FROM STUDENT WHERE ID = 3;",
               [&](const ResultRow &r)
               {
                   count++;
                   for (size_t i = 0; i < r.column_names.size(); i++)
                       if (r.column_names[i] == "NAME")
                           found_name = r.values[i];
                   return true;
               });
    CHECK(count == 1 && found_name == "Carol", "WHERE ID=3 returns Carol");

    // WHERE > on non-PK
    count = 0;
    db.Execute("SELECT * FROM STUDENT WHERE GPA > 9.0;",
               [&](const ResultRow &)
               { count++; return true; });
    CHECK(count == 3, "WHERE GPA>9.0 returns 3 rows");

    // WHERE >=
    count = 0;
    db.Execute("SELECT * FROM STUDENT WHERE GPA >= 9.1;",
               [&](const ResultRow &)
               { count++; return true; });
    CHECK(count == 3, "WHERE GPA>=9.1 returns 3 rows");

    // WHERE <=
    count = 0;
    db.Execute("SELECT * FROM STUDENT WHERE GPA <= 8.5;", [&](const ResultRow &)
               { count++; return true; });
    CHECK(count == 2, "WHERE GPA<=8.5 returns 2 rows");

    // ORDER BY
    std::vector<std::string> names;
    db.Execute("SELECT NAME FROM STUDENT ORDER BY GPA DESC;", [&](const ResultRow &r)
               { names.push_back(r.values[0]); return true; });
    CHECK(!names.empty() && names[0] == "Eva", "ORDER BY GPA DESC: Eva first");

    // Wrong value count rejected
    auto bad = db.Execute("INSERT INTO STUDENT VALUES(99,'OnlyTwo');");
    CHECK(bad.rfind("ERROR", 0) == 0, "Wrong value count rejected");

    // DELETE all + verify
    db.Execute("DELETE FROM STUDENT;");
    count = 0;
    db.Execute("SELECT * FROM STUDENT;", [&](const ResultRow &)
               { count++; return true; });
    CHECK(count == 0, "DELETE FROM empties table");

    // Flush and reopen (persistence)
    db.Execute("INSERT INTO STUDENT VALUES(10,'Persist',8.0);");
    db.Flush_DB();
    {
        Database db2(root, "testdb");
        int c2 = 0;
        db2.Execute("SELECT * FROM STUDENT;",
                    [&](const ResultRow &)
                    { c2++; return true; });
        CHECK(c2 == 1, "Data persists across reopen");
    }

    // INNER JOIN
    db.Execute(
        "CREATE TABLE DEPT("
        "  DID DECIMAL PRIMARY KEY NOT NULL,"
        "  DEPT_NAME VARCHAR(50)"
        ");");
    db.Execute("INSERT INTO DEPT VALUES(1,'Engineering'),(2,'Science');");
    db.Execute(
        "CREATE TABLE ENROLLMENT("
        "  EID DECIMAL PRIMARY KEY NOT NULL,"
        "  SID DECIMAL NOT NULL,"
        "  DID DECIMAL NOT NULL"
        ");");
    db.Execute("INSERT INTO ENROLLMENT VALUES(1,10,1),(2,10,2);");

    count = 0;
    db.Execute(
        "SELECT * FROM STUDENT INNER JOIN ENROLLMENT "
        "ON STUDENT.ID = ENROLLMENT.SID;",
        [&](const ResultRow &)
        { count++; return true; });
    CHECK(count == 2, "INNER JOIN returns 2 matched rows");
}

int main()
{
    std::cout << "================================================\n";
    std::cout << "  FlexQL Test Suite\n";
    std::cout << "================================================\n";

    testPage();
    testLRUCache();
    testBPlusTree();
    testLexer();
    testParser();
    testExpiry();
    testDatabase();

    std::cout << "\n================================================\n";
    std::cout << "  Results: " << g_pass << " passed   "
              << g_fail << " failed\n";
    std::cout << "================================================\n\n";
    return g_fail > 0 ? 1 : 0;
}
