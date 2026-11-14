#pragma once
#include "media/session.hpp"
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/variant/dictionary.hpp>

class RmVideoSession : public godot::RefCounted {
    GDCLASS(RmVideoSession, godot::RefCounted)
    rm::MediaSession receiver_;
    std::string config_error_;
protected:
    static void _bind_methods();
public:
    bool start(const godot::Dictionary &values);
    void stop();
    void request_stop();
    void poll_stop();
    godot::Dictionary snapshot() const;
    rm::MediaSession &core() { return receiver_; }
};
