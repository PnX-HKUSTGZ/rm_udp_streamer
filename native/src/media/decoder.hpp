#pragma once
#include "transport/protocol.hpp"
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
    std::string transfer = "yuv"; // yuv or rgba; GPU bridge adds its own preparation.
    int max_pixels = 1920 * 1088;
    bool allow_10bit_to_8bit = false;
    std::string validate() const;
};
struct FrameInfo {
    uint64_t stream_generation = 0, resource_generation = 0, output_sequence = 0;
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
    // Protocol groups have no media timestamps. A parser may join or split
    // groups; receive-to-display latency is deliberately not fabricated here.
    Millis decoded_ms = 0;
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
    AVPixelFormat hardware_format_ = AV_PIX_FMT_NONE;
    DecodeCounters counters_;
    std::string backend_, identity_, fallback_;
    uint64_t generation_ = 0, resources_ = 1, output_sequence_ = 0;
    size_t parser_bytes_ = 0;
    FrameInfo last_format_;
    bool awaiting_key_ = true, discard_rasl_ = false;
    HevcConfiguration parameters_;
    HevcConfiguration validated_parameters_;
    uint64_t validated_generation_ = 0;
    void open(const std::string &backend);
    void close_codec();
    void reset(uint64_t generation);
    void send(const uint8_t *data, int size);
    void receive();
    static AVPixelFormat choose_format(AVCodecContext *, const AVPixelFormat *);
};
}
