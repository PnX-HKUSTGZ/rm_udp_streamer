#pragma once
#include "frame.hpp"
#include <utility>
extern "C" {
#include <libswscale/swscale.h>
}

namespace rm {
// Owned by one preparation thread. It has no reference to the codec/parser;
// a FrameLease keeps its hardware frame/device alive across decoder recovery.
class CpuPreparer {
public:
    explicit CpuPreparer(MediaConfig config) : config_(std::move(config)) {}
    ~CpuPreparer();
    CpuPreparer(const CpuPreparer &) = delete;
    CpuPreparer &operator=(const CpuPreparer &) = delete;
    std::shared_ptr<PreparedFrame> prepare(FrameLease lease);
    const CpuPreparationCounters &counters() const { return counters_; }
private:
    MediaConfig config_;
    CpuPreparationCounters counters_;
    SwsContext *scaler_ = nullptr;
    int scaler_width_ = 0, scaler_height_ = 0;
    AVPixelFormat scaler_format_ = AV_PIX_FMT_NONE;
    AVChromaLocation scaler_chroma_ = AVCHROMA_LOC_UNSPECIFIED;
    void configure_scaler(int width, int height, AVPixelFormat format, AVChromaLocation chroma);
};
}
