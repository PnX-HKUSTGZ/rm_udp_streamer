#include "video_session.hpp"
#include "video_surface.hpp"
#include "diagnostics.hpp"
#include "media/device.hpp"
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/godot.hpp>
#include <stdexcept>
using namespace godot;
namespace {
int64_t integer(const Dictionary &values, const char *key, int64_t fallback, int64_t minimum, int64_t maximum) {
    Variant value = values.get(key, fallback);
    if (value.get_type() != Variant::INT) throw std::invalid_argument(std::string(key) + " must be an integer");
    int64_t number = value;
    if (number < minimum || number > maximum) throw std::invalid_argument(std::string(key) + " is outside its valid range");
    return number;
}
std::string string(const Dictionary &values, const char *key, const char *fallback) {
    Variant value = values.get(key, fallback);
    if (value.get_type() != Variant::STRING) throw std::invalid_argument(std::string(key) + " must be a string");
    return String(value).utf8().get_data();
}
bool boolean(const Dictionary &values, const char *key, bool fallback) {
    Variant value = values.get(key, fallback);
    if (value.get_type() != Variant::BOOL) throw std::invalid_argument(std::string(key) + " must be boolean");
    return bool(value);
}
}
void RmVideoSession::_bind_methods() {
    ClassDB::bind_method(D_METHOD("start", "config"), &RmVideoSession::start);
    ClassDB::bind_method(D_METHOD("stop"), &RmVideoSession::stop);
    ClassDB::bind_method(D_METHOD("request_stop"), &RmVideoSession::request_stop);
    ClassDB::bind_method(D_METHOD("poll_stop"), &RmVideoSession::poll_stop);
    ClassDB::bind_method(D_METHOD("snapshot", "detailed"), &RmVideoSession::snapshot, DEFVAL(true));
    ClassDB::bind_method(D_METHOD("capabilities"), &RmVideoSession::capabilities);
}
Dictionary RmVideoSession::capabilities() const {
    Dictionary result; Array backends;
    result["schema"] = 1;
    result["scope"] = "enumeration_only; profile, readback and GPU transfer require actual stream/renderer validation";
    for(const auto &backend:rm::enumerate_backends()) {
        Dictionary entry; Array devices;
        entry["name"]=String(backend.name.c_str());entry["compiled"]=backend.compiled;
        entry["error"]=String(backend.error.c_str());
        entry["gpu_transfer_candidate"]=backend.name=="cuda";
        entry["identity_scope"]=backend.name=="software"?"none":backend.name=="d3d11va"?"current_system_boot":backend.name=="vaapi"?"PCI topology or render node":"CUDA UUID";
        for(const auto &device:backend.devices) {
            Dictionary value;
            value["ordinal"]=device.ordinal;value["selector"]=String(device.selector.c_str());
            value["name"]=String(device.name.c_str());value["identity"]=String(device.identity.c_str());
            value["node"]=String(device.node.c_str());value["pci"]=String(device.pci.c_str());
            value["error"]=String(device.error.c_str());devices.push_back(value);
        }
        entry["devices"]=devices;backends.push_back(entry);
    }
    result["backends"]=backends;return result;
}
bool RmVideoSession::start(const Dictionary &values) {
    try {
        if(gpu_holders_.load())throw std::runtime_error("Previous GPU resources have not retired; clear the surface and wait before restarting");
        {std::lock_guard<std::mutex> lock(gpu_mutex_);retired_gpu_.clear();retirement_error_.clear();}
        integer(values, "version", 1, 1, 1);
        rm::TransportConfig config;
        config.diagnostics = boolean(values, "diagnostics", false);
        config.bind_address = string(values, "bind_address", "0.0.0.0");
        config.port = uint16_t(integer(values, "port", 3334, 0, 65535));
        auto source = string(values, "source_address", "");
        if (!source.empty()) {
            auto address = rm::parse_ipv4(source);
            if (!address) throw std::invalid_argument("source_address must be numeric IPv4");
            config.source_address = *address;
        }
        config.source_port = uint16_t(integer(values, "source_port", 0, 0, 65535));
        config.max_frame_bytes = integer(values, "max_frame_bytes", config.max_frame_bytes, 1, 8*1024*1024);
        config.max_pending_frames = integer(values, "max_pending_frames", config.max_pending_frames, 1, 64);
        config.max_pending_bytes = integer(values, "max_pending_bytes", config.max_pending_bytes, 1, 256*1024*1024);
        config.max_queue_frames = integer(values, "max_queue_frames", config.max_queue_frames, 1, 64);
        config.max_queue_bytes = integer(values, "max_queue_bytes", config.max_queue_bytes, 1, 256*1024*1024);
        config.deadline_ms = integer(values, "deadline_ms", config.deadline_ms, 1, 1000);
        config.startup_reorder_ms = integer(values, "startup_reorder_ms", config.startup_reorder_ms, 0, 999);
        config.restart_idle_ms = integer(values, "restart_idle_ms", config.restart_idle_ms, 2, 60000);
        config.sequence_window = uint16_t(integer(values, "sequence_window", config.sequence_window, 1, 1024));
        rm::MediaConfig media;
        media.diagnostics = config.diagnostics;
        media.decoder = string(values, "decoder", "auto"); media.device = string(values, "device", "0");
        media.transfer = string(values, "transfer", "yuv");
        if(media.transfer=="gpu"&&RmCudaGlBridge::unsafe_retirement.load())
            throw std::runtime_error("Previous GPU retirement failed; restart the application before allocating more GPU bridge resources");
        media.max_pixels = int(integer(values, "max_pixels", media.max_pixels, 256, 4096*2304));
        media.allow_10bit_to_8bit = boolean(values, "allow_10bit_to_8bit", false);
        config_error_.clear();
        return receiver_.start(config, media);
    } catch (const std::exception &error) { config_error_ = error.what(); return false; }
}
void RmVideoSession::stop() { receiver_.stop(); }
void RmVideoSession::request_stop() { receiver_.request_stop(); }
void RmVideoSession::poll_stop() { receiver_.poll_stop(); }
Dictionary RmVideoSession::snapshot(bool detailed) const {
    auto state = receiver_.snapshot(); const auto &s = state.receiver.transport;
    Dictionary result;
    auto sample_us=state.snapshot_us;
    result["snapshot_us"]=sample_us;result["diagnostics_enabled"]=state.diagnostics;result["timing_schema"]=1;
    result["accounting_schema"]=2;
    if(detailed) {
        result["last_decoded"]=rm::binding::frame(state.last_decoded);
        result["last_prepared"]=rm::binding::frame(state.last_prepared);
        result["last_submitted"]=rm::binding::frame(state.last_submitted);
        result["last_main_accepted"]=rm::binding::frame(state.last_shown);
        result["submission_windows"]=rm::binding::windows(state.submission_windows,sample_us);
    }
    int64_t pending_uploads=int64_t(state.upload_requested)-int64_t(state.submitted_total+state.upload_stale_before+
        state.upload_stale_after+state.gpu_slot_drops+state.upload_failures);
    result["pending_uploads"]=pending_uploads;
    result["preparing"]=state.preparing;result["prepared_pending"]=state.prepared_pending;
    result["decoded_pending"]=state.decoded_pending;
    result["pending_publications"]=int64_t(state.submitted_total)-int64_t(state.shown_total+state.publication_replaced+state.publication_stale);
    unsigned gpu_pending=gpu_holders_.load();
    result["render_pending"]=int64_t(gpu_pending);
    std::string retirement_error;
    {std::lock_guard<std::mutex> lock(gpu_mutex_);result["gpu_retirement"]=retired_gpu_.duplicate();retirement_error=retirement_error_;}
    bool retirement_failed = !retirement_error.empty();
    result["decoder_done"] = state.worker_done;
    result["render_retired"] = gpu_pending == 0 && !retirement_failed;
    result["render_retirement_failed"] = retirement_failed;
    result["render_retirement_error"] = String(retirement_error.c_str());
    result["stop_terminal"] = state.worker_done && pending_uploads == 0 && (gpu_pending == 0 || retirement_failed);
    result["running"] = !state.worker_done || gpu_pending>0 || pending_uploads>0;
    result["state"] = String((retirement_failed?"Faulted":state.worker_done&&gpu_pending&&state.state!="Faulted"?"Stopping":state.state.c_str()));
    // Compatibility: worker_done still means all ownership retired. Callers
    // needing a failure terminal must use decoder_done/stop_terminal instead.
    result["worker_done"] = state.worker_done && !gpu_pending;
    result["bound_port"] = state.receiver.bound_port; result["socket_buffer_bytes"] = state.receiver.socket_buffer_bytes;
    result["error"] = String((retirement_failed ? retirement_error : config_error_.empty() ? state.error : config_error_).c_str());
    result["source"] = s.source_locked ? String(s.source.text().c_str()) : String();
    result["mode"] = String(state.path.c_str()); result["decoder"] = String(state.backend.c_str());
    result["decode_device"] = String(state.device.c_str()); result["fallback"] = String(state.fallback.c_str());
    if (!state.presentation_fallback.empty()) result["fallback"] = String((state.fallback +
        (state.fallback.empty() ? "" : "; ") + state.presentation_fallback).c_str());
    result["requested_decoder"] = String(state.requested_decoder.c_str());
    result["requested_transfer"] = String(state.requested_transfer.c_str());
    result["gpu_attempts"] = int64_t(state.gpu_attempts);
    result["presentation_fallbacks"] = int64_t(state.presentation_fallbacks);
    result["gpu_disabled"] = state.gpu_disabled;
    result["accepted_presentation"] = int64_t(state.accepted_presentation);
    result["recovery_reason"] = String(state.recovery_reason.c_str());
    result["recoveries"] = int64_t(state.recoveries); result["decode_errors"] = int64_t(state.decode_errors);
    result["format_changes"] = int64_t(state.format_changes);
    result["accepted_stream"] = int64_t(state.accepted_stream); result["accepted_resource"] = int64_t(state.accepted_resource);
    result["ffmpeg"] = av_version_info(); result["avcodec_abi"] = int64_t(avcodec_version());
    result["last_frame_ms"] = state.last_frame_ms; result["started_ms"] = state.started_ms;
    result["frame_age_ms"] = state.last_frame_ms ? sample_us/1000-state.last_frame_ms : int64_t(-1);
    result["prepared"] = int64_t(state.prepared); result["mailbox_replaced"] = int64_t(state.mailbox_replaced);
    result["width"] = state.format.width; result["height"] = state.format.height;
    result["bit_depth"] = state.format.bit_depth; result["hardware"] = state.format.hardware;
    result["color_defaulted"] = state.format.color_defaulted; result["precision_reduced"] = state.format.precision_reduced;
    result["matrix"] = int(state.format.matrix); result["range"] = int(state.format.range);
    result["pixel_format"] = String(state.format.pixel_format.c_str());
    result["resource_generation"] = int64_t(state.format.resource_generation);
#define METRIC(field) result[#field] = int64_t(s.field)
    METRIC(packets); METRIC(bytes); METRIC(invalid); METRIC(oversized);
    METRIC(foreign); METRIC(duplicate); METRIC(conflicts); METRIC(late); METRIC(out_of_window);
    METRIC(complete); METRIC(ordered); METRIC(consumed); METRIC(timeouts); METRIC(missing);
    METRIC(budget_rejected); METRIC(queue_dropped); METRIC(queue_expired); METRIC(restarts); METRIC(generation);
    METRIC(pending_frames); METRIC(pending_bytes); METRIC(queue_frames); METRIC(queue_bytes);
    METRIC(peak_pending_bytes); METRIC(peak_pending_frames); METRIC(peak_queue_bytes); METRIC(last_packet_ms);
    METRIC(peak_queue_frames); METRIC(pending_oldest_ms); METRIC(queue_oldest_ms);
    METRIC(queue_overflow_dropped); METRIC(queue_restart_dropped); METRIC(queue_oversize_dropped);
#undef METRIC
#define METRIC(field) result[#field] = int64_t(state.decode.field)
    METRIC(parser_packets); METRIC(decoded); METRIC(hardware_frames); METRIC(cpu_readbacks);
    METRIC(cpu_readback_bytes); METRIC(converted_rgba); METRIC(skipped_until_key); METRIC(skipped_rasl); METRIC(resets);
    METRIC(output_buffers_allocated); METRIC(output_buffer_growths); METRIC(output_capacity_peak_bytes);
    METRIC(scratch_buffer_growths); METRIC(scratch_capacity_peak_bytes); METRIC(readback_allocations); METRIC(readback_capacity_peak_bytes);
    METRIC(preparation_pool_misses); METRIC(plane_copy_bytes); METRIC(rgba_crop_copy_bytes);
    METRIC(backend_attempts); METRIC(backend_failures); METRIC(backend_switches);
    METRIC(correlated_frames); METRIC(unknown_timing_frames); METRIC(timing_history_evicted); METRIC(timing_history_peak);
#undef METRIC
#define METRIC(field) result[#field] = int64_t(state.field)
    METRIC(prepared_completed); METRIC(prepare_failures); METRIC(decoded_discarded_stop); METRIC(prepared_discarded_stop);
    METRIC(decoded_replaced); METRIC(decoded_discarded_generation); METRIC(decoded_discarded_failure); METRIC(cpu_sources_peak);
    METRIC(decoded_capacity_drops);
    METRIC(prepared_discarded_generation); METRIC(prepared_discarded_failure); METRIC(prepared_discarded_race);
    METRIC(upload_requested); METRIC(submitted_total); METRIC(shown_total); METRIC(upload_busy_polls);
    METRIC(upload_stale_before); METRIC(upload_stale_after); METRIC(gpu_slot_drops); METRIC(upload_failures);
    METRIC(publication_replaced); METRIC(publication_stale); METRIC(shown_invalidated);
    METRIC(correlated_submitted); METRIC(unknown_submitted);
#undef METRIC
    if (detailed && state.diagnostics) {
        Dictionary timings;
#define TIMING(field, owner) timings[#field]=rm::binding::histogram(owner.field)
        TIMING(assembly_time,s); TIMING(order_wait,s); TIMING(compressed_queue_wait,s);
        TIMING(parser_cpu,state.decode); TIMING(packet_prepare_cpu,state.decode);
        TIMING(decode_send_cpu,state.decode); TIMING(decode_receive_cpu,state.decode);
        TIMING(dequeue_to_packet,state.decode); TIMING(packet_to_decoded,state.decode);
        TIMING(cpu_readback,state.decode); TIMING(rgba_conversion,state.decode); TIMING(plane_pack,state.decode); TIMING(prepare_total,state.decode);
        TIMING(preparation_queue_wait,state);
        TIMING(prepared_wait,state); TIMING(render_dispatch_wait,state); TIMING(upload_cpu,state); TIMING(submission_commit_wait,state);
        TIMING(ingress_to_submit,state); TIMING(submit_to_main_accept,state);
#undef TIMING
        result["timings"]=timings;
    }
    return result;
}
static void initialize(ModuleInitializationLevel level) {
    if (level == MODULE_INITIALIZATION_LEVEL_SCENE) {
        ClassDB::register_class<RmVideoSession>();
        ClassDB::register_class<RmCpuUpload>();
        ClassDB::register_class<RmGlTexture>();
        ClassDB::register_class<RmVideoSurface>();
    }
}
static void uninitialize(ModuleInitializationLevel) {}
extern "C" GDExtensionBool GDE_EXPORT rm_video_library_init(GDExtensionInterfaceGetProcAddress get_proc_address,
    GDExtensionClassLibraryPtr library, GDExtensionInitialization *initialization) {
    GDExtensionBinding::InitObject init(get_proc_address, library, initialization);
    init.register_initializer(initialize); init.register_terminator(uninitialize);
    init.set_minimum_library_initialization_level(MODULE_INITIALIZATION_LEVEL_SCENE); return init.init();
}
