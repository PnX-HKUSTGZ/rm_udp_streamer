#pragma once
#include "transport/protocol.hpp"
#include "diagnostics/frame_timing.hpp"
#include "hevc_config.hpp"
#include <functional>
#include <memory>
#include <string>
#include <vector>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <libswscale/swscale.h>
}

namespace rm {
struct MediaConfig {
    std::string decoder = "auto"; // auto, software, cuda, d3d11va
    std::string device = "0";
    std::string transfer = "yuv"; // auto, yuv, rgba, or required GPU bridge.
    int max_pixels = 1920 * 1088;
    bool allow_10bit_to_8bit = false;
    bool diagnostics = false;
    std::string validate() const;
};
struct FrameInfo {
    uint64_t stream_generation = 0, resource_generation = 0, output_sequence = 0, presentation_generation = 0;
    int coded_width = 0, coded_height = 0, width = 0, height = 0;
    int crop_left = 0, crop_top = 0, crop_right = 0, crop_bottom = 0;
    int bit_depth = 8;
    AVRational sample_aspect{1, 1};
    AVColorRange range = AVCOL_RANGE_UNSPECIFIED;
    AVColorSpace matrix = AVCOL_SPC_UNSPECIFIED;
    AVColorPrimaries primaries = AVCOL_PRI_UNSPECIFIED;
    AVColorTransferCharacteristic transfer = AVCOL_TRC_UNSPECIFIED;
    AVChromaLocation chroma = AVCHROMA_LOC_UNSPECIFIED;
    bool color_defaulted = false, hardware = false, precision_reduced = false;
    // Protocol groups have no media timestamps. Only proven parser byte-range
    // matches carry local input timing; media_pts is never fabricated.
    Millis decoded_ms = 0;
    FrameTiming timing;
    int64_t media_pts = AV_NOPTS_VALUE;
    std::string decoder, device, pixel_format;
};
using AvFrameRef = std::shared_ptr<AVFrame>;
struct FrameLease {
    FrameInfo info;
    AvFrameRef frame;
};
struct CpuPlane {
    int width = 0, height = 0, channels = 1;
    std::vector<uint8_t> pixels;
};
struct PreparedFrame {
    FrameInfo info;
    std::string path;
    std::vector<CpuPlane> planes;
    FrameLease gpu_source; // Empty for CPU preparation; owns GPU source for a bridge.
};
struct DecodeCounters {
    uint64_t parser_packets = 0, decoded = 0, hardware_frames = 0;
    uint64_t cpu_readbacks = 0, cpu_readback_bytes = 0, converted_rgba = 0;
    uint64_t skipped_until_key = 0, skipped_rasl = 0, resets = 0, precision_reduced = 0, format_changes = 0;
    uint64_t backend_attempts = 0, backend_failures = 0, backend_switches = 0;
    uint64_t correlated_frames = 0, unknown_timing_frames = 0, timing_history_evicted = 0;
    size_t timing_history_peak = 0;
    TimingHistogram parser_cpu, packet_prepare_cpu, decode_send_cpu, decode_receive_cpu;
    TimingHistogram cpu_readback, rgba_conversion, plane_pack, prepare_total;
};

class Decoder {
public:
    using Output = std::function<void(FrameLease)>;
    explicit Decoder(MediaConfig config, Output output);
    ~Decoder();
    Decoder(const Decoder &) = delete;
    Decoder &operator=(const Decoder &) = delete;
    void feed(EncodedGroup group);
    void finish();
    void recover();
    // Called only after feed/finish has unwound: parser output may alias its buffer.
    bool fallback_backend(const std::string &reason);
    bool waiting_for_key() const { return awaiting_key_; }
    std::shared_ptr<PreparedFrame> prepare_cpu(FrameLease lease);
    const DecodeCounters &counters() const { return counters_; }
    const std::string &backend() const { return backend_; }
    const std::string &device_identity() const { return identity_; }
    const std::string &fallback() const { return fallback_; }
private:
    MediaConfig config_;
    Output output_;
    AVCodecContext *codec_ = nullptr;
    AVCodecParserContext *parser_ = nullptr;
    AVBufferRef *device_ = nullptr;
    AVPacket *packet_ = nullptr;
    AVFrame *scratch_ = nullptr;
    SwsContext *scaler_ = nullptr;
    int scaler_width_ = 0, scaler_height_ = 0;
    AVPixelFormat scaler_format_ = AV_PIX_FMT_NONE;
    AVChromaLocation scaler_chroma_ = AVCHROMA_LOC_UNSPECIFIED;
    AVPixelFormat hardware_format_ = AV_PIX_FMT_NONE;
    DecodeCounters counters_;
    std::string backend_, identity_, fallback_;
    std::vector<std::string> candidates_;
    size_t next_candidate_ = 0;
    bool negotiation_failed_ = false;
    uint64_t generation_ = 0, resources_ = 1, output_sequence_ = 0;
    size_t parser_bytes_ = 0;
    int64_t parser_input_offset_ = 0;
    InputHistory input_history_;
    FrameInfo last_format_;
    bool awaiting_key_ = true, discard_rasl_ = false;
    HevcConfiguration parameters_;
    HevcConfiguration validated_parameters_;
    uint64_t validated_generation_ = 0;
    void open(const std::string &backend);
    void open_next();
    void record_failure(const std::string &reason);
    void check_decode(int code, const char *operation);
    void close_codec();
    void reset(uint64_t generation);
    void send(const uint8_t *data, int size, FrameTiming timing = {});
    FrameTiming packet_timing(int length, bool eof = false);
    void receive();
    void configure_scaler(int width, int height, AVPixelFormat format, AVChromaLocation chroma);
    static AVPixelFormat choose_format(AVCodecContext *, const AVPixelFormat *);
};
}
