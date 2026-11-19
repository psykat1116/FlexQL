#include <stdexcept>
#include "parser/parser.h"

Parser::Parser(std::vector<Token> tokens) : tokens(std::move(tokens)) {}

Token &Parser::Cur() { return tokens[position]; }
Token Parser::Consume() { return tokens[position++]; }

Token Parser::Expect(TT t)
{
    if (Cur().type != t)
        throw std::runtime_error(
            "Parse error: unexpected '" + Cur().val + "'");

    return Consume();
}

ColumnReference Parser::Parse_Column_Reference()
{
    ColumnReference r;
    std::string first = Expect(TT::IDENT).val;

    if (Cur().type == TT::DOT)
    {
        Consume();
        r.tbl = first;
        r.col = Expect(TT::IDENT).val;
    }
    else
    {
        r.col = first;
    }

    return r;
}

std::string Parser::Parse_Value()
{
    switch (Cur().type)
    {
    case TT::LIT_INT:
    case TT::LIT_FLOAT:
    case TT::LIT_STR:
    case TT::KNULL:
    case TT::IDENT:
        return Consume().val;
    default:
        throw std::runtime_error(
            "Parse error: expected value, got '" + Cur().val + "'");
    }
}

WhereClause Parser::Parse_Where()
{
    WhereClause w;
    w.col = Parse_Column_Reference();

    TT op = Cur().type;
    if (op == TT::EQ || op == TT::NEQ || op == TT::LT ||
        op == TT::GT || op == TT::LTE || op == TT::GTE)
        w.op = Consume().val;
    else
        throw std::runtime_error(
            "Parse error: expected comparison operator, got '" + Cur().val + "'");

    w.val = Parse_Value();
    return w;
}

std::string Parser::Token_To_TypeName(TT t)
{
    switch (t)
    {
    case TT::TINT:
        return "INT";
    case TT::TDECIMAL:
        return "DECIMAL";
    case TT::TFLOAT:
        return "DECIMAL"; // FLOAT to stored as DECIMAL
    case TT::TREAL:
        return "DECIMAL"; // REAL to stored as DECIMAL
    case TT::TVARCHAR:
        return "VARCHAR";
    case TT::TTEXT:
        return "TEXT"; // TEXT to large VARCHAR
    case TT::TBOOLEAN:
        return "BOOLEAN";
    default:
        return "";
    }
}

ParsedQuery Parser::Parse_Create()
{
    Consume();
    if (Cur().type == TT::DATABASE)
    {
        Consume();

        ParsedQuery q;
        q.type = QueryType::CREATE_DATABASE;
        q.table_name = Expect(TT::IDENT).val;

        return q;
    }

    Expect(TT::TABLE);
    ParsedQuery q;
    q.type = QueryType::CREATE_TABLE;

    if (Cur().type == TT::IF)
    {
        Consume();
        Expect(TT::NOT);
        Expect(TT::EXISTS);
        q.if_not_exists = true;
    }

    q.table_name = Expect(TT::IDENT).val;
    Expect(TT::LPAR);

    while (Cur().type != TT::RPAR && Cur().type != TT::EEOF)
    {
        ColumnDefinition c;
        c.name = Expect(TT::IDENT).val;

        std::string tn = Token_To_TypeName(Cur().type);
        if (tn.empty())
            throw std::runtime_error("Unknown type: " + Cur().val);

        c.type = tn;
        Consume();

        if ((c.type == "VARCHAR" || c.type == "TEXT") &&
            Cur().type == TT::LPAR)
        {
            Consume();
            c.vsz = std::stoi(Expect(TT::LIT_INT).val);
            Expect(TT::RPAR);
        }

        bool m = true;
        while (m)
        {
            if (Cur().type == TT::PRIMARY)
            {
                Consume();
                Expect(TT::KEY);
                c.pk = true;
            }
            else if (Cur().type == TT::NOT)
            {
                Consume();
                Expect(TT::KNULL);
                c.notnull = true;
            }
            else
                m = false;
        }
        q.col_defs.push_back(c);
        if (Cur().type == TT::COMMA)
            Consume();
        else
            break;
    }

    Expect(TT::RPAR);
    return q;
}

