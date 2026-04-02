#pragma once
#include <ctime>
#include <cstdint>

using namespace std;

inline bool Has_Expired(uint64_t expiry_ts) {
    if (expiry_ts == 0) return false;
    return (uint64_t)time(nullptr) >= expiry_ts;
}

inline uint64_t Make_Expiry(uint64_t seconds_from_now) {
    if (seconds_from_now == 0) return 0;
    return (uint64_t)time(nullptr) + seconds_from_now;
}
