#pragma once

#include <list>
#include <utility>
#include <functional>
#include <unordered_map>
#include "storage/page.h"

using namespace std;

struct CacheEntry
{
    uint32_t page_id;
    bool dirty;
    int pin_count;
    Page page; // left uninitialised on purpose: the caller fills it (disk read or Initialize_Page)

    CacheEntry(uint32_t id, bool d) : page_id(id), dirty(d), pin_count(0) {}
};

class LRUCache;

// RAII handle to a cached page. While a PageGuard is alive its page is pinned
// and can never be evicted, so the Page reference it hands out stays valid.
// The pin is released when the guard is destroyed (or Release() is called).
class PageGuard
{
public:
    PageGuard() = default;
    PageGuard(LRUCache *cache, CacheEntry *entry) : cache(cache), entry(entry) {}
    ~PageGuard() { Release(); }

    PageGuard(const PageGuard &) = delete;
    PageGuard &operator=(const PageGuard &) = delete;
    PageGuard(PageGuard &&o) noexcept : cache(o.cache), entry(o.entry) { o.entry = nullptr; }
    PageGuard &operator=(PageGuard &&o) noexcept
    {
        if (this != &o)
        {
            Release();
            cache = o.cache;
            entry = o.entry;
            o.entry = nullptr;
        }
        return *this;
    }

    explicit operator bool() const { return entry != nullptr; }
    Page &operator*() const { return entry->page; }
    Page *operator->() const { return &entry->page; }
    uint32_t Page_Id() const { return entry->page_id; }
    void Mark_Dirty() { entry->dirty = true; }
    void Release();

private:
    LRUCache *cache = nullptr;
    CacheEntry *entry = nullptr;
};

class LRUCache
{
public:
    using FlushFn = function<void(uint32_t, const Page &)>;

    explicit LRUCache(int capacity) : capacity(capacity) {};
    int Cache_Size() const { return (int)lru.size(); }

    // Cache hit: pin the page, move it to the MRU end, and return a guard.
    // Cache miss: returns an empty guard (check with Is_Hit / bool conversion).
    PageGuard Pin_Page(uint32_t page_id);

    // Create a new pinned entry for a page that is not cached, constructing it
    // in place inside the list node (no 16 KB temporary copy). The page bytes
    // are uninitialised: the caller must fill them before releasing the guard.
    // If the cache is full, the least-recently-used *unpinned* page is evicted
    // (written back via flush_fn if dirty). If every page is pinned, the cache
    // temporarily grows past capacity and shrinks again on later inserts.
    PageGuard Insert_Pinned(uint32_t page_id, bool dirty, const FlushFn &flush_fn);

    // Drop an entry that was never filled (e.g. its disk read failed).
    void Discard(uint32_t page_id);

    void Flush_All(const FlushFn &flush_fn);

private:
    friend class PageGuard;
    void Unpin(CacheEntry *entry) { entry->pin_count--; }
    void Evict_To_Fit(const FlushFn &flush_fn);

    int capacity;
    list<CacheEntry> lru; // front = most recently used, back = least recently used
    unordered_map<uint32_t, list<CacheEntry>::iterator> map;
};

inline void PageGuard::Release()
{
    if (entry)
    {
        cache->Unpin(entry);
        entry = nullptr;
    }
}
