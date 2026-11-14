#include "udp_receiver.hpp"
#include <array>
#include <atomic>
#include <mutex>
#include <thread>
#include <stdexcept>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace rm {
namespace {
#ifdef _WIN32
using Socket = SOCKET;
constexpr Socket invalid_socket = INVALID_SOCKET;
int last_error() { return WSAGetLastError(); }
void close_socket(Socket socket) { closesocket(socket); }
bool would_block(int error) { return error == WSAEWOULDBLOCK; }
#else
using Socket = int;
constexpr Socket invalid_socket = -1;
int last_error() { return errno; }
void close_socket(Socket socket) { close(socket); }
bool would_block(int error) { return error == EAGAIN || error == EWOULDBLOCK; }
#endif
std::string socket_error(const char *operation) { return std::string(operation) + " failed (OS error " + std::to_string(last_error()) + ")"; }
}

struct UdpReceiver::Impl {
    mutable std::mutex data_mutex;
    std::mutex lifecycle_mutex;
    std::unique_ptr<ProtocolCore> core;
    std::thread worker;
    std::atomic<bool> quit{false}, running{false};
    Socket socket = invalid_socket;
    uint16_t bound_port = 0;
    int socket_buffer_bytes = 0;
    uint64_t generation = 0;
    std::string error;
#ifdef _WIN32
    bool winsock_started = false;
#endif
    void stop_locked() {
        quit.store(true);
        if (worker.joinable()) worker.join(); // select is bounded to 5 ms.
        running.store(false);
        if (socket != invalid_socket) { close_socket(socket); socket = invalid_socket; }
#ifdef _WIN32
        if (winsock_started) { WSACleanup(); winsock_started = false; }
#endif
        std::lock_guard<std::mutex> lock(data_mutex);
        if (core) generation = core->stats().generation;
        core.reset();
        bound_port = 0;
    }
    void run() noexcept {
        try {
            std::array<uint8_t, datagram_bytes + 1> buffer{};
            while (!quit.load()) {
                fd_set reads;
                FD_ZERO(&reads); FD_SET(socket, &reads);
                timeval wait{0, 5000};
                int ready = select(static_cast<int>(socket + 1), &reads, nullptr, nullptr, &wait);
                if (ready < 0) {
#ifndef _WIN32
                    if (errno == EINTR) continue;
#endif
                    throw std::runtime_error(socket_error("select"));
                }
                if (ready > 0) {
                    // A bounded drain makes timers and cancellation independent
                    // of an always-busy sender.
                    for (int batch = 0; batch < 128 && !quit.load(); ++batch) {
                        sockaddr_in remote{};
                        bool truncated = false;
                        int length = 0;
#ifdef _WIN32
                        int remote_size = sizeof(remote);
                        length = recvfrom(socket, reinterpret_cast<char *>(buffer.data()), static_cast<int>(buffer.size()), 0,
                                          reinterpret_cast<sockaddr *>(&remote), &remote_size);
                        if (length < 0 && last_error() == WSAEMSGSIZE) { length = int(buffer.size()); truncated = true; }
#else
                        iovec vector{buffer.data(), buffer.size()};
                        msghdr message{};
                        message.msg_name = &remote; message.msg_namelen = sizeof(remote);
                        message.msg_iov = &vector; message.msg_iovlen = 1;
                        length = static_cast<int>(recvmsg(socket, &message, MSG_TRUNC));
                        truncated = (message.msg_flags & MSG_TRUNC) != 0;
#endif
                        if (length < 0) {
                            int code = last_error();
                            if (would_block(code)) break;
#ifndef _WIN32
                            if (code == EINTR) continue;
#endif
                            throw std::runtime_error(socket_error("receive"));
                        }
                        std::lock_guard<std::mutex> lock(data_mutex);
                        if (truncated || size_t(length) > datagram_bytes) core->note_oversized(size_t(length));
                        else core->ingest(buffer.data(), size_t(length), {ntohl(remote.sin_addr.s_addr), ntohs(remote.sin_port)}, monotonic_ms());
                    }
                }
                std::lock_guard<std::mutex> lock(data_mutex);
                core->tick(monotonic_ms());
            }
        } catch (const std::exception &failure) {
            std::lock_guard<std::mutex> lock(data_mutex);
            error = failure.what();
        } catch (...) {
            std::lock_guard<std::mutex> lock(data_mutex);
            error = "Unexpected receive worker failure";
        }
        running.store(false);
    }
};

