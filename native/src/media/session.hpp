#pragma once
#include "decoder.hpp"
#include "transport/udp_receiver.hpp"
#include <atomic>
#include <mutex>
#include <thread>

namespace rm {
struct MediaSnapshot {
    ReceiverSnapshot receiver;
    DecodeCounters decode;
    std::string state = "Stopped", error, backend, device, path, fallback, recovery_reason;
    std::string requested_decoder, requested_transfer, presentation_fallback;
    FrameInfo format;
    uint64_t mailbox_replaced = 0, prepared = 0;
    uint64_t recoveries = 0, decode_errors = 0, format_changes = 0;
    uint64_t accepted_stream = 0, accepted_resource = 0, accepted_presentation = 0;
    uint64_t gpu_attempts = 0, presentation_fallbacks = 0;
    bool gpu_disabled = false;
    bool worker_done = true;
    Millis last_frame_ms = 0, started_ms = 0;
};
class MediaSession {
public:
    MediaSession() = default;
    ~MediaSession();
    bool start(const TransportConfig &, const MediaConfig &);
    void request_stop();
    void poll_stop();
    void stop();
    std::shared_ptr<PreparedFrame> take_frame();
    bool accepts(const FrameInfo &) const;
    void presented(const FrameInfo &, const std::string &path);
    void presentation_attempted();
    // Returns true only for a recoverable automatic GPU -> CPU transition.
    bool presentation_failed(const std::string &error, bool gpu_failure = false);
    MediaSnapshot snapshot() const;
private:
    UdpReceiver receiver_;
    mutable std::mutex mutex_;
    std::thread worker_;
    std::atomic<bool> quit_{false}, done_{true};
    MediaSnapshot state_;
    std::shared_ptr<PreparedFrame> latest_;
    void run(MediaConfig) noexcept;
};
}
