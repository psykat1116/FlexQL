#pragma once
#include <cstring>
#include <cassert>
#include "common/types.h"

#pragma pack(push, 1)
typedef struct
{
    uint16_t offset;
    uint16_t length;
} SlotEntry;

typedef struct
{
    uint32_t page_id;
    uint32_t next_page;
    uint16_t num_slots;
    uint16_t free_space_offset;

    uint8_t flags;
    uint8_t pad[3];
} PageHeader;
#pragma pack(pop)

struct Page
{
    uint8_t data[PAGE_SIZE];
    int Number_Of_Slots() const { return Header()->num_slots; }
    PageHeader *Header() { return reinterpret_cast<PageHeader *>(data); }
    const PageHeader *Header() const { return reinterpret_cast<const PageHeader *>(data); }

    SlotEntry *Get_Slot(int i)
    {
        return reinterpret_cast<SlotEntry *>(data + sizeof(PageHeader)) + i;
    }

    const SlotEntry *Get_Slot(int i) const
    {
        return reinterpret_cast<const SlotEntry *>(data + sizeof(PageHeader)) + i;
    }

    void Initialize_Page(uint32_t id)
    {
        memset(data, 0, PAGE_SIZE);
        auto *h = Header();

        h->flags = 0;
        h->page_id = id;
        h->num_slots = 0;
        h->next_page = INVALID_PAGE;
        h->free_space_offset = PAGE_SIZE;
    }

    uint32_t Free_Space() const
    {
        uint32_t slot_end = sizeof(PageHeader) + Header()->num_slots * sizeof(SlotEntry);
        if (Header()->free_space_offset < slot_end)
            return 0;

        return Header()->free_space_offset - slot_end;
    }

    int Insert_Row(const uint8_t *bytes, uint16_t len)
    {
        if ((uint32_t)(len + sizeof(SlotEntry)) > Free_Space())
            return -1;

        uint16_t off = Header()->free_space_offset - len;
        memcpy(data + off, bytes, len);

        int idx = Header()->num_slots;
        Get_Slot(idx)->offset = off;
        Get_Slot(idx)->length = len;

        Header()->num_slots++;
        Header()->free_space_offset = off;

        return idx;
    }

    const uint8_t *getRow(int i) const
    {
        if (i >= Header()->num_slots)
            return nullptr;

        return data + Get_Slot(i)->offset;
    }
};

Field Make_Field(const string &val, const ColSchema &col);
Row Deserialize_Row(const uint8_t *raw, const TableSchema &sc);
vector<uint8_t> Serialize_Row(const Row &row, const TableSchema &sc);