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

    ~TableManager();
    TableManager(const string &dir, const string &table_name, const TableSchema &sc, bool create_new);

    void Delete_All();
    void Flush_Table();
    PageGuard New_Page();
    PageGuard Fetch_Page(uint32_t page_id);
    void Scan(function<bool(const Row &)> cb);
    bool Find_By_FK(int32_t key, Row &out_row);
    vector<Row> Find_By_Range(int32_t lo, bool lo_inc, int32_t hi, bool hi_inc);
    pair<uint32_t, int> Insert_Row(const Row &row);
    int Delete_Where(function<bool(const Row &)> pred);
    int Purge_Expired();
    void Rebuild_Index();

    uint32_t Page_Count() const { return number_of_pages; };
    uint32_t Row_Count() const { return (uint32_t)btree.Get_Size(); };

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
    LRUCache::FlushFn Write_Back();
    void Ensure_Capacity();
    void Open_FD(bool create);
    void Read_From_Disk(uint32_t page_id, Page &pg);
    void Write_To_Disk(uint32_t page_id, const Page &pg);
};