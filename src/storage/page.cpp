#include <cstring>
#include <stdexcept>
#include "storage/page.h"

Field Make_Field(const string &val, const ColSchema &col)
{
    Field f;
    f.column_type = col.type;
    if (val == "NULL" || val == "null")
    {
        f.is_null = true;
        return f;
    }

    switch (col.type)
    {
    case ColType::INT:
        f.integer_value = stoi(val);
        f.double_value = (double)f.integer_value;
        break;
    case ColType::DECIMAL:
        f.double_value = stod(val);
        f.integer_value = (int32_t)f.integer_value;
        break;
    case ColType::VARCHAR:
        f.string_value = val;
        break;
    case ColType::BOOLEAN:
        if (val == "1" || val == "true" || val == "TRUE" || val == "yes" || val == "YES")
            f.boolean_value = true;
        else
            f.boolean_value = false;

        f.integer_value = f.boolean_value ? 1 : 0;
        break;
    }

    return f;
}

// ── Binary row layout ─────────────────────────────────────────
// [expiry_ts   : uint64_t              8 bytes]
// [col_offsets : uint16_t * (ncols+1)        ]  last = total size sentinel
// [col 0 data  ...]
// [col 1 data  ...]
//   INT/DECIMAL → 8 bytes (double for uniformity)
//   VARCHAR     → 2-byte length + chars
//   BOOLEAN     → 1 byte (0 or 1)
//   NULL        → 1 sentinel byte (0xFF)

vector<uint8_t> Serialize_Row(const Row &row, const TableSchema &sc)
{
    int n = (int)sc.num_cols;

    vector<uint16_t> offsets(n + 1);
    uint32_t expiry_sz = sizeof(uint64_t);
    uint32_t offsets_sz = (uint32_t)(n + 1) * sizeof(uint16_t);

    uint32_t cursor = expiry_sz + offsets_sz;
    for (int i = 0; i < n; i++)
    {
        offsets[i] = (uint16_t)cursor;
        if (row.fields[i].is_null)
        {
            cursor += 1;
            continue;
        }
        switch (sc.cols[i].type)
        {
        case ColType::INT:
        case ColType::DECIMAL:
            cursor += 8;
            break;
        case ColType::VARCHAR:
            cursor += 2 + (uint16_t)row.fields[i].string_value.size();
            break;
        case ColType::BOOLEAN:
            cursor += 1;
            break;
        }
    }
    offsets[n] = (uint16_t)cursor;

    vector<uint8_t> buf(cursor, 0);
    memcpy(buf.data(), &row.expiry_timestamp, sizeof(uint64_t));
    memcpy(buf.data() + expiry_sz, offsets.data(), offsets_sz);

    for (int i = 0; i < n; i++)
    {
        uint8_t *dst = buf.data() + offsets[i];
        if (row.fields[i].is_null)
        {
            *dst = 0xFF;
            continue;
        }
        switch (sc.cols[i].type)
        {
        case ColType::INT:
        {
            double d = (double)row.fields[i].integer_value;
            memcpy(dst, &d, 8);
            break;
        }
        case ColType::DECIMAL:
            memcpy(dst, &row.fields[i].double_value, 8);
            break;
        case ColType::VARCHAR:
        {
            uint16_t slen = (uint16_t)row.fields[i].string_value.size();
            memcpy(dst, &slen, 2);
            memcpy(dst + 2, row.fields[i].string_value.c_str(), slen);
            break;
        }
        case ColType::BOOLEAN:
            *dst = row.fields[i].boolean_value ? 1 : 0;
            break;
        }
    }
    return buf;
}

Row Deserialize_Row(const uint8_t *raw, const TableSchema &sc)
{
    Row row;
    int n = (int)sc.num_cols;

    memcpy(&row.expiry_timestamp, raw, sizeof(uint64_t));

    vector<uint16_t> offsets(n + 1);
    uint32_t expiry_sz = sizeof(uint64_t);
    memcpy(offsets.data(), raw + expiry_sz, (size_t)(n + 1) * sizeof(uint16_t));

    for (int i = 0; i < n; i++)
    {
        const uint8_t *src = raw + offsets[i];
        uint16_t field_sz = offsets[i + 1] - offsets[i];

        Field f;
        f.column_type = sc.cols[i].type;
        switch (sc.cols[i].type)
        {
        case ColType::INT:
            if (*src == 0xFF && field_sz == 1)
            {
                f.is_null = true;
            }
            else
            {
                double d;
                memcpy(&d, src, 8);
                f.integer_value = (int32_t)d;
                f.double_value = d;
            }
            break;
        case ColType::DECIMAL:
            if (*src == 0xFF && field_sz == 1)
            {
                f.is_null = true;
            }
            else
            {
                memcpy(&f.double_value, src, 8);
                f.integer_value = (int32_t)f.double_value;
            }
            break;
        case ColType::VARCHAR:
            if (field_sz == 1 && *src == 0xFF)
            {
                f.is_null = true;
            }
            else
            {
                uint16_t slen;
                memcpy(&slen, src, 2);
                f.string_value.assign(reinterpret_cast<const char *>(src + 2), slen);
            }
            break;
        case ColType::BOOLEAN:
            if (*src == 0xFF && field_sz == 1)
            {
                f.is_null = true;
            }
            else
            {
                f.boolean_value = (*src != 0);
                f.integer_value = f.boolean_value ? 1 : 0;
            }
            break;
        }

        row.fields.push_back(f);
    }

    return row;
}
