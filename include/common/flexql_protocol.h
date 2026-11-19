#pragma once
#include <cstdint>

static const uint32_t MSG_QUERY = 1;
static const uint32_t MSG_ROW_HEADER = 2;
static const uint32_t MSG_ROW = 3;
static const uint32_t MSG_STATUS = 4;

// Largest query (MSG_QUERY payload) the server will accept, in bytes.
// The server rejects anything larger before allocating a buffer for it.
// 64 MiB leaves plenty of room for large batched INSERTs (a 10,000-row
// benchmark batch is ~1 MiB).
static const uint32_t MAX_QUERY_LENGTH = 64u * 1024u * 1024u;

#pragma pack(push, 1)
typedef struct
{
    uint32_t type;
    uint32_t length;
} MessageHeader;
#pragma pack(pop)