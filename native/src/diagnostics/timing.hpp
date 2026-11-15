#pragma once
#include <array>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <limits>

namespace rm {
using Micros = int64_t;
inline Micros monotonic_us() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

// Fixed buckets: zero; 100 us through 10 ms; 1 ms through 100 ms;
// 100 ms through 1 s; overflow. Percentiles are conservative bucket upper
// bounds, never fabricated exact sample quantiles. Owner supplies locking.
struct TimingHistogram {
    static constexpr size_t bucket_count = 201;
    std::array<uint64_t, bucket_count> buckets{};
    uint64_t count = 0, sum_us = 0, invalid = 0;
    Micros min_us = 0, max_us = 0;
    static size_t bucket(Micros value) {
        if (value <= 0) return 0;
        if (value <= 10000) return size_t((value+99)/100);
        if (value <= 100000) return 100+size_t((value-10000+999)/1000);
        if (value <= 1000000) return 190+size_t((value-100000+99999)/100000);
        return 200;
    }
    static Micros upper_bound(size_t index) {
        if (index <= 100) return Micros(index)*100;
        if (index <= 190) return 10000+Micros(index-100)*1000;
        if (index < 200) return 100000+Micros(index-190)*100000;
        return -1; // Beyond representable range: report unknown, with max.
    }
    void observe(Micros value) {
        if (value < 0) { ++invalid; return; }
        if (!count) min_us = max_us = value;
        else { min_us = std::min(min_us,value); max_us = std::max(max_us,value); }
        ++count; sum_us += uint64_t(value); ++buckets[bucket(value)];
    }
    Micros percentile(unsigned percent) const {
        if (!count || !percent || percent > 100) return -1;
        // Avoid count*percent overflow for a process-lifetime counter.
        uint64_t rank = (count/100)*percent + ((count%100)*percent+99)/100;
        uint64_t cumulative = 0;
        for (size_t i=0;i<bucket_count;++i) {
            cumulative += buckets[i];
            if (cumulative >= rank) return upper_bound(i);
        }
        return -1;
    }
};
static_assert(sizeof(TimingHistogram) <= 1664, "Timing storage must stay bounded");

class ScopedTiming {
    TimingHistogram *histogram_;
    Micros started_;
public:
    explicit ScopedTiming(TimingHistogram *histogram)
        : histogram_(histogram), started_(histogram ? monotonic_us() : 0) {}
    ~ScopedTiming() { if (histogram_) histogram_->observe(monotonic_us()-started_); }
    ScopedTiming(const ScopedTiming &) = delete;
    ScopedTiming &operator=(const ScopedTiming &) = delete;
};
}
