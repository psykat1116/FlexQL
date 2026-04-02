#pragma once
#include <cstdint>

static const uint32_t MSG_QUERY = 1;
static const uint32_t MSG_ROW_HEADER = 2;
static const uint32_t MSG_ROW = 3;
static const uint32_t MSG_STATUS = 4;

#pragma pack(push, 1)
typedef struct
{
    uint32_t type;
    uint32_t length;
} MessageHeader;
#pragma pack(pop)