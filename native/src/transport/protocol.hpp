#pragma once
#include "diagnostics/timing.hpp"
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace rm {
using Millis = int64_t;
inline Millis monotonic_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
constexpr size_t header_bytes = 8;
constexpr size_t fragment_bytes = 1392;
constexpr size_t datagram_bytes = header_bytes + fragment_bytes;

struct Endpoint {
    uint32_t address = 0; // IPv4 in host byte order; never inferred from MQTT.
    uint16_t port = 0;
    bool operator==(const Endpoint &other) const { return address == other.address && port == other.port; }
    std::string text() const;
};
std::optional<uint32_t> parse_ipv4(const std::string &address);

struct TransportConfig {
    bool diagnostics = false;
    std::string bind_address = "0.0.0.0";
    uint16_t port = 3334;
    uint32_t source_address = 0; // 0 permits first valid sender, then locks its tuple.
    uint16_t source_port = 0;
    size_t max_frame_bytes = 8 * 1024 * 1024;
    size_t max_pending_frames = 8;
    size_t max_pending_bytes = 32 * 1024 * 1024;
    size_t max_queue_frames = 4;
    size_t max_queue_bytes = 16 * 1024 * 1024;
    uint16_t sequence_window = 64;
    Millis deadline_ms = 50;
    Millis startup_reorder_ms = 5;
    Millis restart_idle_ms = 1000;
    std::string validate() const;
};

struct EncodedGroup {
    std::vector<uint8_t> bytes;
    uint16_t frame_id = 0;
    uint64_t serial = 0;
    uint64_t generation = 0;
    Millis first_packet_ms = 0;
    Millis complete_ms = 0;
    Millis queued_ms = 0, dequeued_ms = 0;
    bool discontinuity_before = false;
};

struct TransportStats {
    uint64_t packets = 0, bytes = 0, invalid = 0, oversized = 0;
    uint64_t foreign = 0, duplicate = 0, conflicts = 0, late = 0, out_of_window = 0;
    uint64_t complete = 0, ordered = 0, consumed = 0, timeouts = 0, missing = 0;
    uint64_t budget_rejected = 0, queue_dropped = 0, queue_expired = 0, restarts = 0;
    uint64_t generation = 1;
    size_t pending_frames = 0, pending_bytes = 0, queue_frames = 0, queue_bytes = 0;
    size_t peak_pending_bytes = 0, peak_pending_frames = 0, peak_queue_bytes = 0;
    size_t peak_queue_frames = 0;
    Millis pending_oldest_ms = 0, queue_oldest_ms = 0;
    uint64_t queue_overflow_dropped = 0, queue_restart_dropped = 0, queue_oversize_dropped = 0;
    TimingHistogram assembly_time, order_wait, compressed_queue_wait;
    Millis last_packet_ms = 0;
    Endpoint source;
    bool source_locked = false;
};

// One owner thread, with synchronization supplied by UdpReceiver. All time
// decisions receive a monotonic timestamp so packet arrival is not the timer.
class ProtocolCore {
public:
    explicit ProtocolCore(TransportConfig config = {}, uint64_t generation = 1);
    void ingest(const uint8_t *packet, size_t length, Endpoint source, Millis now);
    void tick(Millis now);
    std::optional<EncodedGroup> pop(Millis now);
    void note_oversized(size_t length);
    TransportStats stats(Millis now = monotonic_ms()) const;
    const TransportConfig &config() const { return config_; }
private:
    struct Entry {
        std::vector<uint8_t> data, received;
        size_t fragments = 0;
        Millis first = 0, completed_at = 0;
        bool complete = false, invalid = false;
        size_t allocation() const { return data.size() + received.size(); }
    };
    TransportConfig config_;
    TransportStats stats_;
    std::map<uint64_t, Entry> entries_;
    std::deque<EncodedGroup> output_;
    uint64_t expected_ = 0, initial_anchor_ = 0;
    Millis startup_at_ = 0;
    bool anchored_ = false, emitted_ = false, discontinuity_ = true, consumer_gap_ = false;
    void establish(uint16_t id, Endpoint source, Millis now, bool restart);
    void invalidate(Entry &entry);
    void erase(std::map<uint64_t, Entry>::iterator item);
    void drain(Millis now);
    void emit(uint64_t serial, Entry &entry, Millis now);
    void expire_output(Millis now);
};
}
