#include "session.hpp"
#include <chrono>
#include <stdexcept>
#include <utility>

namespace rm {
MediaSession::~MediaSession() { stop(); }
bool MediaSession::start(const TransportConfig &network, const MediaConfig &media) {
    poll_stop();
    if (!done_.load()) return false;
    auto invalid = media.validate();
    if (!invalid.empty()) {
        std::lock_guard<std::mutex> lock(mutex_); state_.error = invalid; state_.state = "Faulted"; return false;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        state_ = {}; latest_.reset(); state_.state = "Starting"; state_.started_ms = monotonic_ms();
    }
    if (!receiver_.start(network)) {
        std::lock_guard<std::mutex> lock(mutex_); state_.error = receiver_.snapshot().error; state_.state = "Faulted"; return false;
    }
    quit_.store(false); done_.store(false);
    try { worker_ = std::thread([this, media] { run(media); }); }
    catch (const std::exception &error) {
        receiver_.stop(); done_.store(true);
        std::lock_guard<std::mutex> lock(mutex_); state_.error = error.what(); state_.state = "Faulted"; return false;
    }
    return true;
}
void MediaSession::request_stop() {
    quit_.store(true);
    std::lock_guard<std::mutex> lock(mutex_);
    latest_.reset();
    if (!done_.load()) state_.state = "Stopping";
    else if (state_.state != "Faulted") state_.state = "Stopped";
}
void MediaSession::poll_stop() {
    if (done_.load() && worker_.joinable()) worker_.join();
}
void MediaSession::stop() {
    request_stop();
    if (worker_.joinable()) worker_.join();
    receiver_.stop();
}
std::shared_ptr<PreparedFrame> MediaSession::take_frame() {
    std::lock_guard<std::mutex> lock(mutex_);
    return std::exchange(latest_, {});
}
MediaSnapshot MediaSession::snapshot() const {
    MediaSnapshot snapshot;
    { std::lock_guard<std::mutex> lock(mutex_); snapshot = state_; }
    snapshot.receiver = receiver_.snapshot(); snapshot.worker_done = done_.load(); return snapshot;
}
void MediaSession::run(MediaConfig config) noexcept {
    try {
        std::unique_ptr<Decoder> decoder;
        decoder = std::make_unique<Decoder>(config, [&](FrameLease frame) {
            if (quit_.load()) return;
            auto prepared = decoder->prepare_cpu(std::move(frame));
            std::lock_guard<std::mutex> lock(mutex_);
            if (quit_.load()) return;
            if (latest_) ++state_.mailbox_replaced;
            state_.format = prepared->info; state_.path = prepared->path;
            state_.last_frame_ms = monotonic_ms(); ++state_.prepared;
            latest_ = std::move(prepared); state_.state = "Running";
        });
        {
            std::lock_guard<std::mutex> lock(mutex_);
            state_.backend = decoder->backend(); state_.device = decoder->device_identity();
            state_.fallback = decoder->fallback();
            if (!quit_.load()) state_.state = "WaitingConfig";
        }
        while (!quit_.load()) {
            auto group = receiver_.pop();
            if (group) {
                decoder->feed(std::move(*group));
                std::lock_guard<std::mutex> lock(mutex_); state_.decode = decoder->counters();
            } else {
                auto status = receiver_.snapshot();
                if (!status.running) throw std::runtime_error(status.error.empty() ? "UDP receiver stopped unexpectedly" : status.error);
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
    } catch (const std::exception &error) {
        std::lock_guard<std::mutex> lock(mutex_); state_.error = error.what(); state_.state = "Faulted";
    } catch (...) {
        std::lock_guard<std::mutex> lock(mutex_); state_.error = "Unexpected decoder failure"; state_.state = "Faulted";
    }
    receiver_.stop();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        latest_.reset(); if (state_.state != "Faulted") state_.state = "Stopped";
    }
    done_.store(true);
}
}
