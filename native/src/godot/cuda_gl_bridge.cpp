#include "cuda_gl_bridge.hpp"
#include "diagnostics.hpp"
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/image.hpp>
#include <chrono>
#include <thread>
#include <cstring>
#include <cmath>
#include <limits>
extern "C" {
#include <libavutil/hwcontext.h>
// Public FFmpeg layout uses these opaque CUDA types; no SDK dependency.
typedef rm::gpu::Context CUcontext;
typedef rm::gpu::Stream CUstream;
#define CUDA_VERSION 7050
#include <libavutil/hwcontext_cuda.h>
}
using namespace godot;
using namespace rm::gpu;
std::atomic<bool> RmCudaGlBridge::unsafe_retirement{false};
namespace {
constexpr unsigned texture_2d=0x0DE1;
}
void RmCudaGlBridge::initialize(const rm::FrameLease &frame) {
    if (unsafe_retirement.load()) throw std::runtime_error("Previous GPU retirement failed; restart the application before using GPU transfer");
    if (RenderingServer::get_singleton()->get_current_rendering_method()!="gl_compatibility")
        throw std::runtime_error("GPU transfer requires Compatibility/OpenGL; restart with --rendering-method gl_compatibility or select CPU transfer");
    if (!frame.frame || frame.frame->format!=AV_PIX_FMT_CUDA || !frame.frame->hw_frames_ctx)
        throw std::runtime_error("CUDA/OpenGL bridge requires actual CUDA hardware frames");
    auto *frames = reinterpret_cast<AVHWFramesContext *>(frame.frame->hw_frames_ctx->data);
    auto *hw = reinterpret_cast<AVCUDADeviceContext *>(frames->device_ctx->hwctx);
    cuda_.load(); gl_.load(); cuda_.check(cuda_.cuInit(0),"initialize CUDA driver");
    int extension_count=0;gl_.GetIntegerv(0x821D,&extension_count);bool copy_image=false;
    for(int i=0;i<extension_count;++i) {
        const auto *extension=gl_.GetStringi(0x1F03,i);
        if(extension&&!std::strcmp(reinterpret_cast<const char *>(extension),"GL_ARB_copy_image"))copy_image=true;
    }
    if(!copy_image)throw std::runtime_error("CUDA/OpenGL bridge requires GL_ARB_copy_image");
    CurrentContext current(cuda_,hw->cuda_ctx);
    int decode_device=-1, devices[16]{}; unsigned count=0;
    cuda_.check(cuda_.cuCtxGetDevice(&decode_device),"query decoder device");
    cuda_.check(cuda_.cuGLGetDevices(&count,devices,16,1),"query CUDA devices for current OpenGL context");
    if (count!=1 || devices[0]!=decode_device)
        throw std::runtime_error("OpenGL and CUDA must use the same single physical GPU; select CPU transfer");
    device_=av_buffer_ref(frames->device_ref); if (!device_) throw std::bad_alloc();
    context_=hw->cuda_ctx; stream_=hw->stream; initialized_=true;
    if(diagnostics_enabled_) {
        unsigned prior_error=gl_.GetError();
        if(prior_error){++external_gl_errors_;throw std::runtime_error("OpenGL consumer reported error "+std::to_string(prior_error));}
        cuda_timing_=cuda_.load_timing();gl_timing_=gl_.load_timing();
        if(gl_timing_) {
            int bits=0;gl_.GetQueryiv(0x8E28,0x8864,&bits); // TIMESTAMP / QUERY_COUNTER_BITS
            gl_timing_=gl_.GetError()==0&&bits>0;
        }
        std::lock_guard<std::mutex> lock(timing_mutex_);
        timing_.cuda_status=cuda_timing_?"available":"unavailable";
        timing_.gl_status=gl_timing_?"available":"unavailable";
    }
}
void RmCudaGlBridge::record_cpu(rm::TimingHistogram TimingState::*field,rm::Micros started) {
    if(!diagnostics_enabled_)return;
    auto elapsed=rm::monotonic_us()-started;
    std::lock_guard<std::mutex> lock(timing_mutex_);(timing_.*field).observe(elapsed);
}
void RmCudaGlBridge::timing_error(bool cuda,const std::string &message) {
    std::lock_guard<std::mutex> lock(timing_mutex_);++timing_.errors;
    (cuda?timing_.cuda_status:timing_.gl_status)="error: "+message;
}
void RmCudaGlBridge::begin_cuda_timing(Slot &slot) {
    if(!diagnostics_enabled_)return;
    slot.copy_queued_us=rm::monotonic_us();
    int status=slot.copy_start?cuda_.cuEventRecord(slot.copy_start,stream_):-1;
    if(slot.copy_start&&status)timing_error(true,"record start event "+std::to_string(status));
    slot.cuda_timed=status==0;
    std::lock_guard<std::mutex> lock(timing_mutex_);++timing_.cuda_requested;
    if(slot.cuda_timed)++timing_.cuda_pending;else ++timing_.cuda_missing;
}
void RmCudaGlBridge::finish_cuda_timing(Slot &slot) {
    if(!diagnostics_enabled_)return;
    auto observed=rm::monotonic_us();float milliseconds=0;
    int status=slot.cuda_timed?cuda_.cuEventElapsedTime(&milliseconds,slot.copy_start,slot.producer):0;
    double microseconds=std::ceil(double(milliseconds)*1000.0);
    bool valid=status==0&&std::isfinite(microseconds)&&microseconds>=0&&microseconds<double(std::numeric_limits<rm::Micros>::max());
    if(slot.cuda_timed&&!valid)timing_error(true,"elapsed event time "+std::to_string(status));
    std::lock_guard<std::mutex> lock(timing_mutex_);
    if(slot.copy_queued_us)timing_.cuda_completion_observed.observe(observed-slot.copy_queued_us);
    if(slot.cuda_timed) {
        --timing_.cuda_pending;
        if(valid)timing_.cuda_copy_unmap_gpu.observe(rm::Micros(microseconds));else ++timing_.cuda_missing;
    }
    slot.cuda_timed=false;slot.copy_queued_us=0;
}
void RmCudaGlBridge::begin_gl_timing(Slot &slot) {
    if(!diagnostics_enabled_)return;
    bool valid=false;
    if(slot.copy_queries[0]) {
        gl_.QueryCounter(slot.copy_queries[0],0x8E28);unsigned error=gl_.GetError();valid=error==0;
        if(error)timing_error(false,"start timestamp "+std::to_string(error));
    }
    slot.gl_timed=valid;slot.gl_end_recorded=false;
    std::lock_guard<std::mutex> lock(timing_mutex_);++timing_.gl_requested;
    if(valid)++timing_.gl_pending;else ++timing_.gl_missing;
}
void RmCudaGlBridge::end_gl_timing(Slot &slot) {
    if(!slot.gl_timed)return;
    gl_.QueryCounter(slot.copy_queries[1],0x8E28);unsigned error=gl_.GetError();
    slot.gl_end_recorded=error==0;
    if(error)timing_error(false,"end timestamp "+std::to_string(error));
}
void RmCudaGlBridge::collect_gl_timing(Slot &slot) {
    if(!slot.gl_timed||!slot.gl_end_recorded)return;
    int first=0,last=0;
    gl_.GetQueryObjectiv(slot.copy_queries[0],0x8867,&first); // QUERY_RESULT_AVAILABLE
    gl_.GetQueryObjectiv(slot.copy_queries[1],0x8867,&last);
    unsigned error=gl_.GetError();
    if(!error&&(!first||!last))return; // Never wait for a diagnostic sample.
    uint64_t start=0,end=0;
    if(!error) {
        gl_.GetQueryObjectui64v(slot.copy_queries[0],0x8866,&start);
        gl_.GetQueryObjectui64v(slot.copy_queries[1],0x8866,&end);error=gl_.GetError();
    }
    bool valid=!error&&end>=start;
    if(!valid)timing_error(false,"timestamp result "+std::to_string(error));
    std::lock_guard<std::mutex> lock(timing_mutex_);--timing_.gl_pending;
    if(valid)timing_.gl_display_copy_gpu.observe(rm::Micros((end-start)/1000+((end-start)%1000!=0)));
    else ++timing_.gl_missing;
    slot.gl_timed=slot.gl_end_recorded=false;
}
void RmCudaGlBridge::discard_timing(Slot &slot) {
    // Ownership fences, not timing queries, decide when the slot can be reused.
    std::lock_guard<std::mutex> lock(timing_mutex_);
    if(slot.cuda_timed){--timing_.cuda_pending;++timing_.cuda_missing;slot.cuda_timed=false;}
    if(slot.gl_timed){--timing_.gl_pending;++timing_.gl_missing;slot.gl_timed=slot.gl_end_recorded=false;}
    slot.copy_queued_us=0;
}
void RmCudaGlBridge::allocate(Slot &slot,int width,int height) {
    int binding=0,unpack=0; gl_.GetIntegerv(0x8069,&binding); gl_.GetIntegerv(0x88EF,&unpack);
    gl_.BindBuffer(0x88EC,0);
    try {
        slot.width=width;slot.height=height;
        gl_.GenTextures(2,slot.textures.data());
        gl_.GenTextures(2,slot.interop_textures.data());
        for (int i=0;i<2;++i) {
            int w=i?(width+1)/2:width,h=i?(height+1)/2:height;
            // CUDA owns only private staging textures. Godot may freely change
            // its display texture's sampling parameters without violating CUDA
            // registration restrictions.
            gl_.BindTexture(texture_2d,slot.interop_textures[i]);
            gl_.TexParameteri(texture_2d,0x2801,0x2600);gl_.TexParameteri(texture_2d,0x2800,0x2600);
            gl_.TexParameteri(texture_2d,0x813C,0);gl_.TexParameteri(texture_2d,0x813D,0);
            gl_.TexImage2D(texture_2d,0,i?0x822B:0x8229,w,h,0,i?0x8227:0x1903,0x1401,nullptr);
            if(gl_.GetError())throw std::runtime_error("Cannot allocate private GPU staging texture");
            cuda_.check(cuda_.cuGraphicsGLRegisterImage(&slot.resources[i],slot.interop_textures[i],texture_2d,2),"register private CUDA/GL plane");
            gl_.BindTexture(texture_2d,slot.textures[i]);
            gl_.TexParameteri(texture_2d,0x2801,0x2601); gl_.TexParameteri(texture_2d,0x2800,0x2601);
            gl_.TexParameteri(texture_2d,0x2802,0x812F); gl_.TexParameteri(texture_2d,0x2803,0x812F);
            gl_.TexParameteri(texture_2d,0x813C,0);gl_.TexParameteri(texture_2d,0x813D,0);
            gl_.TexImage2D(texture_2d,0,i?0x822B:0x8229,w,h,0,i?0x8227:0x1903,0x1401,nullptr);
            unsigned error=gl_.GetError();
            if(error)throw std::runtime_error("Could not allocate OpenGL NV12 texture plane "+std::to_string(i)+" (GL error "+std::to_string(error)+")");
            slot.wrappers[i].instantiate();slot.wrappers[i]->width=w;slot.wrappers[i]->height=h;
            slot.wrappers[i]->rid=RenderingServer::get_singleton()->texture_create_from_native_handle(
                RenderingServer::TEXTURE_TYPE_2D,i?Image::FORMAT_RG8:Image::FORMAT_R8,slot.textures[i],w,h,1);
            if (!slot.wrappers[i]->rid.is_valid()) throw std::runtime_error("Cannot import OpenGL plane into Godot");
        }
        if(cuda_timing_) {
            int status=cuda_.cuEventCreate(&slot.copy_start,0);
            if(status){slot.copy_start=nullptr;timing_error(true,"create start event "+std::to_string(status));}
            else {std::lock_guard<std::mutex> lock(timing_mutex_);++timing_.events_live;}
        }
        cuda_.check(cuda_.cuEventCreate(&slot.producer,slot.copy_start?0:2),"create producer completion event");
        ++live_;
        if(gl_timing_) {
            gl_.GenQueries(2,slot.copy_queries.data());unsigned error=gl_.GetError();
            if(error||!slot.copy_queries[0]||!slot.copy_queries[1]) {
                timing_error(false,"allocate timestamp queries "+std::to_string(error));
                gl_.DeleteQueries(2,slot.copy_queries.data());slot.copy_queries={};
                if(auto cleanup_error=gl_.GetError())timing_error(false,"failed query allocation cleanup "+std::to_string(cleanup_error));
            } else {std::lock_guard<std::mutex> lock(timing_mutex_);timing_.queries_live+=2;}
        }
    } catch (...) {
        gl_.BindTexture(texture_2d,binding);gl_.BindBuffer(0x88EC,unpack);throw;
    }
    gl_.BindTexture(texture_2d,binding);gl_.BindBuffer(0x88EC,unpack);
}
void RmCudaGlBridge::destroy(Slot &slot) {
    // No consumer/producer can still reference this slot here.
    collect_gl_timing(slot);discard_timing(slot);
    bool counted=slot.producer!=nullptr;
    if(slot.producer)cuda_.check(cuda_.cuEventDestroy_v2(slot.producer),"destroy producer event");
    slot.producer=nullptr;
    if(slot.copy_start) {
        cuda_.check(cuda_.cuEventDestroy_v2(slot.copy_start),"destroy timing start event");slot.copy_start=nullptr;
        std::lock_guard<std::mutex> lock(timing_mutex_);--timing_.events_live;
    }
    if(slot.copy_queries[0]) {
        gl_.DeleteQueries(2,slot.copy_queries.data());slot.copy_queries={};
        std::lock_guard<std::mutex> lock(timing_mutex_);timing_.queries_live-=2;
    }
    for(int i=0;i<2;++i) {
        if(slot.resources[i])cuda_.check(cuda_.cuGraphicsUnregisterResource(slot.resources[i]),"unregister GL plane");
        slot.resources[i]=nullptr;
        if(slot.wrappers[i].is_valid()) {
            RenderingServer::get_singleton()->free_rid(slot.wrappers[i]->rid);slot.wrappers[i]->rid=RID();slot.wrappers[i].unref();
        }
    }
    gl_.DeleteTextures(2,slot.textures.data());slot.textures={};slot.width=slot.height=0;
    gl_.DeleteTextures(2,slot.interop_textures.data());slot.interop_textures={};
    if(counted)--live_;
}
void RmCudaGlBridge::poll_current() {
    for(auto &slot:slots_) {
        if(slot.source.frame && slot.recorded) {
            int status=cuda_.cuEventQuery(slot.producer);
            if(status==0){finish_cuda_timing(slot);slot.source={};slot.recorded=false;++released_;--sources_;}
            else if(status!=600)cuda_.check(status,"query video-copy completion");
        }
        collect_gl_timing(slot);
        if(slot.acquired && slot.lease.expired()) {
            if(!slot.consumer) {
                // Main thread explicitly cleared the canvas and material before
                // releasing the publication. FIFO RS commands put this fence
                // after every previously issued consumer, not after N frames.
                slot.consumer=gl_.FenceSync(0x9117,0);
                if(!slot.consumer)throw std::runtime_error("Cannot create consumer fence");
                gl_.Flush();++fences_;
            }
            unsigned status=gl_.ClientWaitSync(slot.consumer,0,0);
            if(status==0x911D)throw std::runtime_error("OpenGL consumer fence failed");
            if((status==0x911A||status==0x911C)&&!slot.source.frame) {
                gl_.DeleteSync(slot.consumer);slot.consumer=nullptr;slot.acquired=false;++retired_;
            }
        }
    }
}
void RmCudaGlBridge::poll() {
    if(!initialized_)return;
    CurrentContext current(cuda_,context_);poll_current();
}
std::shared_ptr<RmGpuPublication> RmCudaGlBridge::upload(const rm::FrameLease &frame) {
    if(!initialized_)initialize(frame);
    unsigned prior_error=gl_.GetError();
    if(prior_error) {++external_gl_errors_;throw std::runtime_error("OpenGL consumer reported error "+std::to_string(prior_error));}
    auto *frames=reinterpret_cast<AVHWFramesContext *>(frame.frame->hw_frames_ctx->data);
    auto *hw=reinterpret_cast<AVCUDADeviceContext *>(frames->device_ctx->hwctx);
    if(hw->cuda_ctx!=context_)throw std::runtime_error("CUDA device context changed before GPU retirement");
    if(frames->sw_format!=AV_PIX_FMT_NV12 || frame.info.bit_depth!=8 || (frame.info.crop_left&1) || (frame.info.crop_top&1))
        throw std::runtime_error("CUDA/OpenGL GPU path currently requires NV12 8-bit with even crop origin; select CPU transfer");
    CurrentContext current(cuda_,context_);poll_current();
    Slot *available=nullptr;
    for(auto &slot:slots_)if(!slot.acquired){available=&slot;break;}
    if(!available){++dropped_;return {};}
    auto &slot=*available;
    collect_gl_timing(slot);discard_timing(slot);
    if(slot.width!=frame.info.width||slot.height!=frame.info.height) {
        destroy(slot);allocate(slot,frame.info.width,frame.info.height);
    }
    auto publication=std::make_shared<RmGpuPublication>();
    slot.source=frame;++sources_;slot.acquired=true;slot.lease=publication;
    auto started=diagnostics_enabled_?rm::monotonic_us():0;
    cuda_.check(cuda_.cuGraphicsMapResources(2,slot.resources.data(),stream_),"map CUDA/GL planes");slot.mapped=true;
    record_cpu(&TimingState::map_cpu,started);begin_cuda_timing(slot);
    started=diagnostics_enabled_?rm::monotonic_us():0;
    for(int i=0;i<2;++i) {
        rm::gpu::Array array=nullptr;
        cuda_.check(cuda_.cuGraphicsSubResourceGetMappedArray(&array,slot.resources[i],0,0),"get CUDA/GL plane array");
        Copy2D copy{};copy.src_type=2;copy.src_device=reinterpret_cast<uintptr_t>(frame.frame->data[i]);
        copy.src_pitch=frame.frame->linesize[i];copy.src_x=frame.info.crop_left;copy.src_y=frame.info.crop_top/(i?2:1);
        copy.dst_type=3;copy.dst_array=array;copy.width=i?size_t((slot.width+1)/2)*2:size_t(slot.width);copy.height=i?(slot.height+1)/2:slot.height;
        if(!copy.src_device||frame.frame->linesize[i]<=0||copy.src_pitch<copy.src_x+copy.width)
            throw std::runtime_error("Invalid CUDA NV12 plane stride");
        // Same FFmpeg stream: async decoder output precedes these copies.
        cuda_.check(cuda_.cuMemcpy2DAsync_v2(&copy,stream_),"copy video plane on GPU");bytes_+=copy.width*copy.height;
        publication->textures.push_back(slot.wrappers[i]);
    }
    record_cpu(&TimingState::cuda_copy_enqueue_cpu,started);
    // CUDA unmap orders subsequent GL work after all earlier CUDA work on stream.
    started=diagnostics_enabled_?rm::monotonic_us():0;
    cuda_.check(cuda_.cuGraphicsUnmapResources(2,slot.resources.data(),stream_),"unmap CUDA/GL planes");slot.mapped=false;
    record_cpu(&TimingState::unmap_cpu,started);
    cuda_.check(cuda_.cuEventRecord(slot.producer,stream_),"record source release event");slot.recorded=true;
    begin_gl_timing(slot);started=diagnostics_enabled_?rm::monotonic_us():0;
    for(int i=0;i<2;++i) {
        int width=i?(slot.width+1)/2:slot.width,height=i?(slot.height+1)/2:slot.height;
        gl_.CopyImageSubData(slot.interop_textures[i],texture_2d,0,0,0,0,slot.textures[i],texture_2d,0,0,0,0,width,height,1);
        display_bytes_+=size_t(width)*height*(i?2:1);
    }
    // Unmap orders this GL copy after CUDA; the same GL command queue orders
    // later Godot sampling after the copy. Source release is polled via its
    // CUDA event, while the consumer fence covers both GL copy and sampling.
    unsigned graphics_error=gl_.GetError();
    if(graphics_error)throw std::runtime_error("OpenGL error after CUDA copy: "+std::to_string(graphics_error));
    record_cpu(&TimingState::gl_copy_enqueue_cpu,started);end_gl_timing(slot);
    ++copied_;return publication;
}
void RmCudaGlBridge::close() {
    if(!initialized_)return;
    CurrentContext current(cuda_,context_);
    for(auto &slot:slots_) {
        if(slot.mapped) {cuda_.check(cuda_.cuGraphicsUnmapResources(2,slot.resources.data(),stream_),"unmap failed upload");slot.mapped=false;}
        if(slot.source.frame&&!slot.recorded) {cuda_.check(cuda_.cuEventRecord(slot.producer,stream_),"record failed upload completion");slot.recorded=true;}
    }
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
    for(;;) {
        poll_current();bool pending=false;
        for(const auto &slot:slots_)pending|=slot.acquired||bool(slot.source.frame);
        if(!pending)break;
        if(std::chrono::steady_clock::now()>=deadline)throw std::runtime_error("GPU retirement timed out; resources retained and restart blocked");
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    for(auto &slot:slots_)destroy(slot);
    // The context guard must pop before releasing the final device reference.
}
RmCudaGlBridge::~RmCudaGlBridge() { av_buffer_unref(&device_); }
Dictionary RmCudaGlBridge::metrics() const {
    Dictionary result;
    result["gpu_copies"]=int64_t(copied_);result["gpu_copy_bytes"]=int64_t(bytes_);
    result["gpu_sources_released"]=int64_t(released_);result["gpu_sources_live"]=int64_t(sources_);
    result["gpu_consumer_fences"]=int64_t(fences_);result["gpu_retired"]=int64_t(retired_);
    result["gpu_busy_drops"]=int64_t(dropped_);result["gpu_slots_live"]=int64_t(live_);result["gpu_slot_limit"]=3;
    result["external_gl_errors"]=int64_t(external_gl_errors_);
    result["gpu_display_copy_bytes"]=int64_t(display_bytes_);
    result["gpu_diagnostics_enabled"]=diagnostics_enabled_;
    if(diagnostics_enabled_) {
        TimingState copy;{std::lock_guard<std::mutex> lock(timing_mutex_);copy=timing_;}
        result["cuda_timing_status"]=String(copy.cuda_status.c_str());result["gl_timing_status"]=String(copy.gl_status.c_str());
#define METRIC(field) result["gpu_timing_" #field]=int64_t(copy.field)
        METRIC(errors);METRIC(cuda_requested);METRIC(cuda_missing);METRIC(cuda_pending);
        METRIC(gl_requested);METRIC(gl_missing);METRIC(gl_pending);METRIC(events_live);METRIC(queries_live);
#undef METRIC
        Dictionary timings;
#define TIMING(field) timings[#field]=rm::binding::histogram(copy.field)
        TIMING(map_cpu);TIMING(cuda_copy_enqueue_cpu);TIMING(unmap_cpu);TIMING(gl_copy_enqueue_cpu);
        TIMING(cuda_copy_unmap_gpu);TIMING(gl_display_copy_gpu);TIMING(cuda_completion_observed);
#undef TIMING
        result["gpu_timings"]=timings;
    }
    return result;
}
