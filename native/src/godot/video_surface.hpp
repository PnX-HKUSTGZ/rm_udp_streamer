#pragma once
#include "media/session.hpp"
#include <godot_cpp/classes/control.hpp>
#include <godot_cpp/classes/image_texture.hpp>
#include <godot_cpp/classes/shader_material.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <atomic>
#include <mutex>

class RmVideoSession;
struct RmUploadState {
    struct Publication {
        rm::FrameInfo info;
        std::string path;
        std::vector<godot::Ref<godot::ImageTexture>> textures;
    };
    std::mutex mutex;
    std::shared_ptr<Publication> published;
    std::vector<godot::Ref<godot::ImageTexture>> textures;
    std::string layout;
    std::atomic<bool> active{true}, busy{false};
    std::atomic<uint64_t> submitted{0};
    std::string error;
};

// The callback retains its own Ref and state. Removing the surface or stopping
// a stream cannot destroy a queued upload's source buffers or engine resources.
class RmCpuUpload : public godot::RefCounted {
    GDCLASS(RmCpuUpload, godot::RefCounted)
protected:
    static void _bind_methods() {}
public:
    std::shared_ptr<RmUploadState> state;
    std::shared_ptr<rm::PreparedFrame> frame;
    void run(const godot::Ref<RmCpuUpload> &keep_alive);
};

class RmVideoSurface : public godot::Control {
    GDCLASS(RmVideoSurface, godot::Control)
    godot::Ref<RmVideoSession> session_;
    std::shared_ptr<RmUploadState> upload_;
    std::shared_ptr<RmUploadState::Publication> shown_;
    godot::Ref<godot::ShaderMaterial> material_;
protected:
    static void _bind_methods();
public:
    RmVideoSurface();
    ~RmVideoSurface();
    void set_session(const godot::Ref<RmVideoSession> &session);
    void clear();
    godot::Dictionary get_metrics() const;
    void _process(double delta) override;
    void _draw() override;
};