UdpReceiver::UdpReceiver() : impl_(std::make_unique<Impl>()) {}
UdpReceiver::~UdpReceiver() { stop(); }
bool UdpReceiver::start(const TransportConfig &config) {
    auto &self = *impl_;
    std::lock_guard<std::mutex> lifecycle(self.lifecycle_mutex);
    self.stop_locked();
    try {
        auto invalid = config.validate();
        if (!invalid.empty()) throw std::invalid_argument(invalid);
#ifdef _WIN32
        WSADATA data{};
        int startup = WSAStartup(MAKEWORD(2,2), &data);
        if (startup) throw std::runtime_error("WSAStartup failed: " + std::to_string(startup));
        self.winsock_started = true;
#endif
        self.socket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (self.socket == invalid_socket) throw std::runtime_error(socket_error("socket"));
#ifdef _WIN32
        int exclusive = 1;
        if (setsockopt(self.socket, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char *>(&exclusive), sizeof(exclusive)))
            throw std::runtime_error(socket_error("exclusive bind"));
        u_long nonblocking = 1;
        if (ioctlsocket(self.socket, FIONBIO, &nonblocking)) throw std::runtime_error(socket_error("nonblocking"));
#else
        if (self.socket >= FD_SETSIZE) throw std::runtime_error("Socket exceeds select descriptor limit");
        if (fcntl(self.socket, F_SETFL, O_NONBLOCK) < 0) throw std::runtime_error(socket_error("nonblocking"));
#endif
        int requested_buffer = 2 * 1024 * 1024;
        setsockopt(self.socket, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char *>(&requested_buffer), sizeof(requested_buffer));
        sockaddr_in address{};
        address.sin_family = AF_INET; address.sin_port = htons(config.port);
        address.sin_addr.s_addr = htonl(*parse_ipv4(config.bind_address));
        if (bind(self.socket, reinterpret_cast<sockaddr *>(&address), sizeof(address))) throw std::runtime_error(socket_error("bind"));
#ifdef _WIN32
        int address_size = sizeof(address), option_size = sizeof(self.socket_buffer_bytes);
#else
        socklen_t address_size = sizeof(address), option_size = sizeof(self.socket_buffer_bytes);
#endif
        if (getsockname(self.socket, reinterpret_cast<sockaddr *>(&address), &address_size)) throw std::runtime_error(socket_error("getsockname"));
        {
            std::lock_guard<std::mutex> lock(self.data_mutex);
            self.error.clear(); self.bound_port = ntohs(address.sin_port);
            getsockopt(self.socket, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<char *>(&self.socket_buffer_bytes), &option_size);
            self.core = std::make_unique<ProtocolCore>(config, ++self.generation);
        }
        self.quit.store(false); self.running.store(true);
        self.worker = std::thread([&self] { self.run(); });
        return true;
    } catch (const std::exception &failure) {
        self.stop_locked();
        std::lock_guard<std::mutex> lock(self.data_mutex);
        self.error = failure.what();
        return false;
    }
}
void UdpReceiver::stop() {
    std::lock_guard<std::mutex> lifecycle(impl_->lifecycle_mutex);
    impl_->stop_locked();
}
std::optional<EncodedGroup> UdpReceiver::pop() {
    std::lock_guard<std::mutex> lock(impl_->data_mutex);
    return impl_->core ? impl_->core->pop(monotonic_ms()) : std::optional<EncodedGroup>{};
}
ReceiverSnapshot UdpReceiver::snapshot() const {
    std::lock_guard<std::mutex> lock(impl_->data_mutex);
    ReceiverSnapshot result;
    if (impl_->core) result.transport = impl_->core->stats();
    else result.transport.generation = impl_->generation;
    result.running = impl_->running.load(); result.bound_port = impl_->bound_port;
    result.socket_buffer_bytes = impl_->socket_buffer_bytes; result.error = impl_->error;
    return result;
}
}
