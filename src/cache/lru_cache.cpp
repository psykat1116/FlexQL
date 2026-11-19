#include "cache/lru_cache.h"
using namespace std;

PageGuard LRUCache::Pin_Page(uint32_t page_id)
{
    auto it = map.find(page_id);
    if (it == map.end())
        return PageGuard();

    lru.splice(lru.begin(), lru, it->second);
    it->second->pin_count++;
    return PageGuard(this, &*it->second);
}

void LRUCache::Evict_To_Fit(const FlushFn &flush_fn)
{
    // Walk from the LRU end, skipping pages someone still holds a guard on.
    auto it = lru.end();
    while ((int)lru.size() >= capacity && it != lru.begin())
    {
        --it;
        if (it->pin_count > 0)
            continue;

        if (it->dirty)
            flush_fn(it->page_id, it->page);

        map.erase(it->page_id);
        it = lru.erase(it); // returns the node after the erased one; the next --it moves on
    }
}

PageGuard LRUCache::Insert_Pinned(uint32_t page_id, bool dirty, const FlushFn &flush_fn)
{
    auto existing = map.find(page_id);
    if (existing != map.end())
    {
        // Re-initialising a page id that is still cached (e.g. after a table
        // truncate). Reuse the node; the caller overwrites its bytes.
        CacheEntry &e = *existing->second;
        lru.splice(lru.begin(), lru, existing->second);
        e.dirty = dirty;
        e.pin_count++;
        return PageGuard(this, &e);
    }

    Evict_To_Fit(flush_fn);

    lru.emplace_front(page_id, dirty);
    map[page_id] = lru.begin();
    lru.front().pin_count = 1;
    return PageGuard(this, &lru.front());
}

void LRUCache::Discard(uint32_t page_id)
{
    auto it = map.find(page_id);
    if (it == map.end())
        return;

    lru.erase(it->second);
    map.erase(it);
}

void LRUCache::Flush_All(const FlushFn &flush_fn)
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
