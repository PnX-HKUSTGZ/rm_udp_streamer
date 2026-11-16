#pragma once
#include "transport/protocol.hpp"
#include "diagnostics/frame_timing.hpp"
#include <memory>
#include <string>
#include <vector>
extern "C" {
#include <libavutil/frame.h>
}

namespace rm {
struct MediaConfig {
    std::string decoder = "auto"; // auto, software, cuda, d3d11va, vaapi
    std::string device = "0";
    std::string transfer = "yuv"; // auto, yuv, rgba, or required GPU bridge.
    int max_pixels = 1920 * 1088;
    bool allow_10bit_to_8bit = false;
    bool diagnostics = false;
    // Optional explicit Annex-B VPS/SPS/PPS for a fixed, source-filtered input.
    std::vector<uint8_t> initial_parameters;
    // A persisted hint is only applied to groups from this IPv4 address.
    // Zero retains the explicit-parameter API's required transport filter.
    uint32_t initial_parameters_source = 0;
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
struct CpuPreparationCounters {
    uint64_t cpu_readbacks = 0, cpu_readback_bytes = 0, converted_rgba = 0, precision_reduced = 0;
    uint64_t output_buffers_allocated = 0, output_buffer_growths = 0, output_capacity_peak_bytes = 0;
    uint64_t scratch_buffer_growths = 0, scratch_capacity_peak_bytes = 0;
    uint64_t readback_allocations = 0, readback_capacity_peak_bytes = 0, preparation_pool_misses = 0;
    uint64_t plane_copy_bytes = 0, rgba_crop_copy_bytes = 0;
    TimingHistogram cpu_readback, rgba_conversion, plane_pack, prepare_total;
};
}
