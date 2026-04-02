#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include "parser/lexer.h"

using namespace std;

enum class QueryType
{
    CREATE_TABLE,
    CREATE_DATABASE,
    INSERT,
    SELECT,
    DELETE_ROWS,
    USE_DB,
    SHOW_DATABASES,
    SHOW_TABLES,
    UNKNOWN
};

typedef struct
{
    string name;
    string type; // normalised: INT DECIMAL VARCHAR BOOLEAN
    int vsz = 0;
    bool pk = false;
    bool notnull = false;
} ColumnDefinition;

typedef struct
{
    string tbl;
    string col;
} ColumnReference;

typedef struct
{
    string op;
    string val;
    ColumnReference col;
} WhereClause;

typedef struct
{
    string jtbl;
    ColumnReference left;
    ColumnReference right;
} JoinClause;

struct OrderBy
{
    bool desc = false;
    ColumnReference col;
};

typedef struct ParsedQuery
{
    QueryType type = QueryType::UNKNOWN;

    string table_name;
    vector<ColumnDefinition> col_defs;
    bool if_not_exists = false;

    uint64_t expiry_secs = 0;
    vector<string> inserted_values;
    vector<vector<string>> inserted_rows;

    OrderBy order;
    JoinClause join;
    WhereClause where;

    string from_table;
    vector<ColumnReference> selected_columns;

    bool has_join = false;
    bool has_where = false;
    bool has_order = false;
    bool select_star = false;

} ParsedQuery;

class Parser
{
public:
    explicit Parser(vector<Token> tokens);
    ParsedQuery Parse();

private:
    size_t position = 0;
    vector<Token> tokens;

    Token &Cur();
    Token Consume();
    Token Expect(TT t);
    string Parse_Value();
    WhereClause Parse_Where();
    ColumnReference Parse_Column_Reference();

    ParsedQuery Parse_Create();
    ParsedQuery Parse_Insert();
    ParsedQuery Parse_Select();
    ParsedQuery Parse_Delete();
    ParsedQuery Parse_Show();
    ParsedQuery Parse_Use();

    // Map token type → normalised type string
    static string Token_To_TypeName(TT t);
};
