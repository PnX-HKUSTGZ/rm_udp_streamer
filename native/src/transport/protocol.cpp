#include "protocol.hpp"
#include <algorithm>
#include <charconv>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace rm {
std::string Endpoint::text() const {
    return std::to_string(address >> 24) + "." + std::to_string((address >> 16) & 255) + "." +
        std::to_string((address >> 8) & 255) + "." + std::to_string(address & 255) + ":" + std::to_string(port);
}
std::optional<uint32_t> parse_ipv4(const std::string &address) {
    uint32_t result = 0;
    size_t begin = 0;
    for (int component = 0; component < 4; ++component) {
        size_t end = address.find('.', begin);
        if (component == 3) end = address.size();
        if (end == std::string::npos || end <= begin || end - begin > 3) return {};
        unsigned value = 0;
        auto parsed = std::from_chars(address.data() + begin, address.data() + end, value);
        if (parsed.ec != std::errc() || parsed.ptr != address.data() + end || value > 255) return {};
        result = (result << 8) | value;
        begin = end + 1;
    }
    return result;
}
std::string TransportConfig::validate() const {
    if (!parse_ipv4(bind_address)) return "bind_address must be a numeric IPv4 address";
    if (!max_frame_bytes || max_frame_bytes > 8 * 1024 * 1024) return "max_frame_bytes must be 1..8 MiB";
    if (!max_pending_frames || max_pending_frames > 64) return "max_pending_frames must be 1..64";
    if (!max_queue_frames || max_queue_frames > 64) return "max_queue_frames must be 1..64";
    if (!max_pending_bytes || max_pending_bytes > 256 * 1024 * 1024) return "max_pending_bytes must be 1..256 MiB";
    if (!max_queue_bytes || max_queue_bytes > 256 * 1024 * 1024) return "max_queue_bytes must be 1..256 MiB";
    if (sequence_window < max_pending_frames || sequence_window > 1024) return "sequence_window must be pending_frames..1024";
    if (deadline_ms < 1 || deadline_ms > 1000) return "deadline_ms must be 1..1000";
    if (startup_reorder_ms < 0 || startup_reorder_ms >= deadline_ms) return "startup_reorder_ms must be below deadline_ms";
    if (restart_idle_ms < deadline_ms * 2 || restart_idle_ms > 60000) return "restart_idle_ms must be 2*deadline_ms..60000";
    return {};
}
ProtocolCore::ProtocolCore(TransportConfig config, uint64_t generation) : config_(std::move(config)) {
    auto error = config_.validate();
    if (!error.empty()) throw std::invalid_argument(error);
    stats_.generation = generation;
}
void ProtocolCore::establish(uint16_t id, Endpoint source, Millis now, bool restart) {
    if (restart) { ++stats_.restarts; ++stats_.generation; stats_.queue_dropped += output_.size(); stats_.queue_restart_dropped += output_.size(); }
    entries_.clear(); output_.clear();
    stats_.pending_bytes = 0; stats_.queue_bytes = 0;
    stats_.source = source; stats_.source_locked = true;
    expected_ = initial_anchor_ = 65536ULL + id;
    anchored_ = true; emitted_ = false; discontinuity_ = true; consumer_gap_ = false;
    startup_at_ = now;
}
void ProtocolCore::invalidate(Entry &entry) {
    stats_.pending_bytes -= entry.allocation();
    std::vector<uint8_t>().swap(entry.data);
    std::vector<uint8_t>().swap(entry.received);
    entry.invalid = true; entry.complete = false;
}
void ProtocolCore::erase(std::map<uint64_t, Entry>::iterator item) {
    stats_.pending_bytes -= item->second.allocation();
    entries_.erase(item);
}
void ProtocolCore::note_oversized(size_t length) {
    ++stats_.packets; stats_.bytes += length; ++stats_.oversized; ++stats_.invalid;
}
void ProtocolCore::ingest(const uint8_t *packet, size_t length, Endpoint source, Millis now) {
    tick(now);
    if (length > datagram_bytes) { note_oversized(length); return; }
    ++stats_.packets; stats_.bytes += length;
    if (length <= header_bytes || !packet) { ++stats_.invalid; return; }
    auto u16 = [&](int offset) { return uint16_t((uint16_t(packet[offset]) << 8) | packet[offset+1]); };
    uint16_t id = u16(0), index = u16(2);
    uint32_t total = (uint32_t(packet[4]) << 24) | (uint32_t(packet[5]) << 16) |
        (uint32_t(packet[6]) << 8) | packet[7];
    if (!total || total > config_.max_frame_bytes) { ++stats_.invalid; return; }
    size_t count = (total + fragment_bytes - 1) / fragment_bytes;
    size_t offset = size_t(index) * fragment_bytes;
    if (index >= count || length - header_bytes != std::min(fragment_bytes, size_t(total) - offset)) {
        ++stats_.invalid; return;
    }
    if ((config_.source_address && config_.source_address != source.address) ||
        (config_.source_port && config_.source_port != source.port)) { ++stats_.foreign; return; }
    bool idle = anchored_ && now - stats_.last_packet_ms >= config_.restart_idle_ms;
    if (!anchored_ || idle) establish(id, source, now, idle);
    if (!(stats_.source == source)) { ++stats_.foreign; return; }
    uint16_t wrapped = uint16_t(id - uint16_t(expected_));
    int delta = wrapped < 32768 ? int(wrapped) : int(wrapped) - 65536;
    if (delta < 0 && !emitted_ && -delta <= config_.sequence_window) {
        uint64_t candidate = expected_ - uint64_t(-delta);
        if (candidate >= initial_anchor_ - config_.sequence_window &&
            (entries_.empty() || entries_.rbegin()->first - candidate <= config_.sequence_window)) {
            expected_ = candidate; delta = 0;
        }
    }
    if (delta < 0) { ++stats_.late; return; }
    if (delta > config_.sequence_window) { ++stats_.out_of_window; return; }
    uint64_t serial = expected_ + unsigned(delta);
    stats_.last_packet_ms = now;
    auto found = entries_.find(serial);
    if (found == entries_.end()) {
        size_t allocation = total + count;
        if (entries_.size() >= config_.max_pending_frames || allocation > config_.max_pending_bytes - stats_.pending_bytes) {
            ++stats_.budget_rejected;
            // Any rejected compressed input is observable as a reference-chain
            // discontinuity; never claim the next delivered chain is intact.
            discontinuity_ = true;
            return;
        }
        Entry entry;
        entry.data.resize(total); entry.received.resize(count); entry.first = now;
        stats_.pending_bytes += entry.allocation();
        found = entries_.emplace(serial, std::move(entry)).first;
        stats_.peak_pending_bytes = std::max(stats_.peak_pending_bytes, stats_.pending_bytes);
        stats_.peak_pending_frames = std::max(stats_.peak_pending_frames, entries_.size());
    }
    auto &entry = found->second;
    if (entry.invalid) { ++stats_.late; return; }
    if (entry.data.size() != total) { ++stats_.conflicts; invalidate(entry); drain(now); return; }
    if (entry.received[index]) {
        if (std::memcmp(entry.data.data() + offset, packet + header_bytes, length - header_bytes) != 0) {
            ++stats_.conflicts; invalidate(entry); drain(now);
        } else ++stats_.duplicate;
        return;
    }
    std::memcpy(entry.data.data() + offset, packet + header_bytes, length - header_bytes);
    entry.received[index] = 1;
    if (++entry.fragments == count) {
        entry.complete = true; entry.completed_at = now; ++stats_.complete;
        if (config_.diagnostics) stats_.assembly_time.observe((now-entry.first)*1000);
    }
    drain(now);
}
void ProtocolCore::emit(uint64_t serial, Entry &entry, Millis now) {
    EncodedGroup group;
    group.frame_id = uint16_t(serial); group.serial = serial; group.generation = stats_.generation;
    group.first_packet_ms = entry.first; group.complete_ms = entry.completed_at;
    group.queued_ms = now;
    if (config_.diagnostics) stats_.order_wait.observe((now-entry.completed_at)*1000);
    group.discontinuity_before = discontinuity_;
    size_t bytes = entry.data.size();
    if (bytes > config_.max_queue_bytes) {
        ++stats_.queue_dropped; ++stats_.queue_oversize_dropped; discontinuity_ = true; return;
    }
    if (output_.size() >= config_.max_queue_frames || bytes > config_.max_queue_bytes - stats_.queue_bytes) {
        stats_.queue_dropped += output_.size();
        stats_.queue_overflow_dropped += output_.size();
        output_.clear(); stats_.queue_bytes = 0;
        group.discontinuity_before = true;
    }
    stats_.pending_bytes -= entry.data.size();
    group.bytes = std::move(entry.data);
    stats_.queue_bytes += bytes;
    stats_.peak_queue_bytes = std::max(stats_.peak_queue_bytes, stats_.queue_bytes);
    output_.push_back(std::move(group));
    stats_.peak_queue_frames = std::max(stats_.peak_queue_frames, output_.size());
    discontinuity_ = false; emitted_ = true; ++stats_.ordered;
}
void ProtocolCore::drain(Millis now) {
    if (!anchored_ || (!emitted_ && now - startup_at_ < config_.startup_reorder_ms)) return;
    while (!entries_.empty()) {
        auto found = entries_.find(expected_);
        if (found == entries_.end()) {
            auto nearest = entries_.begin();
            Millis first_future = nearest->second.first;
            for (const auto &item : entries_) first_future = std::min(first_future, item.second.first);
            if (now - first_future < config_.deadline_ms) break;
            stats_.missing += nearest->first - expected_;
            expected_ = nearest->first; discontinuity_ = true;
            continue;
        }
        auto &entry = found->second;
        if (entry.invalid || now - entry.first >= config_.deadline_ms) {
            if (!entry.invalid) ++stats_.timeouts;
            discontinuity_ = true; ++stats_.missing;
            erase(found); ++expected_; emitted_ = true;
        } else if (entry.complete) {
            emit(expected_, entry, now);
            erase(found); ++expected_; emitted_ = true;
        } else break;
    }
}
void ProtocolCore::expire_output(Millis now) {
    while (!output_.empty() && now - output_.front().first_packet_ms >= config_.deadline_ms) {
        stats_.queue_bytes -= output_.front().bytes.size();
        output_.pop_front(); ++stats_.queue_expired; consumer_gap_ = true;
    }
}
void ProtocolCore::tick(Millis now) {
    // Retain bounded tombstones so a late duplicate cannot recreate an expired
    // future group while the earlier gap is still being sequenced.
    for (auto &item : entries_) {
        auto &entry = item.second;
        if (!entry.invalid && now - entry.first >= config_.deadline_ms) {
            ++stats_.timeouts; invalidate(entry);
        }
    }
    drain(now);
    expire_output(now);
}
std::optional<EncodedGroup> ProtocolCore::pop(Millis now) {
    tick(now);
    if (output_.empty()) return {};
    auto result = std::move(output_.front()); output_.pop_front();
    result.dequeued_ms = now;
    if (config_.diagnostics) stats_.compressed_queue_wait.observe((now-result.queued_ms)*1000);
    stats_.queue_bytes -= result.bytes.size(); ++stats_.consumed;
    if (consumer_gap_) { result.discontinuity_before = true; consumer_gap_ = false; }
    return result;
}
TransportStats ProtocolCore::stats(Millis now) const {
    auto result = stats_;
    result.pending_frames = entries_.size(); result.queue_frames = output_.size();
    for (const auto &item : entries_) result.pending_oldest_ms = std::max(result.pending_oldest_ms, now-item.second.first);
    for (const auto &item : output_) result.queue_oldest_ms = std::max(result.queue_oldest_ms, now-item.first_packet_ms);
    return result;
}
}
