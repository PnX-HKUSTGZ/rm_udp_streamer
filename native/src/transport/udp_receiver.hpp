#pragma once
#include "protocol.hpp"
#include <memory>

namespace rm {
struct ReceiverSnapshot {
    TransportStats transport;
    bool running = false;
    uint16_t bound_port = 0;
    int socket_buffer_bytes = 0;
    std::string error;
};
class UdpReceiver {
public:
    UdpReceiver();
    ~UdpReceiver();
    UdpReceiver(const UdpReceiver &) = delete;
    UdpReceiver &operator=(const UdpReceiver &) = delete;
    bool start(const TransportConfig &config);
    void stop();
    std::optional<EncodedGroup> pop();
    ReceiverSnapshot snapshot() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
