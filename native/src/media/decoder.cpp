#include "decoder.hpp"
#include "interop/gpu_api.hpp"
#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <iomanip>
#include <sstream>
extern "C" {
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>
#include <libavutil/opt.h>
typedef rm::gpu::Context CUcontext;
typedef rm::gpu::Stream CUstream;
#define CUDA_VERSION 7050
#include <libavutil/hwcontext_cuda.h>
}
#ifdef _WIN32
#include <libavutil/hwcontext_d3d11va.h>
#include <dxgi1_2.h>
#endif

namespace rm {
namespace {
struct TimingOpaque {
    static constexpr uint64_t tag = 0x524d54494d450001ULL;
    uint64_t magic = tag;
    FrameTiming timing;
};
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
bool same_format(const FrameInfo &a, const FrameInfo &b) {
    return a.coded_width==b.coded_width && a.coded_height==b.coded_height && a.width==b.width && a.height==b.height &&
        a.crop_left==b.crop_left && a.crop_top==b.crop_top && a.crop_right==b.crop_right && a.crop_bottom==b.crop_bottom &&
        a.bit_depth==b.bit_depth && a.sample_aspect.num==b.sample_aspect.num && a.sample_aspect.den==b.sample_aspect.den &&
        a.range==b.range && a.matrix==b.matrix && a.primaries==b.primaries && a.transfer==b.transfer && a.chroma==b.chroma &&
        a.pixel_format==b.pixel_format && a.hardware==b.hardware && a.device==b.device && a.decoder==b.decoder;
}
std::string cuda_identity(AVBufferRef *device) {
    using namespace rm::gpu;
    struct Uuid { unsigned char bytes[16]; };
    using Push = int(RM_GPU_CALL *)(Context);
    using Pop = int(RM_GPU_CALL *)(Context *);
    using GetDevice = int(RM_GPU_CALL *)(int *);
    using Name = int(RM_GPU_CALL *)(char *, int, int);
    using GetUuid = int(RM_GPU_CALL *)(Uuid *, int);
    auto push = reinterpret_cast<Push>(symbol("nvcuda.dll", "cuCtxPushCurrent_v2"));
    auto pop = reinterpret_cast<Pop>(symbol("nvcuda.dll", "cuCtxPopCurrent_v2"));
    auto get = reinterpret_cast<GetDevice>(symbol("nvcuda.dll", "cuCtxGetDevice"));
    auto name = reinterpret_cast<Name>(symbol("nvcuda.dll", "cuDeviceGetName"));
    auto uuid = reinterpret_cast<GetUuid>(symbol("nvcuda.dll", "cuDeviceGetUuid_v2"));
    if (!uuid) uuid = reinterpret_cast<GetUuid>(symbol("nvcuda.dll", "cuDeviceGetUuid"));
    if (!push || !pop || !get || !name || !uuid) throw std::runtime_error("CUDA device identity API unavailable");
    auto *hw = reinterpret_cast<AVHWDeviceContext *>(device->data);
    auto *cuda = reinterpret_cast<AVCUDADeviceContext *>(hw->hwctx);
    if (push(cuda->cuda_ctx)) throw std::runtime_error("Cannot query CUDA device identity");
    int index = -1; char label[256]{}; Uuid id{};
    int status = get(&index);
    if (!status) status = name(label, sizeof(label), index);
    if (!status) status = uuid(&id, index);
    Context ignored; int popped = pop(&ignored);
    if (status || popped) throw std::runtime_error("Cannot read actual CUDA device name/UUID");
    std::ostringstream out; out << label << " CUDA=" << index << " UUID=GPU-" << std::hex << std::setfill('0');
    for (int i = 0; i < 16; ++i) { if (i == 4 || i == 6 || i == 8 || i == 10) out << '-'; out << std::setw(2) << unsigned(id.bytes[i]); }
    return out.str();
}
#ifdef _WIN32
std::string adapter_identity(IDXGIAdapter *adapter) {
    DXGI_ADAPTER_DESC desc{};
    if (FAILED(adapter->GetDesc(&desc))) throw std::runtime_error("Cannot query D3D11 adapter identity");
    char label[512]{};
    if (!WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, label, sizeof(label), nullptr, nullptr))
        throw std::runtime_error("Cannot read D3D11 adapter name");
    return std::string(label) + " LUID=" + std::to_string(desc.AdapterLuid.HighPart) + ":" + std::to_string(desc.AdapterLuid.LowPart);
}
std::string requested_d3d11_adapter(const std::string &index) {
    // FFmpeg may pass a null adapter to D3D11CreateDevice after EnumAdapters
    // fails, which selects the default GPU. Reject that silent device change.
    using CreateFactory = HRESULT(WINAPI *)(REFIID, void **);
    auto module = GetModuleHandleA("dxgi.dll"); if (!module) module = LoadLibraryA("dxgi.dll");
    auto create = module ? reinterpret_cast<CreateFactory>(GetProcAddress(module,"CreateDXGIFactory1")) : nullptr;
    IDXGIFactory1 *factory = nullptr; IDXGIAdapter *adapter = nullptr;
    if (!create || FAILED(create(__uuidof(IDXGIFactory1),reinterpret_cast<void **>(&factory))))
        throw std::runtime_error("Cannot enumerate requested D3D11 adapter");
    HRESULT result = factory->EnumAdapters(static_cast<UINT>(std::stoul(index)), &adapter);
    factory->Release();
    if (FAILED(result) || !adapter) throw std::runtime_error("D3D11 adapter index " + index + " does not exist");
    try { auto identity = adapter_identity(adapter); adapter->Release(); return identity; }
    catch (...) { adapter->Release(); throw; }
}
#endif
}
std::string MediaConfig::validate() const {
    if (decoder != "auto" && decoder != "software" && decoder != "cuda" && decoder != "d3d11va")
        return "decoder must be auto, software, cuda or d3d11va";
    if (transfer != "auto" && transfer != "yuv" && transfer != "rgba" && transfer != "gpu") return "transfer must be auto, yuv, rgba or gpu";
    if (transfer == "gpu" && decoder != "cuda") return "GPU transfer currently requires explicit CUDA decoding";
    if (device.empty() || device.size() > 3 || device.find_first_not_of("0123456789") != std::string::npos)
        return "device must be a numeric adapter index (0..999)";
    if (max_pixels < 256 || max_pixels > 4096 * 2304) return "max_pixels outside 256..9437184";
    return {};
}
AVPixelFormat Decoder::choose_format(AVCodecContext *context, const AVPixelFormat *formats) {
    auto *self = static_cast<Decoder *>(context->opaque);
    for (auto p = formats; *p != AV_PIX_FMT_NONE; ++p)
        if (*p == self->hardware_format_) return *p;
    // FFmpeg retries get_format without the hardware format when its actual
    // profile/layout initialization fails. Do not misclassify that as packet loss.
    self->negotiation_failed_ = true;
    return AV_PIX_FMT_NONE; // A requested hardware path cannot return software silently.
}
Decoder::Decoder(MediaConfig config, Output output) : config_(std::move(config)), output_(std::move(output)) {
    auto invalid = config_.validate();
    if (!invalid.empty()) throw std::invalid_argument(invalid);
    try {
        if (config_.decoder == "auto") {
#ifdef _WIN32
            candidates_ = config_.transfer == "auto" ? std::vector<std::string>{"cuda", "d3d11va", "software"} :
                std::vector<std::string>{"d3d11va", "cuda", "software"};
#else
            candidates_ = {"cuda", "software"};
#endif
        } else candidates_ = {config_.decoder};
        open_next();
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
void Decoder::record_failure(const std::string &reason) {
    ++counters_.backend_failures;
    if (!fallback_.empty()) fallback_ += "; ";
    fallback_ += backend_ + ": " + reason;
}
void Decoder::open_next() {
    // Monotonic candidate cursor is the per-session negative cache. Failed
    // hardware is never retried on a later frame, source or format change.
    while (next_candidate_ < candidates_.size()) {
        auto candidate = candidates_[next_candidate_++];
        ++counters_.backend_attempts;
        try { open(candidate); return; }
        catch (const std::exception &error) { close_codec(); record_failure(error.what()); }
    }
    throw MediaError(MediaFailure::Backend, "Decoder candidates exhausted: " + fallback_);
}
bool Decoder::fallback_backend(const std::string &reason) {
    record_failure(reason);
    if (config_.decoder != "auto" || backend_ == "software") return false;
    close_codec(); open_next(); ++counters_.backend_switches;
    // Preserve parsed parameter sets, discard old parser/codec output and wait
    // for the next matched IRAP. Never reuse memory owned by an unwound parser.
    reset(generation_);
    return true;
}
void Decoder::check_decode(int code, const char *operation) {
    if (negotiation_failed_) throw MediaError(MediaFailure::Backend,
        backend_ + " cannot initialize the stream's actual hardware format/profile");
    check(code, operation);
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
    negotiation_failed_ = false;
    backend_ = backend; identity_ = backend == "software" ? "CPU" : backend + ":" + config_.device;
    if (backend != "software") {
        auto type = av_hwdevice_find_type_by_name(backend.c_str());
        for (int i = 0; const auto *entry = avcodec_get_hw_config(decoder, i); ++i) {
            if (entry->device_type == type && (entry->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX)) {
                hardware_format_ = entry->pix_fmt; break;
            }
        }
        if (hardware_format_ == AV_PIX_FMT_NONE) throw std::runtime_error("Backend absent from fixed FFmpeg build");
#ifdef _WIN32
        std::string requested_adapter;
        if (backend == "d3d11va") requested_adapter = requested_d3d11_adapter(config_.device);
#endif
        check(av_hwdevice_ctx_create(&device_, type, config_.device.c_str(), nullptr, 0), "create hardware device", MediaFailure::Backend);
        codec_->hw_device_ctx = av_buffer_ref(device_);
        if (!codec_->hw_device_ctx) throw std::bad_alloc();
        codec_->opaque = this; codec_->get_format = choose_format;
        if (backend == "cuda") identity_ = cuda_identity(device_);
#ifdef _WIN32
        if (backend == "d3d11va") {
            auto *hw = reinterpret_cast<AVHWDeviceContext *>(device_->data);
            auto *d3d = reinterpret_cast<AVD3D11VADeviceContext *>(hw->hwctx);
            IDXGIDevice *dxgi = nullptr; IDXGIAdapter *adapter = nullptr;
            if (FAILED(d3d->device->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void **>(&dxgi))))
                throw std::runtime_error("Cannot identify actual D3D11 device");
            HRESULT result = dxgi->GetAdapter(&adapter); dxgi->Release();
            if (FAILED(result) || !adapter) throw std::runtime_error("Cannot identify actual D3D11 adapter");
            try { identity_ = adapter_identity(adapter); adapter->Release(); }
            catch (...) { adapter->Release(); throw; }
            if (identity_ != requested_adapter) throw std::runtime_error("Actual D3D11 adapter differs from requested device");
        }
#endif
    }
    codec_->max_pixels = config_.max_pixels;
    if (config_.diagnostics) codec_->flags |= AV_CODEC_FLAG_COPY_OPAQUE;
    codec_->thread_count = 2; codec_->thread_type = FF_THREAD_SLICE;
    codec_->apply_cropping = 0;
    codec_->err_recognition = AV_EF_CAREFUL;
    check(avcodec_open2(codec_, decoder, nullptr), "open HEVC decoder", MediaFailure::Backend);
}
void Decoder::reset(uint64_t generation) {
    if (parser_) av_parser_close(parser_);
    parser_ = av_parser_init(AV_CODEC_ID_HEVC);
    if (!parser_) throw std::bad_alloc();
    avcodec_flush_buffers(codec_);
    if (generation_ != generation) { parameters_.clear(); validated_parameters_.clear(); validated_generation_ = 0; }
    generation_ = generation; ++resources_; ++counters_.resets;
    parser_bytes_ = 0; parser_input_offset_ = 0; input_history_.clear();
    awaiting_key_ = true; discard_rasl_ = false;
    auto configuration = parameters_.all();
    if (!configuration.empty()) {
        int length = static_cast<int>(configuration.size());
        configuration.resize(configuration.size()+AV_INPUT_BUFFER_PADDING_SIZE, 0);
        uint8_t *output = nullptr; int output_size = 0;
        int consumed = av_parser_parse2(parser_, codec_, &output, &output_size, configuration.data(), length,
            AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
        check(consumed, "restore parser parameters");
        if (consumed != length || output_size) throw MediaError(MediaFailure::Input, "Invalid cached parser configuration");
        parser_bytes_ = length;
        parser_input_offset_ = length;
    }
}
void Decoder::recover() {
    // Failed parameter updates must not poison subsequent recovery. Only a
    // bundle that has produced a valid frame may replace the recovery cache.
    if (validated_generation_ == generation_) parameters_ = validated_parameters_;
    else parameters_.clear();
    reset(generation_);
}
void Decoder::feed(EncodedGroup group) {
    if (generation_ != group.generation || group.discontinuity_before) reset(group.generation);
    size_t size = group.bytes.size();
    if (size > size_t(std::numeric_limits<int64_t>::max()-parser_input_offset_))
        throw MediaError(MediaFailure::Input, "Parser byte position exhausted");
    if (config_.diagnostics) {
        FrameTiming timing; timing.enabled = true; timing.correlation = Correlation::ExactGroup;
        timing.input_serial = group.serial; timing.input_frame_id = group.frame_id;
        timing.ingress_us = group.first_packet_ms*1000; timing.complete_us = group.complete_ms*1000;
        timing.queued_us = group.queued_ms*1000; timing.dequeued_us = group.dequeued_ms*1000;
        if (timing.ingress_us <= 0 || timing.complete_us < timing.ingress_us ||
            timing.queued_us < timing.complete_us || timing.dequeued_us < timing.queued_us)
            timing.unknown(Correlation::MissingClock);
        input_history_.add({parser_input_offset_,parser_input_offset_+int64_t(size),timing});
        counters_.timing_history_evicted = input_history_.evicted;
        counters_.timing_history_peak = input_history_.peak;
    }
    group.bytes.resize(size + AV_INPUT_BUFFER_PADDING_SIZE, 0);
    size_t offset = 0;
    while (offset < size) {
        uint8_t *payload = nullptr; int length = 0;
        int used;
        {
            ScopedTiming timer(config_.diagnostics ? &counters_.parser_cpu : nullptr);
            used = av_parser_parse2(parser_, codec_, &payload, &length, group.bytes.data()+offset,
                static_cast<int>(size-offset), AV_NOPTS_VALUE, AV_NOPTS_VALUE, parser_input_offset_);
        }
        check(used, "parse HEVC");
        if (!used && !length) throw MediaError(MediaFailure::Input, "HEVC parser made no progress");
        offset += used; parser_bytes_ += used; parser_input_offset_ += used;
        if (parser_bytes_ > 8 * 1024 * 1024) throw MediaError(MediaFailure::Input, "HEVC access unit exceeds parser budget");
        if (length) { send(payload, length, packet_timing(length)); parser_bytes_ = 0; }
    }
}
FrameTiming Decoder::packet_timing(int length, bool eof) {
    if (!config_.diagnostics) return {};
    // At EOF the HEVC parser may report a negative sentinel as its next
    // offset. The actual consumed end is known; take still requires that the
    // output length exactly covers this range and one complete input group.
    auto timing = input_history_.take(parser_->frame_offset, eof ? parser_input_offset_ : parser_->next_frame_offset, length);
    timing.packet_ready_us = monotonic_us();
    return timing;
}
void Decoder::send(const uint8_t *data, int size, FrameTiming timing) {
    if (!size) return;
    Micros prepare_started = config_.diagnostics ? monotonic_us() : 0;
    ++counters_.parser_packets;
    auto picture = parameters_.observe(data, size);
    if (timing.enabled && picture.first_slices != 1) timing.unknown(Correlation::NotSinglePicture);
    if(timing.correlated())counters_.dequeue_to_packet.observe(timing.packet_ready_us-timing.dequeued_us);
    std::vector<uint8_t> prefixed;
    if (awaiting_key_) {
        auto configuration = parameters_.for_picture(picture.pps_id);
        if (picture.random_access < 16 || picture.random_access > 21 || configuration.empty()) {
            ++counters_.skipped_until_key; return;
        }
        prefixed = std::move(configuration);
        prefixed.insert(prefixed.end(), data, data+size);
        data = prefixed.data(); size = static_cast<int>(prefixed.size());
        awaiting_key_ = false;
        discard_rasl_ = picture.random_access == 21 || picture.random_access <= 18;
    }
    if (discard_rasl_ && picture.rasl) { ++counters_.skipped_rasl; return; }
    if (discard_rasl_ && picture.has_vcl && !picture.leading && picture.random_access < 0) discard_rasl_ = false;
    check(av_new_packet(packet_, size), "allocate parser packet");
    struct PacketScope {
        AVPacket *packet;
        ~PacketScope() { av_packet_unref(packet); }
    } packet_scope{packet_};
    std::memcpy(packet_->data, data, size);
    if (config_.diagnostics) {
        TimingOpaque trace; trace.timing = timing;
        packet_->opaque_ref = av_buffer_alloc(sizeof(trace));
        if (!packet_->opaque_ref) throw std::bad_alloc();
        std::memcpy(packet_->opaque_ref->data, &trace, sizeof(trace));
        counters_.packet_prepare_cpu.observe(monotonic_us()-prepare_started);
    }
    auto submit = [&] {
        ScopedTiming timer(config_.diagnostics ? &counters_.decode_send_cpu : nullptr);
        return avcodec_send_packet(codec_, packet_);
    };
    int result = submit();
    if (result == AVERROR(EAGAIN)) { receive(); result = submit(); }
    av_packet_unref(packet_);
    check_decode(result, "submit HEVC packet"); receive();
}
void Decoder::receive() {
    while (true) {
        int result;
        {
            ScopedTiming timer(config_.diagnostics ? &counters_.decode_receive_cpu : nullptr);
            result = avcodec_receive_frame(codec_, scratch_);
        }
        if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) return;
        check_decode(result, "decode HEVC");
        auto source = make_frame(); av_frame_move_ref(source.get(), scratch_);
        auto &f = *source;
        if (f.width <= 0 || f.height <= 0 || int64_t(f.width)*f.height > config_.max_pixels)
            throw MediaError(MediaFailure::Unsupported, "Decoded image exceeds pixel budget");
        if (f.flags & AV_FRAME_FLAG_CORRUPT) throw MediaError(MediaFailure::Input, "Corrupt HEVC output requires recovery");
        bool hardware = f.hw_frames_ctx != nullptr;
        if (backend_ != "software" && (!hardware || f.format != hardware_format_))
            throw MediaError(MediaFailure::Backend, "Requested hardware decoder did not produce hardware frames");
        auto pixel_format = static_cast<AVPixelFormat>(f.format);
        if (hardware) pixel_format = reinterpret_cast<AVHWFramesContext *>(f.hw_frames_ctx->data)->sw_format;
        auto *description = av_pix_fmt_desc_get(pixel_format);
        if (!description) throw MediaError(MediaFailure::Unsupported, "Unknown decoded pixel layout");
        if (f.color_trc == AVCOL_TRC_SMPTE2084 || f.color_trc == AVCOL_TRC_ARIB_STD_B67)
            throw MediaError(MediaFailure::Unsupported, "HDR PQ/HLG is not supported by the SDR presenter");
        if (f.color_primaries != AVCOL_PRI_UNSPECIFIED && f.color_primaries != AVCOL_PRI_BT709)
            throw MediaError(MediaFailure::Unsupported, "Unsupported SDR color primaries: display gamut conversion is not implemented");
        if (f.color_trc != AVCOL_TRC_UNSPECIFIED && f.color_trc != AVCOL_TRC_BT709 &&
            f.color_trc != AVCOL_TRC_SMPTE170M && f.color_trc != AVCOL_TRC_IEC61966_2_1 &&
            f.color_trc != AVCOL_TRC_BT2020_10 && f.color_trc != AVCOL_TRC_BT2020_12)
            throw MediaError(MediaFailure::Unsupported, "Unsupported transfer function: nonlinear SDR input is required");
        if (description->comp[0].depth > 8 && !config_.allow_10bit_to_8bit)
            throw MediaError(MediaFailure::Unsupported, "Main10 requires explicit allow_10bit_to_8bit conversion");
        FrameInfo info;
        info.stream_generation = generation_; info.output_sequence = ++output_sequence_;
        info.coded_width = f.width; info.coded_height = f.height;
        if (f.crop_left+f.crop_right >= size_t(f.width) || f.crop_top+f.crop_bottom >= size_t(f.height))
            throw MediaError(MediaFailure::Input, "Invalid visible crop");
        info.crop_left = static_cast<int>(f.crop_left); info.crop_top = static_cast<int>(f.crop_top);
        info.crop_right = static_cast<int>(f.crop_right); info.crop_bottom = static_cast<int>(f.crop_bottom);
        info.width = f.width-info.crop_left-info.crop_right; info.height = f.height-info.crop_top-info.crop_bottom;
        if (info.width <= 0 || info.height <= 0) throw MediaError(MediaFailure::Input, "Invalid visible crop");
        info.sample_aspect = f.sample_aspect_ratio.num > 0 && f.sample_aspect_ratio.den > 0 ? f.sample_aspect_ratio : AVRational{1,1};
        info.bit_depth = description->comp[0].depth;
        info.range = f.color_range; info.matrix = f.colorspace; info.primaries = f.color_primaries;
        info.transfer = f.color_trc; info.chroma = f.chroma_location;
        info.color_defaulted = info.range == AVCOL_RANGE_UNSPECIFIED || info.matrix == AVCOL_SPC_UNSPECIFIED ||
            info.primaries == AVCOL_PRI_UNSPECIFIED || info.transfer == AVCOL_TRC_UNSPECIFIED || info.chroma == AVCHROMA_LOC_UNSPECIFIED;
        if (info.range == AVCOL_RANGE_UNSPECIFIED) info.range = AVCOL_RANGE_MPEG;
        if (info.matrix == AVCOL_SPC_UNSPECIFIED) info.matrix = info.height >= 720 ? AVCOL_SPC_BT709 : AVCOL_SPC_SMPTE170M;
        if (info.matrix != AVCOL_SPC_BT709 && info.matrix != AVCOL_SPC_SMPTE170M &&
            info.matrix != AVCOL_SPC_BT470BG && info.matrix != AVCOL_SPC_BT2020_NCL)
            throw MediaError(MediaFailure::Unsupported, "Unsupported SDR matrix; refusing incorrect colors");
        info.hardware = hardware; info.decoder = backend_; info.device = identity_;
        info.pixel_format = av_get_pix_fmt_name(pixel_format);
        if (!same_format(info, last_format_)) { ++resources_; ++counters_.format_changes; last_format_ = info; }
        info.resource_generation = resources_;
        info.decoded_ms = monotonic_ms(); info.media_pts = f.pts;
        if (config_.diagnostics) {
            info.timing.enabled = true; info.timing.correlation = Correlation::MissingOpaque;
            if (f.opaque_ref && f.opaque_ref->size == sizeof(TimingOpaque)) {
                TimingOpaque trace; std::memcpy(&trace, f.opaque_ref->data, sizeof(trace));
                if (trace.magic == TimingOpaque::tag) info.timing = trace.timing;
            }
            info.timing.decoded_us = monotonic_us();
            if(info.timing.packet_ready_us>0)counters_.packet_to_decoded.observe(info.timing.decoded_us-info.timing.packet_ready_us);
            if (info.timing.correlated()) ++counters_.correlated_frames;
            else ++counters_.unknown_timing_frames;
        }
        ++counters_.decoded; if (hardware) ++counters_.hardware_frames;
        output_({std::move(info), std::move(source)});
        if (validated_generation_ != generation_ || validated_parameters_.revision() != parameters_.revision()) {
            validated_parameters_ = parameters_; validated_generation_ = generation_;
        }
    }
}
void Decoder::finish() {
    uint8_t *data = nullptr; int size = 0;
    check(av_parser_parse2(parser_, codec_, &data, &size, nullptr, 0, AV_NOPTS_VALUE, AV_NOPTS_VALUE, parser_input_offset_), "finish parser");
    send(data, size, packet_timing(size, true));
    int result = avcodec_send_packet(codec_, nullptr);
    if (result == AVERROR(EAGAIN)) { receive(); result = avcodec_send_packet(codec_, nullptr); }
    check_decode(result, "finish decoder"); receive();
}
void Decoder::configure_scaler(int width, int height, AVPixelFormat format, AVChromaLocation chroma) {
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
std::shared_ptr<PreparedFrame> Decoder::prepare_cpu(FrameLease lease) {
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
