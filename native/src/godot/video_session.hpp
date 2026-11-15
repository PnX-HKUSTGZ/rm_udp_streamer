#pragma once
#include "media/session.hpp"
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/variant/dictionary.hpp>

class RmVideoSession : public godot::RefCounted {
    GDCLASS(RmVideoSession, godot::RefCounted)
    rm::MediaSession receiver_;
    std::string config_error_;
    std::atomic<unsigned> gpu_holders_{0};
    mutable std::mutex gpu_mutex_;
    godot::Dictionary retired_gpu_;
    std::string retirement_error_;
protected:
    static void _bind_methods();
public:
    bool start(const godot::Dictionary &values);
    void stop();
    void request_stop();
    void poll_stop();
    godot::Dictionary snapshot(bool detailed=true) const;
    godot::Dictionary capabilities() const;
    rm::MediaSession &core() { return receiver_; }
    void retain_gpu() { ++gpu_holders_; }
    void release_gpu() { --gpu_holders_; }
    void record_gpu_retirement(const godot::Dictionary &metrics) { std::lock_guard<std::mutex> lock(gpu_mutex_);retired_gpu_=metrics; }
    void record_gpu_retirement_failure(const godot::Dictionary &metrics, const std::string &error) {
        std::lock_guard<std::mutex> lock(gpu_mutex_);
        retired_gpu_ = metrics; retirement_error_ = error;
    }
};
