#pragma once
#include "timing.hpp"
namespace rm {
// Exact one-second count windows, anchored by the first accepted submission.
// A reader samples at most once a second and deduplicates indices. History is
// capped at 63 completed seconds plus the current second; a longer reader gap
// is observable, not filled in. The current bucket can overwrite index N-64.
struct SubmissionWindows {
    static constexpr size_t capacity=64;
    struct Bucket { int64_t index=-1; uint64_t count=0; };
    std::array<Bucket,capacity> buckets{};
    Micros origin_us=0;
    bool started=false;
    uint64_t invalid=0;
    void add(Micros now) {
        if(!started){started=true;origin_us=now;}
        if(now<origin_us){++invalid;return;}
        int64_t index=(now-origin_us)/1000000;
        auto &bucket=buckets[size_t(index)%capacity];
        if(bucket.index!=index)bucket={index,0};
        ++bucket.count;
    }
    int64_t current(Micros now) const { return started&&now>=origin_us?(now-origin_us)/1000000:0; }
    int64_t first_closed(Micros now) const { return std::max(int64_t(0),current(now)-int64_t(capacity-1)); }
    uint64_t count(int64_t index) const {
        if(index<0)return 0;
        const auto &bucket=buckets[size_t(index)%capacity];
        return bucket.index==index?bucket.count:0;
    }
};
}
