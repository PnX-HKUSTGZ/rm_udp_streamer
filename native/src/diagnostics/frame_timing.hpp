#pragma once
#include "timing.hpp"
#include <cstddef>

namespace rm {
enum class Correlation { Disabled, ExactGroup, ParserBoundary, HistoryEvicted, MissingClock, NotSinglePicture, MissingOpaque };
inline const char *correlation_name(Correlation reason) {
    switch (reason) {
        case Correlation::ExactGroup: return "exact_group";
        case Correlation::ParserBoundary: return "parser_split_or_join";
        case Correlation::HistoryEvicted: return "history_budget";
        case Correlation::MissingClock: return "missing_input_clock";
        case Correlation::NotSinglePicture: return "not_single_picture";
        case Correlation::MissingOpaque: return "missing_decoder_opaque";
        default: return "disabled";
    }
}
struct FrameTiming {
    bool enabled = false;
    Correlation correlation = Correlation::Disabled;
    uint64_t input_serial = 0;
    uint16_t input_frame_id = 0;
    Micros ingress_us = 0, complete_us = 0, queued_us = 0, dequeued_us = 0;
    Micros packet_ready_us = 0, decoded_us = 0, prepared_us = 0;
    Micros upload_queued_us = 0, upload_started_us = 0, submitted_us = 0, shown_us = 0;
    bool correlated() const { return correlation == Correlation::ExactGroup; }
    void unknown(Correlation reason) {
        correlation = reason; input_serial = input_frame_id = 0;
        ingress_us = complete_us = queued_us = dequeued_us = 0;
    }
};

// Diagnostic byte-range history has a separate fixed budget from parser data.
// Losing timing provenance must never drop compressed input or invent a match.
class InputHistory {
public:
    static constexpr size_t capacity = 128;
    struct Span { int64_t begin = 0, end = 0; FrameTiming timing; };
private:
    std::array<Span,capacity> spans_{};
    size_t head_ = 0, count_ = 0;
    int64_t lost_until_ = 0;
public:
    uint64_t evicted = 0;
    size_t peak = 0;
    void clear() { head_ = count_ = 0; lost_until_ = 0; }
    size_t size() const { return count_; }
    void add(Span span) {
        if (count_ == capacity) {
            lost_until_ = spans_[head_].end;
            head_ = (head_+1)%capacity; --count_; ++evicted;
        }
        spans_[(head_+count_)%capacity] = span; ++count_; peak = std::max(peak,count_);
    }
    FrameTiming take(int64_t begin, int64_t end, int length) {
        FrameTiming result; result.enabled = true;
        result.correlation = begin < lost_until_ ? Correlation::HistoryEvicted : Correlation::ParserBoundary;
        if (begin >= 0 && end >= begin && end-begin == length) {
            for (size_t i=0;i<count_;++i) {
                const auto &span = spans_[(head_+i)%capacity];
                if (span.begin == begin && span.end == end) { result = span.timing; break; }
            }
        }
        while (count_ && spans_[head_].end <= end) { head_ = (head_+1)%capacity; --count_; }
        return result;
    }
};
}
