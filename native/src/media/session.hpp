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
    std::string state = "Stopped", error, backend, device, path, fallback;
    FrameInfo format;
    uint64_t mailbox_replaced = 0, prepared = 0;
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
