#include "cuda_gl_bridge.hpp"
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/image.hpp>
#include <chrono>
#include <thread>
#include <cstring>
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
        cuda_.check(cuda_.cuEventCreate(&slot.producer,2),"create producer completion event");
        ++live_;
    } catch (...) {
        gl_.BindTexture(texture_2d,binding);gl_.BindBuffer(0x88EC,unpack);throw;
    }
    gl_.BindTexture(texture_2d,binding);gl_.BindBuffer(0x88EC,unpack);
}
void RmCudaGlBridge::destroy(Slot &slot) {
    // No consumer/producer can still reference this slot here.
    bool counted=slot.producer!=nullptr;
    if(slot.producer)cuda_.check(cuda_.cuEventDestroy_v2(slot.producer),"destroy producer event");
    slot.producer=nullptr;
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
            if(status==0){slot.source={};slot.recorded=false;++released_;--sources_;}
            else if(status!=600)cuda_.check(status,"query video-copy completion");
        }
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
    if(slot.width!=frame.info.width||slot.height!=frame.info.height) {
        destroy(slot);allocate(slot,frame.info.width,frame.info.height);
    }
    auto publication=std::make_shared<RmGpuPublication>();
    slot.source=frame;++sources_;slot.acquired=true;slot.lease=publication;
    cuda_.check(cuda_.cuGraphicsMapResources(2,slot.resources.data(),stream_),"map CUDA/GL planes");slot.mapped=true;
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
    // CUDA unmap orders subsequent GL work after all earlier CUDA work on stream.
    cuda_.check(cuda_.cuGraphicsUnmapResources(2,slot.resources.data(),stream_),"unmap CUDA/GL planes");slot.mapped=false;
    cuda_.check(cuda_.cuEventRecord(slot.producer,stream_),"record source release event");slot.recorded=true;
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
    return result;
}
