#include "session.hpp"
#include <chrono>
#include <stdexcept>
#include <utility>

namespace rm {
MediaSession::~MediaSession() { stop(); }
bool MediaSession::start(const TransportConfig &network, const MediaConfig &media) {
    poll_stop();
    if (!done_.load()) return false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if(state_.upload_requested != state_.submitted_total+state_.upload_stale_before+
            state_.upload_stale_after+state_.gpu_slot_drops+state_.upload_failures) return false;
    }
    auto invalid = media.validate();
    if (!invalid.empty()) {
        std::lock_guard<std::mutex> lock(mutex_); state_.error = invalid; state_.state = "Faulted"; return false;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        state_ = {}; latest_.reset(); state_.state = "Starting"; state_.started_ms = monotonic_ms();
        state_.requested_decoder = media.decoder; state_.requested_transfer = media.transfer;
        state_.diagnostics = media.diagnostics;
    }
    if (!receiver_.start(network)) {
        std::lock_guard<std::mutex> lock(mutex_); state_.error = receiver_.snapshot().error; state_.state = "Faulted"; return false;
    }
    quit_.store(false); done_.store(false);
    try { worker_ = std::thread([this, media] { run(media); }); }
    catch (const std::exception &error) {
        receiver_.stop(); done_.store(true);
        std::lock_guard<std::mutex> lock(mutex_); state_.error = error.what(); state_.state = "Faulted"; return false;
    }
    return true;
}
void MediaSession::request_stop() {
    quit_.store(true);
    std::lock_guard<std::mutex> lock(mutex_);
    if (latest_) ++state_.prepared_discarded_stop;
    latest_.reset();
    state_.accepted_stream = state_.accepted_resource = 0;
    if (!done_.load()) state_.state = "Stopping";
    else if (state_.state != "Faulted") state_.state = "Stopped";
}
void MediaSession::poll_stop() {
    if (done_.load() && worker_.joinable()) worker_.join();
}
void MediaSession::stop() {
    request_stop();
    if (worker_.joinable()) worker_.join();
    receiver_.stop();
}
std::shared_ptr<PreparedFrame> MediaSession::take_frame() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (latest_) {
        ++state_.upload_requested;
        if (state_.diagnostics) {
            latest_->info.timing.upload_queued_us = monotonic_us();
            state_.prepared_wait.observe(latest_->info.timing.upload_queued_us-latest_->info.timing.prepared_us);
        }
    }
    return std::exchange(latest_, {});
}
MediaSnapshot MediaSession::snapshot() const {
    MediaSnapshot snapshot;
    { std::lock_guard<std::mutex> lock(mutex_);
      snapshot = state_; snapshot.prepared_pending=bool(latest_); snapshot.snapshot_us=monotonic_us();
      snapshot.worker_done=done_.load(); }
    snapshot.receiver = receiver_.snapshot(); return snapshot;
}
bool MediaSession::accepts(const FrameInfo &info) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return !quit_.load() && state_.state != "Faulted" && info.stream_generation == state_.accepted_stream &&
        info.resource_generation == state_.accepted_resource && info.presentation_generation == state_.accepted_presentation &&
        info.decoded_ms && monotonic_ms()-info.decoded_ms < 2000;
}
bool MediaSession::presented(FrameInfo &info,const std::string &path,Micros upload_cpu_us) {
    std::lock_guard<std::mutex> lock(mutex_);
    if(!quit_.load()&&info.stream_generation==state_.accepted_stream&&info.resource_generation==state_.accepted_resource&&
        info.presentation_generation==state_.accepted_presentation) {
        // Stamp while committing under the same lock as the count windows.
        // A later callback cannot backdate a supposedly closed window.
        info.timing.submitted_us=monotonic_us();state_.submission_windows.add(info.timing.submitted_us);
        state_.path=path; state_.last_submitted=info; ++state_.submitted_total;
        if (state_.diagnostics) {
            state_.render_dispatch_wait.observe(info.timing.upload_started_us-info.timing.upload_queued_us);
            state_.upload_cpu.observe(upload_cpu_us);
            state_.submission_commit_wait.observe(info.timing.submitted_us-info.timing.upload_completed_us);
            if (info.timing.correlated()) {
                ++state_.correlated_submitted;
                state_.ingress_to_submit.observe(info.timing.submitted_us-info.timing.ingress_us);
            } else ++state_.unknown_submitted;
        }
        return true;
    }
    return false;
}
void MediaSession::shown(const FrameInfo &info) {
    std::lock_guard<std::mutex> lock(mutex_);
    state_.last_shown=info; ++state_.shown_total;
    if (state_.diagnostics) state_.submit_to_main_accept.observe(info.timing.shown_us-info.timing.submitted_us);
}
void MediaSession::presentation_event(PresentationEvent event) {
    std::lock_guard<std::mutex> lock(mutex_);
    switch (event) {
        case PresentationEvent::BusyPoll: ++state_.upload_busy_polls; break;
        case PresentationEvent::StaleBefore: ++state_.upload_stale_before; break;
        case PresentationEvent::StaleAfter: ++state_.upload_stale_after; break;
        case PresentationEvent::GpuBusy: ++state_.gpu_slot_drops; break;
        case PresentationEvent::UploadFailure: ++state_.upload_failures; break;
        case PresentationEvent::PublicationReplaced: ++state_.publication_replaced; break;
        case PresentationEvent::PublicationStale: ++state_.publication_stale; break;
        case PresentationEvent::ShownInvalidated: ++state_.shown_invalidated; break;
    }
}
void MediaSession::presentation_attempted() {
    std::lock_guard<std::mutex> lock(mutex_); ++state_.gpu_attempts;
}
bool MediaSession::presentation_failed(const std::string &error, bool gpu_failure) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (gpu_failure && state_.requested_transfer == "auto" && !quit_.load()) {
        if (!state_.gpu_disabled) {
            state_.gpu_disabled = true; ++state_.presentation_fallbacks;
            state_.presentation_fallback = "GPU transfer -> CPU: " + error;
            state_.accepted_stream = state_.accepted_resource = 0;
            if (latest_) ++state_.prepared_discarded_generation;
            latest_.reset();
            state_.path = "cpu_pending"; state_.state = "Recovering";
            state_.recovery_reason = state_.presentation_fallback;
        }
        return true;
    }
    quit_.store(true); state_.error=error;state_.state="Faulted";
    state_.accepted_stream=state_.accepted_resource=0;
    if (latest_) ++state_.prepared_discarded_failure;
    latest_.reset();
    return false;
}
void MediaSession::run(MediaConfig config) noexcept {
    try {
        std::unique_ptr<Decoder> decoder;
        decoder = std::make_unique<Decoder>(config, [&](FrameLease frame) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                state_.last_decoded=frame.info;
                state_.decode=decoder->counters();
                if (quit_.load()) { ++state_.decoded_discarded_stop; return; }
                state_.preparing=true;
            }
            if (config.transfer == "auto" && frame.info.decoder != "cuda")
                presentation_failed("CUDA/OpenGL bridge is unavailable for " + frame.info.decoder, true);
            bool gpu_disabled;
            { std::lock_guard<std::mutex> lock(mutex_); gpu_disabled = state_.gpu_disabled; }
            frame.info.presentation_generation = gpu_disabled ? 1 : 0;
            { std::lock_guard<std::mutex> lock(mutex_); state_.last_decoded=frame.info; }
            std::shared_ptr<PreparedFrame> prepared;
            try {
            if(config.transfer=="gpu" || (config.transfer=="auto" && !gpu_disabled)) {
                prepared=std::make_shared<PreparedFrame>();prepared->info=frame.info;
                prepared->path="gpu_pending";prepared->gpu_source=std::move(frame);
                if (config.diagnostics) prepared->info.timing.prepared_us=monotonic_us();
            } else prepared = decoder->prepare_cpu(std::move(frame));
            } catch (...) {
                std::lock_guard<std::mutex> lock(mutex_);
                state_.preparing=false;++state_.prepare_failures;state_.decode=decoder->counters();throw;
            }
            std::lock_guard<std::mutex> lock(mutex_);
            state_.preparing=false;state_.decode=decoder->counters();
            ++state_.prepared_completed; state_.last_prepared=prepared->info;
            if (quit_.load() || gpu_disabled != state_.gpu_disabled) { ++state_.prepared_discarded_race; return; }
            if (latest_) ++state_.mailbox_replaced;
            state_.format = prepared->info;
            if(prepared->path!="gpu_pending"||state_.path.empty())state_.path=prepared->path;
            state_.accepted_stream = prepared->info.stream_generation; state_.accepted_resource = prepared->info.resource_generation;
            state_.accepted_presentation = prepared->info.presentation_generation;
            state_.last_frame_ms = monotonic_ms(); ++state_.prepared;
            latest_ = std::move(prepared); state_.state = "Running"; state_.recovery_reason.clear();
        });
        auto update_decoder_status = [&] {
            std::lock_guard<std::mutex> lock(mutex_);
            state_.backend = decoder->backend(); state_.device = decoder->device_identity();
            state_.fallback = decoder->fallback();
            state_.decode = decoder->counters(); state_.format_changes = state_.decode.format_changes;
        };
        update_decoder_status();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!quit_.load()) state_.state = "WaitingConfig";
        }
        uint64_t current_generation = 0;
        auto invalidate = [&](const std::string &reason, bool error) {
            std::lock_guard<std::mutex> lock(mutex_);
            if (latest_) ++state_.prepared_discarded_generation;
            latest_.reset(); state_.accepted_stream = state_.accepted_resource = 0;
            ++state_.recoveries; if (error) ++state_.decode_errors;
            state_.state = "Recovering"; state_.recovery_reason = reason;
        };
        while (!quit_.load()) {
            auto group = receiver_.pop();
            if (group) {
                if (current_generation && (group->generation != current_generation || group->discontinuity_before))
                    invalidate(group->generation != current_generation ? "Sender restarted or source changed; waiting for a random access point" : "Compressed stream discontinuity; waiting for a random access point", false);
                current_generation = group->generation;
                try { decoder->feed(std::move(*group)); }
                catch (const MediaError &error) {
                    if (error.kind == MediaFailure::Input) {
                        invalidate(error.what(), true); decoder->recover();
                    } else if (error.kind == MediaFailure::Backend) {
                        invalidate(error.what(), false);
                        bool switched = false;
                        try { switched = decoder->fallback_backend(error.what()); }
                        catch (...) { update_decoder_status(); throw; }
                        if (!switched) {
                            update_decoder_status();
                            throw MediaError(MediaFailure::Backend, decoder->backend()+": "+error.what());
                        }
                        if (config.transfer == "auto" && decoder->backend() != "cuda")
                            presentation_failed("decoder changed to " + decoder->backend(), true);
                    } else { update_decoder_status(); throw; }
                }
                update_decoder_status();
            } else {
                auto status = receiver_.snapshot();
                if (!status.running) throw std::runtime_error(status.error.empty() ? "UDP receiver stopped unexpectedly" : status.error);
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
    } catch (const std::exception &error) {
        std::lock_guard<std::mutex> lock(mutex_); state_.error = error.what(); state_.state = "Faulted";
    } catch (...) {
        std::lock_guard<std::mutex> lock(mutex_); state_.error = "Unexpected decoder failure"; state_.state = "Faulted";
    }
    receiver_.stop();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (latest_) {
            if (state_.state=="Faulted") ++state_.prepared_discarded_failure;
            else ++state_.prepared_discarded_stop;
        }
        latest_.reset(); state_.accepted_stream = state_.accepted_resource = 0;
        if (state_.state != "Faulted") state_.state = "Stopped";
    }
    done_.store(true);
}
}
