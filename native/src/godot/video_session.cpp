#include "transport/udp_receiver.hpp"
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/godot.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <stdexcept>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
}
using namespace godot;

class RmVideoSession : public RefCounted {
    GDCLASS(RmVideoSession, RefCounted)
    rm::UdpReceiver receiver_;
    std::string config_error_;
    static int64_t integer(const Dictionary &values, const char *key, int64_t fallback, int64_t minimum, int64_t maximum) {
        Variant value = values.get(key, fallback);
        if (value.get_type() != Variant::INT) throw std::invalid_argument(std::string(key) + " must be an integer");
        int64_t number = value;
        if (number < minimum || number > maximum) throw std::invalid_argument(std::string(key) + " is outside its valid range");
        return number;
    }
    static std::string string(const Dictionary &values, const char *key, const char *fallback) {
        Variant value = values.get(key, fallback);
        if (value.get_type() != Variant::STRING) throw std::invalid_argument(std::string(key) + " must be a string");
        return String(value).utf8().get_data();
    }
protected:
    static void _bind_methods() {
        ClassDB::bind_method(D_METHOD("start", "config"), &RmVideoSession::start);
        ClassDB::bind_method(D_METHOD("stop"), &RmVideoSession::stop);
        ClassDB::bind_method(D_METHOD("snapshot"), &RmVideoSession::snapshot);
    }
public:
    bool start(const Dictionary &values) {
        try {
            integer(values, "version", 1, 1, 1);
            rm::TransportConfig config;
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
            config_error_.clear();
            return receiver_.start(config);
        } catch (const std::exception &error) {
            config_error_ = error.what();
            return false;
        }
    }
    void stop() { receiver_.stop(); }
    Dictionary snapshot() const {
        auto state = receiver_.snapshot();
        const auto &s = state.transport;
        Dictionary result;
        result["running"] = state.running;
        result["bound_port"] = state.bound_port;
        result["socket_buffer_bytes"] = state.socket_buffer_bytes;
        result["error"] = String((config_error_.empty() ? state.error : config_error_).c_str());
        result["source"] = s.source_locked ? String(s.source.text().c_str()) : String();
        result["mode"] = "protocol_only";
        result["ffmpeg"] = av_version_info();
        result["avcodec_abi"] = int64_t(avcodec_version());
#define METRIC(field) result[#field] = int64_t(s.field)
        METRIC(packets); METRIC(bytes); METRIC(invalid); METRIC(oversized);
        METRIC(foreign); METRIC(duplicate); METRIC(conflicts); METRIC(late); METRIC(out_of_window);
        METRIC(complete); METRIC(ordered); METRIC(consumed); METRIC(timeouts); METRIC(missing);
        METRIC(budget_rejected); METRIC(queue_dropped); METRIC(queue_expired); METRIC(restarts); METRIC(generation);
        METRIC(pending_frames); METRIC(pending_bytes); METRIC(queue_frames); METRIC(queue_bytes);
        METRIC(peak_pending_bytes); METRIC(peak_pending_frames); METRIC(peak_queue_bytes); METRIC(last_packet_ms);
#undef METRIC
        return result;
    }
};

static void initialize(ModuleInitializationLevel level) {
    if (level == MODULE_INITIALIZATION_LEVEL_SCENE) ClassDB::register_class<RmVideoSession>();
}
static void uninitialize(ModuleInitializationLevel) {}
extern "C" GDExtensionBool GDE_EXPORT rm_video_library_init(
    GDExtensionInterfaceGetProcAddress get_proc_address, GDExtensionClassLibraryPtr library,
    GDExtensionInitialization *initialization) {
    GDExtensionBinding::InitObject init(get_proc_address, library, initialization);
    init.register_initializer(initialize); init.register_terminator(uninitialize);
    init.set_minimum_library_initialization_level(MODULE_INITIALIZATION_LEVEL_SCENE);
    return init.init();
}
