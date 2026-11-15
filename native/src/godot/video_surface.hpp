#pragma once
#include "media/session.hpp"
#include "cuda_gl_bridge.hpp"
#include "cpu_upload_buffers.hpp"
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
        std::vector<godot::Ref<godot::Texture2D>> textures;
        std::shared_ptr<RmGpuPublication> gpu;
    };
    std::mutex mutex;
    std::shared_ptr<Publication> published;
    std::vector<godot::Ref<godot::ImageTexture>> textures;
    RmCpuUploadBuffers cpu_buffers;
    std::string layout;
    std::atomic<bool> active{true}, busy{false};
    std::atomic<uint64_t> submitted{0};
    std::atomic<uint64_t> cpu_upload_storage_changes{0}, cpu_upload_copied_bytes{0}, cpu_upload_images_created{0};
    std::atomic<uint64_t> cpu_upload_cached_bytes{0}, cpu_upload_cached_peak_bytes{0};
    std::string error;
    std::shared_ptr<RmCudaGlBridge> gpu;
    bool gpu_registered=false;
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
    godot::Ref<RmVideoSession> session;
    bool close_only=false, poll_only=false;
    void run(const godot::Ref<RmCpuUpload> &keep_alive);
};

class RmVideoSurface : public godot::Control {
    GDCLASS(RmVideoSurface, godot::Control)
    godot::Ref<RmVideoSession> session_;
    std::shared_ptr<RmUploadState> upload_;
    std::shared_ptr<RmUploadState::Publication> shown_;
    godot::Ref<godot::ShaderMaterial> material_;
    void detach_picture();
    void retire_upload();
protected:
    static void _bind_methods();
public:
    RmVideoSurface();
    ~RmVideoSurface();
    void set_session(const godot::Ref<RmVideoSession> &session);
    void clear();
    godot::Dictionary get_metrics(bool detailed=true) const;
    void _process(double delta) override;
    void _draw() override;
};
