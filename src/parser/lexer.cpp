#include <cctype>
#include <stdexcept>
#include <algorithm>
#include <unordered_map>
#include "parser/lexer.h"

using namespace std;

Lexer::Lexer(string sql) : source(move(sql)) {}

void Lexer::Skip_White_Space()
{
    while (position < source.size() && (source[position] == ' ' || source[position] == '\t' || source[position] == '\n' || source[position] == '\r'))
        position++;
}

Token Lexer::Read_Word()
{
    size_t start = position;
    while (position < source.size() && (isalnum(source[position]) || source[position] == '_'))
        position++;

    string word = source.substr(start, position - start);
    string upper = word;
    transform(upper.begin(), upper.end(), upper.begin(), ::toupper);

    static const unordered_map<string, TT> kw = {
        {"SELECT", TT::SELECT},
        {"FROM", TT::FROM},
        {"WHERE", TT::WHERE},
        {"INSERT", TT::INSERT},
        {"INTO", TT::INTO},
        {"VALUES", TT::VALUES},
        {"CREATE", TT::CREATE},
        {"TABLE", TT::TABLE},
        {"INNER", TT::INNER},
        {"JOIN", TT::JOIN},
        {"ON", TT::ON},
        {"PRIMARY", TT::PRIMARY},
        {"KEY", TT::KEY},
        {"NOT", TT::NOT},
        {"NULL", TT::KNULL},
        {"EXPIRY", TT::EXPIRY},
        {"DATABASE", TT::DATABASE},
        {"USE", TT::USE},
        {"SHOW", TT::SHOW},
        {"DATABASES", TT::DATABASES},
        {"TABLES", TT::TABLES},
        {"DROP", TT::DROP},
        {"DELETE", TT::DELETE},
        {"ORDER", TT::ORDER},
        {"BY", TT::BY},
        {"ASC", TT::ASC},
        {"DESC", TT::DESC},
        {"AND", TT::AND},
        {"IF", TT::IF},
        {"EXISTS", TT::EXISTS},
        {"INT", TT::TINT},
        {"INTEGER", TT::TINT},
        {"DECIMAL", TT::TDECIMAL},
        {"NUMERIC", TT::TDECIMAL},
        {"FLOAT", TT::TFLOAT},
        {"REAL", TT::TREAL},
        {"DOUBLE", TT::TDECIMAL},
        {"VARCHAR", TT::TVARCHAR},
        {"TEXT", TT::TTEXT},
        {"BOOLEAN", TT::TBOOLEAN},
        {"BOOL", TT::TBOOLEAN},
        {"TRUE", TT::LIT_INT},
        {"FALSE", TT::LIT_INT},
    };

    auto it = kw.find(upper);
    if (it != kw.end())
    {
        if (upper == "TRUE")
            return {TT::LIT_INT, "1"};
        if (upper == "FALSE")
            return {TT::LIT_INT, "0"};

        return {it->second, upper};
    }

    return {TT::IDENT, word};
}

Token Lexer::Read_Number()
{
    bool fl = false;
    size_t start = position;

    while (position < source.size() && isdigit(source[position]))
        position++;

    if (position < source.size() && source[position] == '.' && position + 1 < source.size() && isdigit(source[position + 1]))
    {
        position++;
        fl = true;

        while (position < source.size() && isdigit(source[position]))
            position++;
    }

    return {fl ? TT::LIT_FLOAT : TT::LIT_INT, source.substr(start, position - start)};
}

Token Lexer::Read_String_Literal(char delim)
{
    position++;
    size_t start = position;
    while (position < source.size() && source[position] != delim)
    {
        if (source[position] == delim && position + 1 < source.size() && source[position + 1] == delim)
        {
            position += 2;
            continue;
        }

        position++;
    }

    string val = source.substr(start, position - start);
    if (position < source.size())
        position++;

    return {TT::LIT_STR, val};
}

Token Lexer::Read_Symbol()
{
    char c = source[position++];
    switch (c)
    {
    case '*':
        return {TT::STAR, "*"};
    case ',':
        return {TT::COMMA, ","};
    case '(':
        return {TT::LPAR, "("};
    case ')':
        return {TT::RPAR, ")"};
    case ';':
        return {TT::SEMI, ";"};
    case '.':
        return {TT::DOT, "."};
    case '=':
        return {TT::EQ, "="};
    case '<':
        if (position < source.size() && source[position] == '=')
        {
            position++;
            return {TT::LTE, "<="};
        }

        return {TT::LT, "<"};
    case '>':
        if (position < source.size() && source[position] == '=')
        {
            position++;
            return {TT::GTE, ">="};
        }

        return {TT::GT, ">"};
    case '!':
        if (position < source.size() && source[position] == '=')
        {
            position++;
            return {TT::NEQ, "!="};
        }

        throw runtime_error("Unexpected character '!'");
    default:
        throw runtime_error(string("Unknown character: ") + c);
    }
}

vector<Token> Lexer::Tokenize()
{
    vector<Token> out;
    out.reserve(source.size() / 4 + 16);

    while (position < source.size())
    {
        Skip_White_Space();
        if (position >= source.size())
            break;
        
        char c = source[position];
        if (isalpha(c) || c == '_')
            out.push_back(Read_Word());
        else if (isdigit(c))
            out.push_back(Read_Number());
        else if (c == '\'')
            out.push_back(Read_String_Literal('\''));
        else if (c == '"')
            out.push_back(Read_String_Literal('"'));
        else
            out.push_back(Read_Symbol());
    }
    
    out.push_back({TT::EEOF, ""});
    return out;
}