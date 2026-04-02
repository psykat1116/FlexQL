#include <cassert>
#include <cstring>
#include <fstream>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

#include "expiration/expiry.h"
#include "storage/table_manager.h"

void TableManager::Open_FD(bool create)
{
    if (table_fd >= 0)
        return;

    int flags = create ? (O_RDWR | O_CREAT | O_TRUNC) : (O_RDWR | O_CREAT);
    table_fd = ::open(table_path.c_str(), flags, 0644);
    if (table_fd < 0)
        table_fd = ::open(table_path.c_str(), O_RDWR | O_CREAT, 0644);
}

void TableManager::Close_FD()
{
    if (table_fd >= 0)
    {
        ::close(table_fd);
        table_fd = -1;
    }
}

void TableManager::Ensure_Capacity()
{
    if (number_of_pages <= allocated_pages)
        return;

    allocated_pages = number_of_pages + PREALLOC_PAGES;
    ::ftruncate(table_fd, (off_t)allocated_pages * PAGE_SIZE);
}

TableManager::TableManager(const std::string &dir, const std::string &table_name, const TableSchema &sc, bool create_new) : cache(CACHE_CAPACITY), directory(dir)
{
    schema = sc;
    table_path = dir + "/" + table_name + ".tbl";
    index_path = dir + "/" + table_name + ".idx";
    schema_path = dir + "/" + table_name + ".schema";

    if (create_new)
    {
        std::ofstream sf(schema_path, std::ios::binary | std::ios::trunc);
        sf.write(reinterpret_cast<const char *>(&schema), sizeof(TableSchema));
        Open_FD(true);
        number_of_pages = 0;
        allocated_pages = 0;
    }
    else
    {
        std::ifstream sf(schema_path, std::ios::binary);
        sf.read(reinterpret_cast<char *>(&schema), sizeof(TableSchema));

        Open_FD(false);
        if (table_fd >= 0)
        {
            off_t sz = ::lseek(table_fd, 0, SEEK_END);
            number_of_pages = (sz > 0) ? (uint32_t)(sz / PAGE_SIZE) : 0;
            allocated_pages = number_of_pages;
        }

        btree.Load_Tree(index_path);
    }
}

TableManager::~TableManager()
{
    Flush_Table();
    Close_FD();
}

void TableManager::Flush_Table()
{
    cache.Flush_All([this](uint32_t pid, const Page &pg)
                    { Write_To_Disk(pid, pg); });
    if (table_fd >= 0 && number_of_pages < allocated_pages)
    {
        ::ftruncate(table_fd, (off_t)number_of_pages * PAGE_SIZE);
        allocated_pages = number_of_pages;
    }

    btree.Save_Tree(index_path);
}

Page &TableManager::Fetch_Page(uint32_t page_id)
{
    Page *p = cache.Get_Page(page_id);
    if (p)
        return *p;

    Page pg;
    Read_From_Disk(page_id, pg);
    cache.Put_Page(page_id, pg, false, [this](uint32_t pid, const Page &pp)
                   { Write_To_Disk(pid, pp); });

    return *cache.Get_Page(page_id);
}

uint32_t TableManager::New_Page()
{
    uint32_t pid = number_of_pages++;
    Ensure_Capacity();

    Page pg;
    pg.Initialize_Page(pid);
    cache.Put_Page(pid, pg, true, [this](uint32_t p, const Page &pp)
                   { Write_To_Disk(p, pp); });

    return pid;
}

std::pair<uint32_t, int> TableManager::Insert_Row(const Row &row)
{
    auto bytes = Serialize_Row(row, schema);
    if (number_of_pages > 0)
    {
        uint32_t pid = number_of_pages - 1;
        Page &pg = Fetch_Page(pid);
        int slot = pg.Insert_Row(bytes.data(), (uint16_t)bytes.size());
        if (slot >= 0)
        {
            cache.Mark_Dirty(pid);
            int pk = schema.Primary_Key_Column();
            btree.Insert_Key(row.fields[pk].integer_value, pid, (uint16_t)slot);
            return {pid, slot};
        }
    }

    uint32_t pid = New_Page();
    Page &pg = Fetch_Page(pid);
    int slot = pg.Insert_Row(bytes.data(), (uint16_t)bytes.size());

    assert(slot >= 0);
    cache.Mark_Dirty(pid);
    int pk = schema.Primary_Key_Column();
    btree.Insert_Key(row.fields[pk].integer_value, pid, (uint16_t)slot);

    return {pid, slot};
}

bool TableManager::Find_By_FK(int32_t key, Row &out_row)
{
    IndexRecord rec;
    if (!btree.Find_Key(key, rec))
        return false;

    Page &pg = Fetch_Page(rec.page_id);
    const uint8_t *raw = pg.getRow(rec.slot_idx);
    if (!raw)
        return false;

    out_row = Deserialize_Row(raw, schema);
    return !Has_Expired(out_row.expiry_timestamp);
}

void TableManager::Scan(std::function<bool(const Row &)> cb)
{
    for (uint32_t pid = 0; pid < number_of_pages; pid++)
    {
        Page &pg = Fetch_Page(pid);
        for (int s = 0; s < pg.Number_Of_Slots(); s++)
        {
            const uint8_t *raw = pg.getRow(s);
            if (!raw)
                continue;
            Row row = Deserialize_Row(raw, schema);
            if (Has_Expired(row.expiry_timestamp))
                continue;
            if (!cb(row))
                return;
        }
    }
}

void TableManager::Delete_All()
{
    cache.Flush_All([](uint32_t, const Page &) {});
    if (table_fd >= 0)
        ::ftruncate(table_fd, 0);

    number_of_pages = 0;
    allocated_pages = 0;
    btree.Clear_Tree();
}

int TableManager::Delete_Where(std::function<bool(const Row &)> pred)
{
    std::vector<Row> survivors;
    Scan([&](const Row &row) -> bool
         {
        if (!pred(row)) survivors.push_back(row);
        return true; });

    int deleted = (int)btree.Get_Size() - (int)survivors.size();
    cache.Flush_All([](uint32_t, const Page &) {});
    if (table_fd >= 0)
        ::ftruncate(table_fd, 0);

    number_of_pages = 0;
    allocated_pages = 0;
    btree.Clear_Tree();
    for (auto &row : survivors)
        Insert_Row(row);

    return deleted;
}

uint32_t TableManager::Page_Count() const { return number_of_pages; }
uint32_t TableManager::Row_Count() const { return (uint32_t)btree.Get_Size(); }

void TableManager::Write_To_Disk(uint32_t page_id, const Page &pg)
{
    if (table_fd < 0)
        Open_FD(false);

    ::pwrite(table_fd, pg.data, PAGE_SIZE, (off_t)page_id * PAGE_SIZE);
}

void TableManager::Read_From_Disk(uint32_t page_id, Page &pg)
{
    if (table_fd < 0)
    {
        pg.Initialize_Page(page_id);
        return;
    }

    ssize_t n = ::pread(table_fd, pg.data, PAGE_SIZE, (off_t)page_id * PAGE_SIZE);
    if (n != (ssize_t)PAGE_SIZE)
        pg.Initialize_Page(page_id);
}
