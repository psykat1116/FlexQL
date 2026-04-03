#pragma once
#include <string>
#include <vector>

using namespace std;

enum class TT
{
    SELECT,
    FROM,
    WHERE,
    INSERT,
    INTO,
    VALUES,
    CREATE,
    TABLE,
    INNER,
    JOIN,
    ON,
    PRIMARY,
    KEY,
    NOT,
    KNULL,
    EXPIRY,
    DATABASE,
    USE,
    SHOW,
    DATABASES,
    TABLES,
    DROP,
    DELETE,
    ORDER,
    BY,
    ASC,
    DESC,
    AND,
    IF,
    EXISTS,
    TINT,
    TDECIMAL,
    TVARCHAR,
    TTEXT,
    TFLOAT,
    TREAL,
    TBOOLEAN,
    LIT_INT,
    LIT_FLOAT,
    LIT_STR,
    IDENT,
    STAR,
    COMMA,
    LPAR,
    RPAR,
    SEMI,
    DOT,
    EQ,
    NEQ,
    LT,
    GT,
    LTE,
    GTE,
    EEOF
};

typedef struct
{
    TT type;
    string val;
} Token;

class Lexer
{
public:
    explicit Lexer(string sql);
    vector<Token> Tokenize();

private:
    string source;
    size_t position = 0;

    Token Read_Word();
    Token Read_Number();
    Token Read_Symbol();
    void Skip_White_Space();
    Token Read_String_Literal(char delim);
};