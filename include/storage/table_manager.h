#pragma once
#include <mutex>
#include <string>
#include <utility>
#include <functional>

#include "common/types.h"
#include "storage/page.h"
#include "cache/lru_cache.h"
#include "index/bplus_tree.h"

class TableManager
{
public:
    mutex table_mutex;
    TableSchema schema;

    TableManager(const string &dir, const string &table_name, const TableSchema &sc, bool create_new);
    ~TableManager();

    void Delete_All();
    void Flush_Table();
    uint32_t New_Page();
    uint32_t Row_Count() const;
    uint32_t Page_Count() const;
    Page &Fetch_Page(uint32_t page_id);
    void Scan(function<bool(const Row &)> cb);
    bool Find_By_FK(int32_t key, Row &out_row);
    pair<uint32_t, int> Insert_Row(const Row &row);
    int Delete_Where(function<bool(const Row &)> pred);

private:
    static constexpr int CACHE_CAPACITY = 4096; // 4096 × 16KB = 64 MB
    static constexpr int PREALLOC_PAGES = 256;  // extend file in chunks

    string directory;
    string table_path;
    string index_path;
    string schema_path;

    int table_fd = -1;
    uint32_t number_of_pages = 0;
    uint32_t allocated_pages = 0;

    LRUCache cache;
    BPlusTree btree;

    void Close_FD();
    void Ensure_Capacity();
    void Open_FD(bool create);
    void Read_From_Disk(uint32_t page_id, Page &pg);
    void Write_To_Disk(uint32_t page_id, const Page &pg);
};
