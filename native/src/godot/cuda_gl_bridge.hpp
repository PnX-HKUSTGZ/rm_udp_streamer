#pragma once
#include "media/decoder.hpp"
#include "interop/gpu_api.hpp"
#include <godot_cpp/classes/texture2d.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <array>
#include <atomic>

class RmGlTexture : public godot::Texture2D {
    GDCLASS(RmGlTexture,godot::Texture2D)
protected:
    static void _bind_methods() {}
public:
    godot::RID rid;
    int width=0,height=0;
    int32_t _get_width() const override { return width; }
    int32_t _get_height() const override { return height; }
    bool _has_alpha() const override { return false; }
    godot::RID _get_rid() const override { return rid; }
};

struct RmGpuPublication {
    std::vector<godot::Ref<godot::Texture2D>> textures;
};
class RmCudaGlBridge {
    struct Slot {
        std::array<unsigned,2> textures{};
        std::array<unsigned,2> interop_textures{};
        std::array<rm::gpu::Resource,2> resources{};
        std::array<godot::Ref<RmGlTexture>,2> wrappers;
        rm::gpu::Event producer = nullptr;
        rm::gpu::Sync consumer = nullptr;
        rm::FrameLease source;
        std::weak_ptr<RmGpuPublication> lease;
        bool acquired=false, mapped=false, recorded=false;
        int width=0,height=0;
    };
    std::array<Slot,3> slots_;
    rm::gpu::Driver cuda_;
    rm::gpu::Gl gl_;
    rm::gpu::Context context_ = nullptr;
    rm::gpu::Stream stream_ = nullptr;
    AVBufferRef *device_ = nullptr;
    bool initialized_=false;
    std::atomic<uint64_t> copied_{0},bytes_{0},released_{0},fences_{0},retired_{0},dropped_{0},live_{0},sources_{0};
    std::atomic<uint64_t> external_gl_errors_{0};
    std::atomic<uint64_t> display_bytes_{0};
    void initialize(const rm::FrameLease &);
    void allocate(Slot &,int width,int height);
    void destroy(Slot &);
    void poll_current();
public:
    // All methods except metrics run on the Godot render thread.
    std::shared_ptr<RmGpuPublication> upload(const rm::FrameLease &);
    void poll();
    void close(); // Called only after all canvas consumers and publications detach.
    godot::Dictionary metrics() const;
    ~RmCudaGlBridge();
};
