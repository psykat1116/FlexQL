#pragma once

#include <list>
#include <functional>
#include <unordered_map>
#include "storage/page.h"

using namespace std;

typedef struct
{
    uint32_t page_id;
    Page page;
    bool dirty;
} CacheEntry;

class LRUCache
{
public:
    explicit LRUCache(int capacity) : capacity(capacity) {};
    int Cache_Size() const { return (int)lru.size(); }

    Page *Get_Page(uint32_t page_id);
    void Mark_Dirty(uint32_t page_id);
    void Flush_All(function<void(uint32_t, const Page &)> flush_fn);
    void Put_Page(uint32_t page_id, const Page &page, bool dirty, function<void(uint32_t, const Page &)> flush_fn);

private:
    int capacity;
    list<CacheEntry> lru;
    unordered_map<uint32_t, list<CacheEntry>::iterator> map;
};
