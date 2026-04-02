#include "cache/lru_cache.h"
using namespace std;

Page *LRUCache::Get_Page(uint32_t page_id)
{
    auto it = map.find(page_id);
    if (it == map.end())
        return nullptr;

    lru.splice(lru.begin(), lru, it->second);
    return &it->second->page;
}

void LRUCache::Mark_Dirty(uint32_t page_id)
{
    auto it = map.find(page_id);
    if (it != map.end())
        it->second->dirty = true;
}

void LRUCache::Flush_All(function<void(uint32_t, const Page &)> flush_fn)
{
    for (auto &e : lru)
    {
        if (e.dirty)
        {
            flush_fn(e.page_id, e.page);
            e.dirty = false;
        }
    }
}

void LRUCache::Put_Page(uint32_t page_id, const Page &page, bool dirty, function<void(uint32_t, const Page &)> flush_fn)
{
    auto it = map.find(page_id);
    if (it != map.end())
    {
        it->second->page = page;
        it->second->dirty = dirty;
        lru.splice(lru.begin(), lru, it->second);
        return;
    }
    if ((int)lru.size() >= capacity)
    {
        auto &back = lru.back();
        if (back.dirty)
            flush_fn(back.page_id, back.page);
        map.erase(back.page_id);
        lru.pop_back();
    }

    lru.push_front({page_id, page, dirty});
    map[page_id] = lru.begin();
}