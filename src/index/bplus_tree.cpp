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

void BPlusTree::Save_Tree(const string &path) const
{
    uint32_t n = (uint32_t)index.size();
    ofstream f(path, ios::binary | ios::trunc);

    f.write(reinterpret_cast<const char *>(&n), 4);
    for (auto &[k, v] : index)
    {
        f.write(reinterpret_cast<const char *>(&k), 4);
        f.write(reinterpret_cast<const char *>(&v.page_id), 4);
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

        f.read(reinterpret_cast<char *>(&k), 4);
        f.read(reinterpret_cast<char *>(&r.page_id), 4);
        f.read(reinterpret_cast<char *>(&r.slot_idx), 2);

        index[k] = r;
    }
}
