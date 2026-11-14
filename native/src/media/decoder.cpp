#include "decoder.hpp"
#include <algorithm>
#include <cstring>
#include <stdexcept>
extern "C" {
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>
}
#ifdef _WIN32
#include <libavutil/hwcontext_d3d11va.h>
#include <dxgi.h>
#endif

namespace rm {
namespace {
void check(int code, const char *operation) {
    if (code >= 0) return;
    char text[AV_ERROR_MAX_STRING_SIZE]{};
    av_strerror(code, text, sizeof(text));
    throw std::runtime_error(std::string(operation) + ": " + text);
}
AvFrameRef make_frame() {
    auto *frame = av_frame_alloc();
    if (!frame) throw std::bad_alloc();
    return AvFrameRef(frame, [](AVFrame *p) { av_frame_free(&p); });
}
struct Nal { size_t start, end; int type; };
std::vector<Nal> nals(const uint8_t *data, size_t size) {
    std::vector<Nal> result;
    for (size_t i = 0; i + 4 < size;) {
        size_t prefix = 0;
        if (!data[i] && !data[i+1]) {
            if (data[i+2] == 1) prefix = 3;
            else if (!data[i+2] && data[i+3] == 1) prefix = 4;
        }
        if (!prefix) { ++i; continue; }
        if (!result.empty()) result.back().end = i;
        result.push_back({i, size, int((data[i+prefix] >> 1) & 63)});
        i += prefix + 2;
    }
    return result;
}
}
std::string MediaConfig::validate() const {
    if (decoder != "auto" && decoder != "software" && decoder != "cuda" && decoder != "d3d11va")
        return "decoder must be auto, software, cuda or d3d11va";
    if (transfer != "yuv" && transfer != "rgba") return "transfer must be yuv or rgba";
    if (device.empty() || device.size() > 3 || device.find_first_not_of("0123456789") != std::string::npos)
        return "device must be a numeric adapter index (0..999)";
    if (max_pixels < 256 || max_pixels > 4096 * 2304) return "max_pixels outside 256..9437184";
    return {};
}
AVPixelFormat Decoder::choose_format(AVCodecContext *context, const AVPixelFormat *formats) {
    auto *self = static_cast<Decoder *>(context->opaque);
    for (auto p = formats; *p != AV_PIX_FMT_NONE; ++p)
        if (*p == self->hardware_format_) return *p;
    return AV_PIX_FMT_NONE; // A requested hardware path cannot return software silently.
}
Decoder::Decoder(MediaConfig config, Output output) : config_(std::move(config)), output_(std::move(output)) {
    auto invalid = config_.validate();
    if (!invalid.empty()) throw std::invalid_argument(invalid);
    try {
        std::vector<std::string> candidates;
        if (config_.decoder == "auto") {
#ifdef _WIN32
            candidates = {"d3d11va", "cuda", "software"};
#else
            candidates = {"cuda", "software"};
#endif
        } else candidates = {config_.decoder};
        for (const auto &candidate : candidates) {
            try { open(candidate); break; }
            catch (const std::exception &error) {
                close_codec();
                if (config_.decoder != "auto" || candidate == candidates.back()) throw;
                if (!fallback_.empty()) fallback_ += "; ";
                fallback_ += candidate + ": " + error.what();
            }
        }
        parser_ = av_parser_init(AV_CODEC_ID_HEVC);
        packet_ = av_packet_alloc(); scratch_ = av_frame_alloc();
        if (!parser_ || !packet_ || !scratch_) throw std::bad_alloc();
    } catch (...) {
        close_codec();
        if (parser_) av_parser_close(parser_);
        av_packet_free(&packet_); av_frame_free(&scratch_);
        throw;
    }
}
void Decoder::close_codec() {
    avcodec_free_context(&codec_); av_buffer_unref(&device_);
}
Decoder::~Decoder() {
    sws_freeContext(scaler_);
    av_packet_free(&packet_); av_frame_free(&scratch_);
    if (parser_) av_parser_close(parser_);
    close_codec();
}
void Decoder::open(const std::string &backend) {
    auto *decoder = avcodec_find_decoder(AV_CODEC_ID_HEVC);
    if (!decoder) throw std::runtime_error("Pinned FFmpeg HEVC decoder is unavailable");
    codec_ = avcodec_alloc_context3(decoder);
    if (!codec_) throw std::bad_alloc();
    hardware_format_ = AV_PIX_FMT_NONE;
    backend_ = backend; identity_ = backend == "software" ? "CPU" : backend + ":" + config_.device;
    if (backend != "software") {
        auto type = av_hwdevice_find_type_by_name(backend.c_str());
        for (int i = 0; const auto *entry = avcodec_get_hw_config(decoder, i); ++i) {
            if (entry->device_type == type && (entry->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX)) {
                hardware_format_ = entry->pix_fmt; break;
            }
        }
        if (hardware_format_ == AV_PIX_FMT_NONE) throw std::runtime_error("Backend absent from fixed FFmpeg build");
        check(av_hwdevice_ctx_create(&device_, type, config_.device.c_str(), nullptr, 0), "create hardware device");
        codec_->hw_device_ctx = av_buffer_ref(device_);
        if (!codec_->hw_device_ctx) throw std::bad_alloc();
        codec_->opaque = this; codec_->get_format = choose_format;
#ifdef _WIN32
        if (backend == "d3d11va") {
            auto *hw = reinterpret_cast<AVHWDeviceContext *>(device_->data);
            auto *d3d = reinterpret_cast<AVD3D11VADeviceContext *>(hw->hwctx);
            IDXGIDevice *dxgi = nullptr; IDXGIAdapter *adapter = nullptr;
            if (SUCCEEDED(d3d->device->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void **>(&dxgi)))) {
                if (SUCCEEDED(dxgi->GetAdapter(&adapter))) {
                    DXGI_ADAPTER_DESC desc{};
                    if (SUCCEEDED(adapter->GetDesc(&desc))) {
                        char label[512]{};
                        WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, label, sizeof(label), nullptr, nullptr);
                        identity_ = std::string(label) + " LUID=" + std::to_string(desc.AdapterLuid.HighPart) + ":" + std::to_string(desc.AdapterLuid.LowPart);
                    }
                    adapter->Release();
                }
                dxgi->Release();
            }
        }
#endif
    }
    codec_->max_pixels = config_.max_pixels;
    codec_->thread_count = 2; codec_->thread_type = FF_THREAD_SLICE;
    codec_->apply_cropping = 0;
    codec_->err_recognition = AV_EF_CAREFUL;
    check(avcodec_open2(codec_, decoder, nullptr), "open HEVC decoder");
}
void Decoder::reset(uint64_t generation) {
    if (parser_) av_parser_close(parser_);
    parser_ = av_parser_init(AV_CODEC_ID_HEVC);
    if (!parser_) throw std::bad_alloc();
    avcodec_flush_buffers(codec_);
    generation_ = generation; ++resources_; ++counters_.resets;
    parser_bytes_ = 0; awaiting_key_ = true;
    // Conservative recovery: never combine stale parameter sets with a new
    // stream. The sender must repeat a coherent VPS/SPS/PPS at an IDR.
    for (auto &parameter : parameters_) parameter.clear();
}
void Decoder::feed(EncodedGroup group) {
    if (generation_ != group.generation || group.discontinuity_before) reset(group.generation);
    size_t size = group.bytes.size();
    group.bytes.resize(size + AV_INPUT_BUFFER_PADDING_SIZE, 0);
    size_t offset = 0;
    while (offset < size) {
        uint8_t *payload = nullptr; int length = 0;
        int used = av_parser_parse2(parser_, codec_, &payload, &length, group.bytes.data()+offset,
            static_cast<int>(size-offset), AV_NOPTS_VALUE, AV_NOPTS_VALUE, -1);
        check(used, "parse HEVC");
        if (!used && !length) throw std::runtime_error("HEVC parser made no progress");
        offset += used; parser_bytes_ += used;
        if (parser_bytes_ > 8 * 1024 * 1024) throw std::runtime_error("HEVC access unit exceeds parser budget");
        if (length) { send(payload, length); parser_bytes_ = 0; }
    }
}
void Decoder::send(const uint8_t *data, int size) {
    if (!size) return;
    ++counters_.parser_packets;
    auto units = nals(data, size);
    bool idr = false;
    for (const auto &nal : units) {
        if (nal.type >= 32 && nal.type <= 34) {
            if (nal.end - nal.start > 65536) throw std::runtime_error("Oversized HEVC parameter set");
            parameters_[nal.type-32].assign(data+nal.start, data+nal.end);
        }
        idr |= nal.type == 19 || nal.type == 20;
    }
    std::vector<uint8_t> prefixed;
    if (awaiting_key_) {
        if (!idr || parameters_[0].empty() || parameters_[1].empty() || parameters_[2].empty()) {
            ++counters_.skipped_until_key; return;
        }
        for (const auto &parameter : parameters_) prefixed.insert(prefixed.end(), parameter.begin(), parameter.end());
        prefixed.insert(prefixed.end(), data, data+size);
        data = prefixed.data(); size = static_cast<int>(prefixed.size());
        awaiting_key_ = false;
    }
    check(av_new_packet(packet_, size), "allocate parser packet");
    std::memcpy(packet_->data, data, size);
    int result = avcodec_send_packet(codec_, packet_);
    if (result == AVERROR(EAGAIN)) { receive(); result = avcodec_send_packet(codec_, packet_); }
    av_packet_unref(packet_);
    check(result, "submit HEVC packet"); receive();
}
void Decoder::receive() {
    while (true) {
        int result = avcodec_receive_frame(codec_, scratch_);
        if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) return;
        check(result, "decode HEVC");
        auto source = make_frame(); av_frame_move_ref(source.get(), scratch_);
        auto &f = *source;
        if (f.width <= 0 || f.height <= 0 || int64_t(f.width)*f.height > config_.max_pixels)
            throw std::runtime_error("Decoded image exceeds pixel budget");
        if (f.flags & AV_FRAME_FLAG_CORRUPT) throw std::runtime_error("Corrupt HEVC output requires recovery");
        bool hardware = f.hw_frames_ctx != nullptr;
        if (backend_ != "software" && (!hardware || f.format != hardware_format_))
            throw std::runtime_error("Requested hardware decoder did not produce hardware frames");
        auto pixel_format = static_cast<AVPixelFormat>(f.format);
        if (hardware) pixel_format = reinterpret_cast<AVHWFramesContext *>(f.hw_frames_ctx->data)->sw_format;
        auto *description = av_pix_fmt_desc_get(pixel_format);
        if (!description) throw std::runtime_error("Unknown decoded pixel layout");
        if (f.color_trc == AVCOL_TRC_SMPTE2084 || f.color_trc == AVCOL_TRC_ARIB_STD_B67)
            throw std::runtime_error("HDR PQ/HLG is not supported by the SDR presenter");
        if (description->comp[0].depth > 8 && !config_.allow_10bit_to_8bit)
            throw std::runtime_error("Main10 requires explicit allow_10bit_to_8bit conversion");
        if (f.width != last_width_ || f.height != last_height_ || f.format != last_format_) {
            ++resources_; last_width_ = f.width; last_height_ = f.height; last_format_ = f.format;
        }
        FrameInfo info;
        info.stream_generation = generation_; info.resource_generation = resources_; info.output_sequence = ++output_sequence_;
        info.coded_width = f.width; info.coded_height = f.height;
        info.crop_left = static_cast<int>(f.crop_left); info.crop_top = static_cast<int>(f.crop_top);
        info.crop_right = static_cast<int>(f.crop_right); info.crop_bottom = static_cast<int>(f.crop_bottom);
        info.width = f.width-info.crop_left-info.crop_right; info.height = f.height-info.crop_top-info.crop_bottom;
        if (info.width <= 0 || info.height <= 0) throw std::runtime_error("Invalid visible crop");
        info.sample_aspect = f.sample_aspect_ratio.num > 0 && f.sample_aspect_ratio.den > 0 ? f.sample_aspect_ratio : AVRational{1,1};
        info.bit_depth = description->comp[0].depth;
        info.range = f.color_range; info.matrix = f.colorspace; info.primaries = f.color_primaries;
        info.transfer = f.color_trc; info.chroma = f.chroma_location;
        info.color_defaulted = info.range == AVCOL_RANGE_UNSPECIFIED || info.matrix == AVCOL_SPC_UNSPECIFIED;
        if (info.range == AVCOL_RANGE_UNSPECIFIED) info.range = AVCOL_RANGE_MPEG;
        if (info.matrix == AVCOL_SPC_UNSPECIFIED) info.matrix = info.height >= 720 ? AVCOL_SPC_BT709 : AVCOL_SPC_SMPTE170M;
        if (info.matrix != AVCOL_SPC_BT709 && info.matrix != AVCOL_SPC_SMPTE170M &&
            info.matrix != AVCOL_SPC_BT470BG && info.matrix != AVCOL_SPC_BT2020_NCL)
            throw std::runtime_error("Unsupported SDR matrix; refusing incorrect colors");
        info.hardware = hardware; info.decoder = backend_; info.device = identity_;
        info.pixel_format = av_get_pix_fmt_name(pixel_format);
        info.decoded_ms = monotonic_ms(); info.media_pts = f.pts;
        ++counters_.decoded; if (hardware) ++counters_.hardware_frames;
        output_({std::move(info), std::move(source)});
    }
}
void Decoder::finish() {
    uint8_t *data = nullptr; int size = 0;
    check(av_parser_parse2(parser_, codec_, &data, &size, nullptr, 0, AV_NOPTS_VALUE, AV_NOPTS_VALUE, -1), "finish parser");
    send(data, size);
    int result = avcodec_send_packet(codec_, nullptr);
    if (result == AVERROR(EAGAIN)) { receive(); result = avcodec_send_packet(codec_, nullptr); }
    check(result, "finish decoder"); receive();
}
std::shared_ptr<PreparedFrame> Decoder::prepare_cpu(FrameLease lease) {
    auto source = lease.frame;
    if (source->hw_frames_ctx) {
        auto downloaded = make_frame();
        check(av_hwframe_transfer_data(downloaded.get(), source.get(), 0), "download hardware frame");
        check(av_frame_copy_props(downloaded.get(), source.get()), "copy frame metadata");
        ++counters_.cpu_readbacks;
        int bytes = av_image_get_buffer_size(static_cast<AVPixelFormat>(downloaded->format), downloaded->width, downloaded->height, 1);
        if (bytes > 0) counters_.cpu_readback_bytes += bytes;
        source = std::move(downloaded);
    }
    // Crop in owned AVFrame metadata, respecting chroma and signed line stride.
    check(av_frame_apply_cropping(source.get(), AV_FRAME_CROP_UNALIGNED), "apply visible crop");
    auto prepared = std::make_shared<PreparedFrame>(); prepared->info = lease.info;
    prepared->info.width = source->width; prepared->info.height = source->height;
    auto format = static_cast<AVPixelFormat>(source->format);
    bool rgba = config_.transfer == "rgba" || (format != AV_PIX_FMT_YUV420P && format != AV_PIX_FMT_YUVJ420P && format != AV_PIX_FMT_NV12);
    if (rgba) {
        CpuPlane plane; plane.width = source->width; plane.height = source->height; plane.channels = 4;
        plane.pixels.resize(size_t(plane.width)*plane.height*4);
        scaler_ = sws_getCachedContext(scaler_, plane.width, plane.height, format, plane.width, plane.height,
            AV_PIX_FMT_RGBA, SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (!scaler_) throw std::runtime_error("Cannot initialize SDR RGBA conversion");
        int matrix = lease.info.matrix == AVCOL_SPC_BT709 ? SWS_CS_ITU709 :
            lease.info.matrix == AVCOL_SPC_BT2020_NCL ? SWS_CS_BT2020 : SWS_CS_ITU601;
        auto *coefficients = sws_getCoefficients(matrix);
        check(sws_setColorspaceDetails(scaler_, coefficients, lease.info.range == AVCOL_RANGE_JPEG,
            coefficients, 1, 0, 1<<16, 1<<16), "set SDR conversion matrix");
        uint8_t *output[4]{plane.pixels.data(), nullptr, nullptr, nullptr}; int strides[4]{plane.width*4,0,0,0};
        if (sws_scale(scaler_, source->data, source->linesize, 0, plane.height, output, strides) != plane.height)
            throw std::runtime_error("Incomplete RGBA conversion");
        prepared->planes.push_back(std::move(plane)); prepared->path = "cpu_rgba"; ++counters_.converted_rgba;
        if (lease.info.bit_depth > 8) { prepared->info.precision_reduced = true; ++counters_.precision_reduced; }
    } else {
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
    return prepared;
}
}
