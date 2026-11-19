#include <climits>
#include <fstream>
#include "index/bplus_tree.h"

using namespace std;

bool BPlusTree::Find_Key(int32_t key, IndexRecord &rec) const
{
    auto it = index.find(key);
    if (it == index.end())
        return false;

    rec = it->second;
    return true;
}

// Walk only the entries whose key satisfies lo <[=] k <[=] hi.
// Uses std::map::lower_bound / upper_bound so cost is O(log N + K)
vector<IndexRecord> BPlusTree::Find_Range(int32_t lo, bool lo_inclusive, int32_t hi, bool hi_inclusive) const
{
    vector<IndexRecord> results;
    
    auto it = lo_inclusive ? index.lower_bound(lo) : index.upper_bound(lo);
    for (; it != index.end(); ++it)
    {
        if (hi_inclusive ? it->first > hi : it->first >= hi)
            break;
        
        results.push_back(it->second);
    }

    return results;
}

void BPlusTree::Save_Tree(const string &path) const
{
    uint32_t n = (uint32_t)index.size();
    ofstream f(path, ios::binary | ios::trunc);

    f.write(reinterpret_cast<const char *>(&n), 4);
    for (auto &[k, v] : index)
    {
        f.write(reinterpret_cast<const char *>(&k),         4);
        f.write(reinterpret_cast<const char *>(&v.page_id),  4);
        f.write(reinterpret_cast<const char *>(&v.slot_idx), 2);
    }
}

void BPlusTree::Load_Tree(const string &path)
{
    ifstream f(path, ios::binary);
    if (!f.is_open())
        return;

    uint32_t n;
    f.read(reinterpret_cast<char *>(&n), 4);
    for (uint32_t i = 0; i < n; i++)
    {
        int32_t k;
        IndexRecord r;

        f.read(reinterpret_cast<char *>(&k),         4);
        f.read(reinterpret_cast<char *>(&r.page_id),  4);
        f.read(reinterpret_cast<char *>(&r.slot_idx), 2);

        index[k] = r;
    }
}