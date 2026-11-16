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
    if (!media.initial_parameters.empty() && !network.source_address && !media.initial_parameters_source)
        invalid = "Explicit HEVC parameters require a source IP filter";
    if (!invalid.empty()) {
        std::lock_guard<std::mutex> lock(mutex_); state_.error = invalid; state_.state = "Faulted"; return false;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        state_ = {}; preparation_ = {}; latest_.reset(); decoded_latest_ = {};
        learned_parameters_ = {};
        preparation_error_ = {}; preparation_epoch_ = 0;
        state_.state = "Starting"; state_.started_ms = monotonic_ms();
        state_.requested_decoder = media.decoder; state_.requested_transfer = media.transfer;
        state_.diagnostics = media.diagnostics;
        state_.initial_parameters_supplied = !media.initial_parameters.empty();
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
    discard_pending(Discard::Stop);
    if (state_.state != "Faulted") state_.state = done_.load() ? "Stopped" : "Stopping";
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
    auto result = std::exchange(latest_, {});
    if (result) prepare_ready_.notify_one();
    return result;
}
MediaSnapshot MediaSession::snapshot() const {
    MediaSnapshot snapshot;
    { std::lock_guard<std::mutex> lock(mutex_);
      snapshot = state_; snapshot.prepared_pending=bool(latest_); snapshot.decoded_pending=bool(decoded_latest_.frame);
      static_cast<CpuPreparationCounters &>(snapshot.decode) = preparation_;
      snapshot.snapshot_us=monotonic_us();
      snapshot.worker_done=done_.load(); }
    snapshot.receiver = receiver_.snapshot(); return snapshot;
}
LearnedParameters MediaSession::hevc_parameters() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return learned_parameters_;
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
        fallback_presentation(error);
        return true;
    }
    quit_.store(true); state_.error=error;state_.state="Faulted";
    discard_pending(Discard::Failure);
    return false;
}
void MediaSession::fallback_presentation(const std::string &reason) {
    if (quit_.load() || state_.gpu_disabled) return;
    state_.gpu_disabled = true; ++state_.presentation_fallbacks;
    state_.presentation_fallback = "GPU transfer -> CPU: " + reason;
    discard_pending(Discard::Generation);
    state_.path = "cpu_pending"; state_.state = "Recovering";
    state_.recovery_reason = state_.presentation_fallback;
}
void MediaSession::discard_pending(Discard reason) {
    if (decoded_latest_.frame) {
        if (reason == Discard::Stop) ++state_.decoded_discarded_stop;
        else if (reason == Discard::Generation) ++state_.decoded_discarded_generation;
        else ++state_.decoded_discarded_failure;
    }
    if (latest_) {
        if (reason == Discard::Stop) ++state_.prepared_discarded_stop;
        else if (reason == Discard::Generation) ++state_.prepared_discarded_generation;
        else ++state_.prepared_discarded_failure;
    }
    decoded_latest_ = {}; latest_.reset(); ++preparation_epoch_; preparation_error_ = {};
    state_.accepted_stream = state_.accepted_resource = 0;
    prepare_ready_.notify_all();
}
void MediaSession::complete_preparation(std::shared_ptr<PreparedFrame> prepared, uint64_t epoch) {
    ++state_.prepared_completed; state_.last_prepared = prepared->info;
    const auto &info = prepared->info;
    if (quit_.load() || state_.state == "Faulted" || epoch != preparation_epoch_ || info.stream_generation != state_.accepted_stream ||
        info.resource_generation != state_.accepted_resource || info.presentation_generation != state_.accepted_presentation) {
        ++state_.prepared_discarded_race; return;
    }
    if (latest_) ++state_.mailbox_replaced;
    state_.format = info;
    if (prepared->path != "gpu_pending" || state_.path.empty()) state_.path = prepared->path;
    state_.last_frame_ms = monotonic_ms(); ++state_.prepared;
    latest_ = std::move(prepared); state_.state = "Running"; state_.recovery_reason.clear();
}
void MediaSession::prepare(MediaConfig config) noexcept {
    try {
        CpuPreparer preparer(std::move(config));
        while (true) {
            FrameLease frame; uint64_t epoch;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                prepare_ready_.wait(lock, [&] {
                    return quit_.load() || (decoded_latest_.frame && !latest_ && !preparation_error_);
                });
                if (quit_.load()) break;
                frame = std::exchange(decoded_latest_, {}); epoch = preparation_epoch_;
                state_.preparing = true;
                if (state_.diagnostics) state_.preparation_queue_wait.observe(monotonic_us()-frame.info.timing.decoded_us);
            }
            std::shared_ptr<PreparedFrame> prepared;
            std::exception_ptr failure;
            try { prepared = preparer.prepare(std::move(frame)); }
            catch (...) { failure = std::current_exception(); }
            std::lock_guard<std::mutex> lock(mutex_);
            preparation_ = preparer.counters(); state_.preparing = false;
            if (failure) {
                ++state_.prepare_failures;
                if (!quit_.load() && epoch == preparation_epoch_) preparation_error_ = failure;
            } else if (!prepared) ++state_.decoded_capacity_drops;
            else complete_preparation(std::move(prepared), epoch);
        }
    } catch (...) {
        std::lock_guard<std::mutex> lock(mutex_);
        preparation_error_ = std::current_exception();
    }
}
void MediaSession::run(MediaConfig config) noexcept {
    try {
        std::unique_ptr<Decoder> decoder;
        decoder = std::make_unique<Decoder>(config, [&](FrameLease frame) {
            uint64_t epoch;
            bool gpu;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (config.transfer == "auto" && frame.info.decoder != "cuda")
                    fallback_presentation("CUDA/OpenGL bridge is unavailable for " + frame.info.decoder);
                frame.info.presentation_generation = state_.gpu_disabled ? 1 : 0;
                state_.last_decoded = frame.info;
                state_.decode=decoder->counters();
                if (quit_.load()) { ++state_.decoded_discarded_stop; return; }
                if (frame.info.stream_generation != state_.accepted_stream || frame.info.resource_generation != state_.accepted_resource ||
                    frame.info.presentation_generation != state_.accepted_presentation) {
                    discard_pending(Discard::Generation);
                    state_.accepted_stream = frame.info.stream_generation; state_.accepted_resource = frame.info.resource_generation;
                    state_.accepted_presentation = frame.info.presentation_generation;
                }
                epoch = preparation_epoch_;
                gpu = config.transfer == "gpu" || (config.transfer == "auto" && !state_.gpu_disabled);
                if (!gpu) {
                    if (decoded_latest_.frame) ++state_.decoded_replaced;
                    decoded_latest_ = std::move(frame);
                    state_.cpu_sources_peak = std::max(state_.cpu_sources_peak, uint64_t(1+unsigned(state_.preparing)));
                } else state_.preparing = true;
            }
            if (!gpu) {
                // Only the decoder worker creates/joins this thread. GPU-only
                // sessions retain their direct publication path.
                if (!preparer_.joinable()) preparer_ = std::thread([this, config] { prepare(config); });
                prepare_ready_.notify_one(); return;
            }
            std::shared_ptr<PreparedFrame> prepared;
            try {
                prepared=std::make_shared<PreparedFrame>();prepared->info=frame.info;
                prepared->path="gpu_pending";prepared->gpu_source=std::move(frame);
                if (config.diagnostics) prepared->info.timing.prepared_us=monotonic_us();
            } catch (...) {
                std::lock_guard<std::mutex> lock(mutex_);
                state_.preparing=false;++state_.prepare_failures;throw;
            }
            std::lock_guard<std::mutex> lock(mutex_);
            state_.preparing=false;complete_preparation(std::move(prepared), epoch);
        }, true);
        auto update_decoder_status = [&] {
            std::lock_guard<std::mutex> lock(mutex_);
            state_.backend = decoder->backend(); state_.device = decoder->device_identity();
            state_.fallback = decoder->fallback();
            state_.decode = decoder->counters(); state_.format_changes = state_.decode.format_changes;
            state_.initial_parameters_applied = decoder->initial_parameters_applied();
            if (learned_parameters_.revision != decoder->parameters_revision()) {
                learned_parameters_ = {decoder->validated_parameters(), decoder->parameters_source(), decoder->parameters_revision()};
                state_.hevc_parameters_revision = learned_parameters_.revision;
            }
        };
        update_decoder_status();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!quit_.load()) state_.state = "WaitingConfig";
        }
        uint64_t current_generation = 0;
        auto invalidate = [&](const std::string &reason, bool error) {
            std::lock_guard<std::mutex> lock(mutex_);
            discard_pending(Discard::Generation);
            ++state_.recoveries; if (error) ++state_.decode_errors;
            state_.state = "Recovering"; state_.recovery_reason = reason;
        };
        auto recover = [&](const MediaError &error) {
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
                if (config.transfer == "auto" && decoder->backend() != "cuda") {
                    std::lock_guard<std::mutex> lock(mutex_);
                    fallback_presentation("decoder changed to " + decoder->backend());
                }
            } else { update_decoder_status(); throw error; }
        };
        while (!quit_.load()) {
            std::exception_ptr failure;
            { std::lock_guard<std::mutex> lock(mutex_); failure = preparation_error_; }
            if (failure) {
                try { std::rethrow_exception(failure); }
                catch (const MediaError &error) { recover(error); }
                update_decoder_status();
                continue;
            }
            auto group = receiver_.pop();
            if (group) {
                if (current_generation && (group->generation != current_generation || group->discontinuity_before))
                    invalidate(group->generation != current_generation ? "Sender restarted or source changed; waiting for a random access point" : "Compressed stream discontinuity; waiting for a random access point", false);
                current_generation = group->generation;
                try { decoder->feed(std::move(*group)); }
                catch (const MediaError &error) { recover(error); }
                update_decoder_status();
            } else {
                auto status = receiver_.snapshot();
                if (!status.running) throw std::runtime_error(status.error.empty() ? "UDP receiver stopped unexpectedly" : status.error);
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
    } catch (const std::exception &error) {
        std::lock_guard<std::mutex> lock(mutex_);
        quit_.store(true); state_.error = error.what(); state_.state = "Faulted";
        discard_pending(Discard::Failure);
    } catch (...) {
        std::lock_guard<std::mutex> lock(mutex_);
        quit_.store(true); state_.error = "Unexpected decoder failure"; state_.state = "Faulted";
        discard_pending(Discard::Failure);
    }
    quit_.store(true); prepare_ready_.notify_all();
    if (preparer_.joinable()) preparer_.join();
    receiver_.stop();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        discard_pending(state_.state == "Faulted" ? Discard::Failure : Discard::Stop);
        if (state_.state != "Faulted") state_.state = "Stopped";
    }
    done_.store(true);
}
}
