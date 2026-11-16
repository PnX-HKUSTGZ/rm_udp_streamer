#pragma once
#include "decoder.hpp"
#include "transport/udp_receiver.hpp"
#include "diagnostics/submission_windows.hpp"
#include <atomic>
#include <condition_variable>
#include <exception>
#include <mutex>
#include <thread>

namespace rm {
enum class PresentationEvent { BusyPoll, StaleBefore, StaleAfter, GpuBusy, UploadFailure,
    PublicationReplaced, PublicationStale, ShownInvalidated };
struct MediaSnapshot {
    ReceiverSnapshot receiver;
    DecodeCounters decode;
    std::string state = "Stopped", error, backend, device, path, fallback, recovery_reason;
    std::string requested_decoder, requested_transfer, presentation_fallback;
    FrameInfo format;
    FrameInfo last_decoded, last_prepared, last_submitted, last_shown;
    uint64_t mailbox_replaced = 0, prepared = 0;
    uint64_t prepared_completed = 0, prepare_failures = 0, decoded_discarded_stop = 0, prepared_discarded_stop = 0;
    uint64_t decoded_replaced = 0, decoded_discarded_generation = 0, decoded_discarded_failure = 0;
    uint64_t decoded_capacity_drops = 0;
    bool preparing = false, prepared_pending = false, decoded_pending = false;
    uint64_t cpu_sources_peak = 0;
    TimingHistogram preparation_queue_wait;
    uint64_t prepared_discarded_generation = 0, prepared_discarded_failure = 0, prepared_discarded_race = 0;
    uint64_t upload_requested = 0, submitted_total = 0, shown_total = 0;
    uint64_t upload_busy_polls = 0, upload_stale_before = 0, upload_stale_after = 0, gpu_slot_drops = 0, upload_failures = 0;
    uint64_t publication_replaced = 0, publication_stale = 0, shown_invalidated = 0;
    uint64_t correlated_submitted = 0, unknown_submitted = 0;
    TimingHistogram prepared_wait, render_dispatch_wait, upload_cpu, submission_commit_wait, ingress_to_submit, submit_to_main_accept;
    SubmissionWindows submission_windows;
    Micros snapshot_us = 0;
    uint64_t recoveries = 0, decode_errors = 0, format_changes = 0;
    uint64_t accepted_stream = 0, accepted_resource = 0, accepted_presentation = 0;
    uint64_t gpu_attempts = 0, presentation_fallbacks = 0;
    bool gpu_disabled = false;
    bool worker_done = true;
    bool diagnostics = false;
    bool initial_parameters_supplied = false;
    bool initial_parameters_applied = false;
    uint64_t hevc_parameters_revision = 0;
    Millis last_frame_ms = 0, started_ms = 0;
};
struct LearnedParameters {
    std::vector<uint8_t> bytes;
    Endpoint source;
    uint64_t revision = 0;
};
class MediaSession {
public:
    MediaSession() = default;
    ~MediaSession();
    bool start(const TransportConfig &, const MediaConfig &);
    void request_stop();
    void poll_stop();
    void stop();
    std::shared_ptr<PreparedFrame> take_frame();
    bool accepts(const FrameInfo &) const;
    bool presented(FrameInfo &, const std::string &path, Micros upload_cpu_us = 0);
    void shown(const FrameInfo &);
    void presentation_event(PresentationEvent);
    void presentation_attempted();
    // Returns true only for a recoverable automatic GPU -> CPU transition.
    bool presentation_failed(const std::string &error, bool gpu_failure = false);
    MediaSnapshot snapshot() const;
    LearnedParameters hevc_parameters() const;
private:
    UdpReceiver receiver_;
    mutable std::mutex mutex_;
    std::thread worker_, preparer_;
    std::condition_variable prepare_ready_;
    std::atomic<bool> quit_{false}, done_{true};
    MediaSnapshot state_;
    LearnedParameters learned_parameters_;
    CpuPreparationCounters preparation_;
    FrameLease decoded_latest_;
    uint64_t preparation_epoch_ = 0;
    std::exception_ptr preparation_error_;
    std::shared_ptr<PreparedFrame> latest_;
    enum class Discard { Stop, Generation, Failure };
    // Caller holds mutex_. The epoch also invalidates same-format recovery.
    void discard_pending(Discard);
    // Caller holds mutex_. A capability notification cannot fault a stopping session.
    void fallback_presentation(const std::string &);
    void complete_preparation(std::shared_ptr<PreparedFrame>, uint64_t epoch);
    void prepare(MediaConfig) noexcept;
    void run(MediaConfig) noexcept;
};
}
