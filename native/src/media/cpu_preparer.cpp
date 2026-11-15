#include "cpu_preparer.hpp"
#include "hevc_config.hpp"
#include <cstring>
#include <stdexcept>
#include <utility>
extern "C" {
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>
#include <libavutil/opt.h>
}

namespace rm {
namespace {
void check(int code, const char *operation, MediaFailure kind = MediaFailure::Input) {
    if (code >= 0) return;
    char text[AV_ERROR_MAX_STRING_SIZE]{};
    av_strerror(code, text, sizeof(text));
    bool device_error = code == AVERROR(ENOMEM) || code == AVERROR_EXTERNAL || code == AVERROR(EIO);
    throw MediaError(device_error ? MediaFailure::Backend : kind, std::string(operation) + ": " + text);
}
AvFrameRef make_frame() {
    auto *frame = av_frame_alloc();
    if (!frame) throw std::bad_alloc();
    return AvFrameRef(frame, [](AVFrame *p) { av_frame_free(&p); });
}
}
CpuPreparer::~CpuPreparer() { sws_freeContext(scaler_); }
void CpuPreparer::configure_scaler(int width, int height, AVPixelFormat format, AVChromaLocation chroma) {
    // Match the YUV presenter default. sws_scale receives raw planes, so it
    // cannot read AVFrame.chroma_location; the phase must be set before init.
    if (chroma == AVCHROMA_LOC_UNSPECIFIED) chroma = AVCHROMA_LOC_LEFT;
    if (scaler_ && scaler_width_ == width && scaler_height_ == height &&
        scaler_format_ == format && scaler_chroma_ == chroma) return;
    int x = 0, y = 0;
    check(av_chroma_location_enum_to_pos(&x, &y, chroma), "resolve chroma location", MediaFailure::Unsupported);
    std::unique_ptr<SwsContext, decltype(&sws_freeContext)> next(sws_alloc_context(), sws_freeContext);
    if (!next) throw std::bad_alloc();
    const std::pair<const char *, int> options[] = {
        {"srcw", width}, {"srch", height}, {"src_format", format},
        {"dstw", width}, {"dsth", height}, {"dst_format", AV_PIX_FMT_RGBA},
        // Accurate rounding also bypasses the unscaled shortcut which does
        // not honor explicit chroma phase in the pinned FFmpeg converter.
        {"sws_flags", SWS_BILINEAR | SWS_FULL_CHR_H_INT | SWS_ACCURATE_RND},
        {"src_h_chr_pos", x}, {"src_v_chr_pos", y}
    };
    for (const auto &option : options)
        check(av_opt_set_int(next.get(), option.first, option.second, 0), "set SDR scaler option", MediaFailure::Unsupported);
    check(sws_init_context(next.get(), nullptr, nullptr), "initialize SDR RGBA conversion", MediaFailure::Unsupported);
    sws_freeContext(scaler_); scaler_ = next.release();
    scaler_width_ = width; scaler_height_ = height; scaler_format_ = format; scaler_chroma_ = chroma;
}
std::shared_ptr<PreparedFrame> CpuPreparer::prepare(FrameLease lease) {
    ScopedTiming preparation(config_.diagnostics ? &counters_.prepare_total : nullptr);
    auto source = lease.frame;
    if (source->hw_frames_ctx) {
        ScopedTiming readback(config_.diagnostics ? &counters_.cpu_readback : nullptr);
        auto downloaded = make_frame();
        check(av_hwframe_transfer_data(downloaded.get(), source.get(), 0), "download hardware frame", MediaFailure::Backend);
        check(av_frame_copy_props(downloaded.get(), source.get()), "copy frame metadata");
        ++counters_.cpu_readbacks;
        int bytes = av_image_get_buffer_size(static_cast<AVPixelFormat>(downloaded->format), downloaded->width, downloaded->height, 1);
        if (bytes > 0) counters_.cpu_readback_bytes += bytes;
        source = std::move(downloaded);
    }
    auto prepared = std::make_shared<PreparedFrame>(); prepared->info = lease.info;
    auto format = static_cast<AVPixelFormat>(source->format);
    bool rgba = config_.transfer == "rgba" || (format != AV_PIX_FMT_YUV420P && format != AV_PIX_FMT_YUVJ420P && format != AV_PIX_FMT_NV12);
    if (rgba) {
        ScopedTiming conversion(config_.diagnostics ? &counters_.rgba_conversion : nullptr);
        const size_t left = source->crop_left, right = source->crop_right;
        const size_t top = source->crop_top, bottom = source->crop_bottom;
        if (source->width <= 0 || source->height <= 0 || left >= size_t(source->width) ||
            right >= size_t(source->width)-left || top >= size_t(source->height) || bottom >= size_t(source->height)-top)
            throw MediaError(MediaFailure::Input, "Invalid visible RGBA crop");
        CpuPlane plane; plane.width = source->width-int(left+right); plane.height = source->height-int(top+bottom); plane.channels = 4;
        prepared->info.width = plane.width; prepared->info.height = plane.height;
        plane.pixels.resize(size_t(plane.width)*plane.height*4);
        configure_scaler(source->width, source->height, format, lease.info.chroma);
        int matrix = lease.info.matrix == AVCOL_SPC_BT709 ? SWS_CS_ITU709 :
            lease.info.matrix == AVCOL_SPC_BT2020_NCL ? SWS_CS_BT2020 : SWS_CS_ITU601;
        auto *coefficients = sws_getCoefficients(matrix);
        check(sws_setColorspaceDetails(scaler_, coefficients, lease.info.range == AVCOL_RANGE_JPEG,
            coefficients, 1, 0, 1<<16, 1<<16), "set SDR conversion matrix");
        // Preserve chroma neighbors at the visible boundary and odd crop phase.
        // Convert in coded coordinates, then crop RGB. Uncropped video writes
        // directly into the output buffer and needs no extra copy.
        bool cropped = left || right || top || bottom;
        std::vector<uint8_t> coded_pixels;
        if (cropped) coded_pixels.resize(size_t(source->width)*source->height*4);
        uint8_t *output[4]{cropped ? coded_pixels.data() : plane.pixels.data(), nullptr, nullptr, nullptr};
        int strides[4]{source->width*4,0,0,0};
        if (sws_scale(scaler_, source->data, source->linesize, 0, source->height, output, strides) != source->height)
            throw std::runtime_error("Incomplete RGBA conversion");
        if (cropped) for (int row = 0; row < plane.height; ++row)
            std::memcpy(plane.pixels.data()+size_t(row)*plane.width*4,
                coded_pixels.data()+((size_t(row)+top)*source->width+left)*4, size_t(plane.width)*4);
        prepared->planes.push_back(std::move(plane)); prepared->path = "cpu_rgba"; ++counters_.converted_rgba;
        if (lease.info.bit_depth > 8) { prepared->info.precision_reduced = true; ++counters_.precision_reduced; }
    } else {
        ScopedTiming packing(config_.diagnostics ? &counters_.plane_pack : nullptr);
        // Plane paths retain their native layout and signed line stride.
        check(av_frame_apply_cropping(source.get(), AV_FRAME_CROP_UNALIGNED), "apply visible crop");
        prepared->info.width = source->width; prepared->info.height = source->height;
        int count = format == AV_PIX_FMT_NV12 ? 2 : 3;
        for (int i = 0; i < count; ++i) {
            CpuPlane plane; plane.width = i ? (source->width+1)/2 : source->width;
            plane.height = i ? (source->height+1)/2 : source->height;
            plane.channels = i && format == AV_PIX_FMT_NV12 ? 2 : 1;
            size_t row_bytes = size_t(plane.width)*plane.channels;
            if (!source->data[i] || uint64_t(std::abs(source->linesize[i])) < row_bytes)
                throw std::runtime_error("Invalid decoded plane stride");
            plane.pixels.resize(row_bytes*plane.height);
            for (int y = 0; y < plane.height; ++y)
                std::memcpy(plane.pixels.data()+size_t(y)*row_bytes, source->data[i]+ptrdiff_t(y)*source->linesize[i], row_bytes);
            prepared->planes.push_back(std::move(plane));
        }
        prepared->path = format == AV_PIX_FMT_NV12 ? "cpu_nv12" : "cpu_yuv420p";
    }
    if (config_.diagnostics) prepared->info.timing.prepared_us = monotonic_us();
    return prepared;
}
}
