#pragma once
#include "cpu_preparer.hpp"
#include "hevc_config.hpp"
#include <functional>
#include <memory>
#include <string>
#include <vector>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
}

namespace rm {
struct DecodeCounters : CpuPreparationCounters {
    uint64_t parser_packets = 0, decoded = 0, hardware_frames = 0;
    uint64_t skipped_until_key = 0, skipped_rasl = 0, resets = 0, format_changes = 0;
    uint64_t backend_attempts = 0, backend_failures = 0, backend_switches = 0;
    uint64_t correlated_frames = 0, unknown_timing_frames = 0, timing_history_evicted = 0;
    size_t timing_history_peak = 0;
    TimingHistogram parser_cpu, packet_prepare_cpu, decode_send_cpu, decode_receive_cpu;
    TimingHistogram dequeue_to_packet, packet_to_decoded;
};

class Decoder {
public:
    using Output = std::function<void(FrameLease)>;
    // Live sessions parse the first in-band configuration before a potentially
    // slow device open, so the bounded UDP queue cannot discard it meanwhile.
    explicit Decoder(MediaConfig config, Output output, bool defer_backend = false);
    ~Decoder();
    Decoder(const Decoder &) = delete;
    Decoder &operator=(const Decoder &) = delete;
    void feed(EncodedGroup group);
    void finish();
    void recover();
    // Called only after feed/finish has unwound: parser output may alias its buffer.
    bool fallback_backend(const std::string &reason);
    bool waiting_for_key() const { return awaiting_key_; }
    // Synchronous convenience API. MediaSession owns its separate preparer.
    std::shared_ptr<PreparedFrame> prepare_cpu(FrameLease lease);
    const DecodeCounters &counters() const { return counters_; }
    const std::string &backend() const { return backend_; }
    const std::string &device_identity() const { return identity_; }
    const std::string &fallback() const { return fallback_; }
    uint64_t parameters_revision() const { return parameters_revision_; }
    std::vector<uint8_t> validated_parameters() const { return validated_parameters_.all(); }
    Endpoint parameters_source() const { return validated_source_; }
    bool initial_parameters_applied() const { return initial_parameters_selected_ && !config_.initial_parameters.empty(); }
private:
    MediaConfig config_;
    Output output_;
    AVCodecContext *codec_ = nullptr;
    AVCodecParserContext *parser_ = nullptr;
    AVBufferRef *device_ = nullptr;
    AVPacket *packet_ = nullptr;
    AVFrame *scratch_ = nullptr;
    std::unique_ptr<CpuPreparer> synchronous_preparer_;
    AVPixelFormat hardware_format_ = AV_PIX_FMT_NONE;
    DecodeCounters counters_;
    std::string backend_, identity_, fallback_;
    std::vector<std::string> candidates_;
    size_t next_candidate_ = 0;
    bool negotiation_failed_ = false;
    uint64_t generation_ = 0, resources_ = 1, output_sequence_ = 0;
    uint64_t configuration_generation_ = 0;
    size_t parser_bytes_ = 0;
    int64_t parser_input_offset_ = 0;
    InputHistory input_history_;
    FrameInfo last_format_;
    bool awaiting_key_ = true, discard_rasl_ = false;
    HevcConfiguration parameters_;
    HevcConfiguration initial_parameters_;
    HevcConfiguration validated_parameters_;
    uint64_t validated_configuration_generation_ = 0;
    uint64_t parameters_revision_ = 0;
    Endpoint input_source_, validated_source_;
    bool initial_parameters_selected_ = false;
    void open(const std::string &backend);
    void open_next();
    void record_failure(const std::string &reason);
    void check_decode(int code, const char *operation);
    void close_codec();
    void reset(uint64_t generation, uint64_t configuration_generation);
    void send(const uint8_t *data, int size, FrameTiming timing = {});
    FrameTiming packet_timing(int length, bool eof = false);
    void receive();
    static AVPixelFormat choose_format(AVCodecContext *, const AVPixelFormat *);
};
}
