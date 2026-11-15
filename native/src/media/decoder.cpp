#include "decoder.hpp"
#include "device.hpp"
#include <algorithm>
#include <climits>
#include <cstring>
#include <stdexcept>
#include <iomanip>
#include <sstream>
extern "C" {
#include <libavutil/hwcontext.h>
#include <libavutil/pixdesc.h>
}

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
}
std::string MediaConfig::validate() const {
    if (decoder != "auto" && decoder != "software" && decoder != "cuda" && decoder != "d3d11va" && decoder != "vaapi")
        return "decoder must be auto, software, cuda, d3d11va or vaapi";
    if (transfer != "auto" && transfer != "yuv" && transfer != "rgba" && transfer != "gpu") return "transfer must be auto, yuv, rgba or gpu";
    if (transfer == "gpu" && decoder != "cuda") return "GPU transfer currently requires explicit CUDA decoding";
    auto device_error = validate_device_selector(decoder, device);
    if (!device_error.empty()) return device_error;
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
        candidates_ = decoder_candidates(config_.decoder, config_.transfer, config_.device);
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
        open_hardware_device(backend, config_.device, &device_, hardware_format_, identity_);
        codec_->hw_device_ctx = av_buffer_ref(device_);
        if (!codec_->hw_device_ctx) throw std::bad_alloc();
        codec_->opaque = this; codec_->get_format = choose_format;
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
    if (size < 0 || !data) throw MediaError(MediaFailure::Input, "Invalid HEVC parser packet");
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
        const size_t prefix_size = prefixed.size();
        if (prefix_size > size_t(INT_MAX) || size_t(size) > size_t(INT_MAX)-prefix_size)
            throw MediaError(MediaFailure::Input, "HEVC packet with cached parameters exceeds packet size limit");
        // Bound the packet's int-sized API before growing its storage. Explicit
        // resize/copy also avoids GCC 13's vector range-insert overflow warning.
        prefixed.resize(prefix_size+size_t(size));
        std::memcpy(prefixed.data()+prefix_size,data,size_t(size));
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
std::shared_ptr<PreparedFrame> Decoder::prepare_cpu(FrameLease lease) {
    if (!synchronous_preparer_) synchronous_preparer_ = std::make_unique<CpuPreparer>(config_);
    try {
        auto frame = synchronous_preparer_->prepare(std::move(lease));
        static_cast<CpuPreparationCounters &>(counters_) = synchronous_preparer_->counters();
        if (!frame) throw std::runtime_error("CPU preparation output budget exhausted; release a previous frame");
        return frame;
    } catch (...) {
        static_cast<CpuPreparationCounters &>(counters_) = synchronous_preparer_->counters();
        throw;
    }
}
}
