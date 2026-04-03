#pragma once
#include <vector>
#include <map>
#include <string>
#include <cstdint>

using namespace std;

typedef struct
{
    uint32_t page_id;
    uint16_t slot_idx;
} IndexRecord;

class BPlusTree
{
private:
    map<int32_t, IndexRecord> index;

public:
    void Clear_Tree() { index.clear(); }
    size_t Get_Size() const { return index.size(); }
    void Remove_Key(int32_t key) { index.erase(key); }
    void Insert_Key(int32_t key, uint32_t page_id, uint16_t slot) { index[key] = {page_id, slot}; }

    void Load_Tree(const string &path);
    void Save_Tree(const string &path) const;
    bool Find_Key(int32_t key, IndexRecord &rec) const;
    vector<IndexRecord> Find_Range(int32_t lo, bool lo_inclusive, int32_t hi, bool hi_inclusive) const;
};