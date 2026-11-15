#pragma once
#include "media/decoder.hpp"
#include "interop/gpu_api.hpp"
#include <godot_cpp/classes/texture2d.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <array>
#include <atomic>
#include <mutex>

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
        rm::gpu::Event copy_start = nullptr;
        std::array<unsigned,2> copy_queries{};
        bool cuda_timed=false, gl_timed=false, gl_end_recorded=false;
        rm::Micros copy_queued_us=0;
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
    struct TimingState {
        std::string cuda_status="disabled", gl_status="disabled";
        uint64_t errors=0, cuda_requested=0, cuda_missing=0, cuda_pending=0;
        uint64_t gl_requested=0, gl_missing=0, gl_pending=0, events_live=0, queries_live=0;
        rm::TimingHistogram map_cpu, cuda_copy_enqueue_cpu, unmap_cpu, gl_copy_enqueue_cpu;
        rm::TimingHistogram cuda_copy_unmap_gpu, gl_display_copy_gpu, cuda_completion_observed;
    };
    const bool diagnostics_enabled_;
    bool cuda_timing_=false, gl_timing_=false;
    mutable std::mutex timing_mutex_;
    TimingState timing_;
    void record_cpu(rm::TimingHistogram TimingState::*,rm::Micros started);
    void timing_error(bool cuda,const std::string &);
    void begin_cuda_timing(Slot &);
    void finish_cuda_timing(Slot &);
    void begin_gl_timing(Slot &);
    void end_gl_timing(Slot &);
    void collect_gl_timing(Slot &);
    void discard_timing(Slot &);
    void initialize(const rm::FrameLease &);
    void allocate(Slot &,int width,int height);
    void destroy(Slot &);
    void poll_current();
public:
    explicit RmCudaGlBridge(bool diagnostics=false):diagnostics_enabled_(diagnostics) {}
    // All methods except metrics run on the Godot render thread.
    static std::atomic<bool> unsafe_retirement;
    std::shared_ptr<RmGpuPublication> upload(const rm::FrameLease &);
    void poll();
    void close(); // Called only after all canvas consumers and publications detach.
    godot::Dictionary metrics(bool detailed=true) const;
    ~RmCudaGlBridge();
};
