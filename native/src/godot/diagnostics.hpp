#pragma once
#include "media/decoder.hpp"
#include "diagnostics/submission_windows.hpp"
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/dictionary.hpp>

namespace rm::binding {
inline godot::Variant quantile(const TimingHistogram &h,unsigned p) {
    auto value=h.percentile(p);return value<0?godot::Variant():godot::Variant(int64_t(value));
}
inline godot::Dictionary histogram(const TimingHistogram &h) {
    godot::Dictionary d;
    d["count"]=int64_t(h.count);d["sum_us"]=int64_t(h.sum_us);d["min_us"]=h.min_us;d["max_us"]=h.max_us;
    d["p50_us"]=quantile(h,50);d["p95_us"]=quantile(h,95);d["p99_us"]=quantile(h,99);
    d["percentile_upper_bound"]=true;d["overflow"]=int64_t(h.buckets.back());d["invalid"]=int64_t(h.invalid);
    godot::Array bins;
    for(size_t i=0;i<h.bucket_count;++i) if(h.buckets[i]) {
        godot::Array bin;bin.push_back(int64_t(i));bin.push_back(int64_t(h.buckets[i]));bins.push_back(bin);
    }
    d["bins"]=bins;return d;
}
inline godot::Dictionary frame(const FrameInfo &info) {
    godot::Dictionary d;
    d["sequence"]=int64_t(info.output_sequence);d["stream"]=int64_t(info.stream_generation);
    d["resource"]=int64_t(info.resource_generation);d["presentation"]=int64_t(info.presentation_generation);
    d["timing_enabled"]=info.timing.enabled;d["correlated"]=info.timing.correlated();
    d["correlation"]=correlation_name(info.timing.correlation);
    if(info.timing.correlated()){d["input_serial"]=int64_t(info.timing.input_serial);d["input_frame_id"]=int64_t(info.timing.input_frame_id);}
#define TIME(name) d[#name]=int64_t(info.timing.name)
    TIME(ingress_us);TIME(complete_us);TIME(queued_us);TIME(dequeued_us);TIME(packet_ready_us);
    TIME(decoded_us);TIME(prepared_us);TIME(upload_queued_us);TIME(upload_started_us);TIME(upload_completed_us);TIME(submitted_us);TIME(shown_us);
#undef TIME
    return d;
}
inline godot::Dictionary windows(const SubmissionWindows &value,Micros now) {
    godot::Dictionary d;godot::Array closed;
    auto current=value.current(now);auto first=value.first_closed(now);
    for(auto index=first;index<current;++index){
        godot::Array window;window.push_back(index);window.push_back(int64_t(value.count(index)));closed.push_back(window);
    }
    d["origin_us"]=value.origin_us;d["started"]=value.started;d["first_index"]=first;
    d["current_index"]=current;d["closed"]=closed;d["capacity"]=int64_t(value.capacity);d["invalid"]=int64_t(value.invalid);
    return d;
}
}
