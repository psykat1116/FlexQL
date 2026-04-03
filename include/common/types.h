#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include <cstring>
#include <sstream>
#include <iomanip>
#include <strings.h>
#include <functional>

using namespace std;

static const uint32_t MAX_NAME = 64;
static const uint32_t MAX_COLS = 32;
static const uint32_t PAGE_SIZE = 16384;
static const uint32_t INVALID_PAGE = 0xFFFFFFFF;

enum class ColType : uint8_t
{
    INT,
    DECIMAL, // covers FLOAT, REAL, DECIMAL
    VARCHAR, // covers TEXT (unbounded VARCHAR)
    BOOLEAN  // stored as 1-byte int (0/1)
};

typedef struct
{
    char name[MAX_NAME];
    ColType type;
    int32_t max_size;
    bool not_null;
    bool is_primary_key;
} ColSchema;

typedef struct
{
    uint32_t num_cols;
    ColSchema cols[MAX_COLS];
    char table_name[MAX_NAME];

    int Find_Column(const string &n) const
    {
        for (uint32_t i = 0; i < num_cols; i++)
            if (strcasecmp(cols[i].name, n.c_str()) == 0)
                return (int)i;

        return -1;
    }

    int Primary_Key_Column() const
    {
        for (uint32_t i = 0; i < num_cols; i++)
            if (cols[i].is_primary_key)
                return (int)i;

        return 0;
    }
} TableSchema;

typedef struct
{
    string string_value;
    bool is_null = false;
    int32_t integer_value = 0;
    double double_value = 0.0;
    bool boolean_value = false;
    ColType column_type = ColType::INT;

    string Convert_To_String() const
    {
        if (is_null)
            return "NULL";

        switch (column_type)
        {
        case ColType::INT:
            return to_string(integer_value);
        case ColType::DECIMAL:
            if (double_value == (long long)double_value)
                return to_string((long long)double_value);
            else
            {
                ostringstream ss;
                ss << fixed << setprecision(2) << double_value;
                return ss.str();
            }
        case ColType::VARCHAR:
            return string_value;
        case ColType::BOOLEAN:
            return boolean_value ? "true" : "false";
        }

        return "";
    }

} Field;

typedef struct
{
    vector<Field> fields;
    uint64_t expiry_timestamp = 0;
} Row;

typedef struct
{
    vector<string> values;
    vector<string> column_names;
} ResultRow;

using RowCB = function<bool(const ResultRow &)>;