ParsedQuery Parser::Parse_Insert()
{
    Consume();
    Expect(TT::INTO);
    ParsedQuery q;
    q.type = QueryType::INSERT;
    q.table_name = Expect(TT::IDENT).val;
    Expect(TT::VALUES);

    do
    {
        Expect(TT::LPAR);
        std::vector<std::string> row_vals;
        while (Cur().type != TT::RPAR && Cur().type != TT::EEOF)
        {
            row_vals.push_back(Parse_Value());
            if (Cur().type == TT::COMMA)
                Consume();
            else
                break;
        }
        Expect(TT::RPAR);
        q.inserted_rows.push_back(std::move(row_vals));
        if (Cur().type == TT::COMMA)
            Consume();
        else
            break;
    } while (Cur().type == TT::LPAR);

    if (!q.inserted_rows.empty())
        q.inserted_values = q.inserted_rows[0];

    if (Cur().type == TT::EXPIRY)
    {
        Consume();
        q.expiry_secs = (uint64_t)std::stoull(Expect(TT::LIT_INT).val);
    }
    return q;
}

ParsedQuery Parser::Parse_Select()
{
    Consume();
    ParsedQuery q;
    q.type = QueryType::SELECT;

    if (Cur().type == TT::STAR)
    {
        q.select_star = true;
        Consume();
    }
    else
    {
        while (true)
        {
            q.selected_columns.push_back(Parse_Column_Reference());
            if (Cur().type == TT::COMMA)
                Consume();
            else
                break;
        }
    }

    Expect(TT::FROM);
    q.from_table = Expect(TT::IDENT).val;

    if (Cur().type == TT::INNER)
    {
        Consume();
        Expect(TT::JOIN);
        q.join.jtbl = Expect(TT::IDENT).val;
        if (Cur().type == TT::ON)
        {
            Consume();
            q.join.left = Parse_Column_Reference();
            Expect(TT::EQ);
            q.join.right = Parse_Column_Reference();
            q.join.has_on = true;
        }
        else
        {
            q.join.has_on = false;
        }

        q.has_join = true;
    }

    if (Cur().type == TT::WHERE)
    {
        Consume();
        q.has_where = true;
        q.where = Parse_Where();
        q.where_list.push_back(q.where);
        while (Cur().type == TT::AND)
        {
            Consume();
            q.where_list.push_back(Parse_Where());
        }
    }

    if (Cur().type == TT::ORDER)
    {
        Consume();
        Expect(TT::BY);
        q.has_order = true;
        q.order.col = Parse_Column_Reference();
        if (Cur().type == TT::DESC)
        {
            Consume();
            q.order.desc = true;
        }
        else if (Cur().type == TT::ASC)
        {
            Consume();
            q.order.desc = false;
        }
    }

    return q;
}

ParsedQuery Parser::Parse_Delete()
{
    Consume();
    Expect(TT::FROM);
    ParsedQuery q;
    q.type = QueryType::DELETE_ROWS;
    q.table_name = Expect(TT::IDENT).val;

    if (Cur().type == TT::WHERE)
    {
        Consume();
        q.has_where = true;
        q.where = Parse_Where();
    }
    return q;
}

ParsedQuery Parser::Parse_Show()
{
    Consume();
    ParsedQuery q;
    if (Cur().type == TT::DATABASES)
    {
        Consume();
        q.type = QueryType::SHOW_DATABASES;
    }
    else if (Cur().type == TT::TABLES)
    {
        Consume();
        q.type = QueryType::SHOW_TABLES;
    }
    else
        throw std::runtime_error(
            "Parse error: expected DATABASES or TABLES after SHOW");
    return q;
}

ParsedQuery Parser::Parse_Use()
{
    Consume();
    ParsedQuery q;
    q.type = QueryType::USE_DB;
    q.table_name = Expect(TT::IDENT).val;
    return q;
}

ParsedQuery Parser::Parse()
{
    ParsedQuery q;
    switch (Cur().type)
    {
    case TT::CREATE:
        q = Parse_Create();
        break;
    case TT::INSERT:
        q = Parse_Insert();
        break;
    case TT::SELECT:
        q = Parse_Select();
        break;
    case TT::DELETE:
        q = Parse_Delete();
        break;
    case TT::SHOW:
        q = Parse_Show();
        break;
    case TT::USE:
        q = Parse_Use();
        break;
    default:
        throw std::runtime_error(
            "Unknown SQL statement: '" + Cur().val + "'");
    }
    if (Cur().type == TT::SEMI)
        Consume();
    return q;
}