#include "video_surface.hpp"
#include "video_session.hpp"
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/shader.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/callable_method_pointer.hpp>
#include <cstring>
#include <stdexcept>
#include <utility>
using namespace godot;

namespace {
const char *yuv_shader = R"(
shader_type canvas_item;
render_mode unshaded;
uniform sampler2D plane_u : filter_linear, repeat_disable;
uniform sampler2D plane_v : filter_linear, repeat_disable;
uniform bool nv12 = false;
uniform vec4 levels = vec4(0.062745098, 1.16438356, 0.501960784, 1.13839286);
uniform vec4 coefficients = vec4(1.5748, -0.187324, -0.468124, 1.8556);
uniform vec2 chroma_offset = vec2(0.0);
void fragment() {
    float y = (texture(TEXTURE, UV).r - levels.x) * levels.y;
    vec2 uv_coord = UV + chroma_offset;
    vec2 pair = texture(plane_u, uv_coord).rg;
    float u = (pair.r - levels.z) * levels.w;
    float v = ((nv12 ? pair.g : texture(plane_v, uv_coord).r) - levels.z) * levels.w;
    COLOR = vec4(clamp(vec3(y + coefficients.x*v,
        y + coefficients.y*u + coefficients.z*v, y + coefficients.w*u), vec3(0.0), vec3(1.0)), 1.0);
}
)";
}
void RmCpuUpload::run(const Ref<RmCpuUpload> &keep_alive) {
    (void)keep_alive;
    try {
        if (state->active.load() && session.is_valid() && session->core().accepts(frame->info)) {
            auto publication = std::make_shared<RmUploadState::Publication>();
            publication->info = frame->info; publication->path = frame->path;
            auto layout = frame->path + ":" + std::to_string(frame->info.width) + ":" + std::to_string(frame->info.height);
            bool recreate = layout != state->layout;
            if (recreate) { state->textures.clear(); state->layout = layout; }
            for (size_t i = 0; i < frame->planes.size(); ++i) {
                const auto &plane = frame->planes[i];
                PackedByteArray packed; packed.resize(static_cast<int64_t>(plane.pixels.size()));
                std::memcpy(packed.ptrw(), plane.pixels.data(), plane.pixels.size());
                auto format = plane.channels == 4 ? Image::FORMAT_RGBA8 : plane.channels == 2 ? Image::FORMAT_RG8 : Image::FORMAT_R8;
                Ref<Image> image = Image::create_from_data(plane.width, plane.height, false, format, packed);
                if (image.is_null() || image->is_empty()) throw std::runtime_error("Cannot allocate CPU upload image");
                if (recreate) state->textures.push_back(ImageTexture::create_from_image(image));
                else state->textures[i]->update(image);
                if (state->textures[i].is_null()) throw std::runtime_error("Cannot allocate video texture");
            }
            publication->textures = state->textures;
            std::lock_guard<std::mutex> lock(state->mutex);
            if (state->active.load() && session->core().accepts(frame->info)) { state->published = std::move(publication); ++state->submitted; }
        }
    } catch (const std::exception &error) {
        std::lock_guard<std::mutex> lock(state->mutex); state->error = error.what();
    }
    frame.reset(); state->busy.store(false);
}
void RmVideoSurface::_bind_methods() {
    ClassDB::bind_method(D_METHOD("set_session", "session"), &RmVideoSurface::set_session);
    ClassDB::bind_method(D_METHOD("clear"), &RmVideoSurface::clear);
    ClassDB::bind_method(D_METHOD("get_metrics"), &RmVideoSurface::get_metrics);
}
RmVideoSurface::RmVideoSurface() {
    set_process(true); set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
    set_texture_filter(CanvasItem::TEXTURE_FILTER_LINEAR);
    upload_ = std::make_shared<RmUploadState>();
    Ref<Shader> shader; shader.instantiate(); shader->set_code(yuv_shader);
    material_.instantiate(); material_->set_shader(shader);
}
RmVideoSurface::~RmVideoSurface() { upload_->active.store(false); }
void RmVideoSurface::set_session(const Ref<RmVideoSession> &session) { clear(); session_ = session; }
void RmVideoSurface::clear() {
    upload_->active.store(false);
    upload_ = std::make_shared<RmUploadState>(); shown_.reset(); set_material(Ref<Material>()); queue_redraw();
    material_->set_shader_parameter("plane_u", Variant()); material_->set_shader_parameter("plane_v", Variant());
}
Dictionary RmVideoSurface::get_metrics() const {
    Dictionary result;
    result["submitted"] = int64_t(upload_->submitted.load());
    result["upload_pending"] = upload_->busy.load();
    { std::lock_guard<std::mutex> lock(upload_->mutex); result["error"] = String(upload_->error.c_str()); }
    if (shown_) {
        result["shown_sequence"] = int64_t(shown_->info.output_sequence);
        result["shown_generation"] = int64_t(shown_->info.stream_generation);
        result["shown_resource"] = int64_t(shown_->info.resource_generation);
        result["width"] = shown_->info.width; result["height"] = shown_->info.height;
        result["path"] = String(shown_->path.c_str());
    }
    return result;
}
void RmVideoSurface::_process(double) {
    if (session_.is_null()) return;
    session_->poll_stop();
    if (shown_ && !session_->core().accepts(shown_->info)) {
        shown_.reset(); set_material(Ref<Material>()); queue_redraw();
    }
    std::shared_ptr<RmUploadState::Publication> publication;
    { std::lock_guard<std::mutex> lock(upload_->mutex); publication = std::exchange(upload_->published, {}); }
    if (publication) {
        if (session_->core().accepts(publication->info)) {
            shown_ = std::move(publication);
            if (shown_->path == "cpu_rgba") set_material(Ref<Material>());
            else {
                auto &info = shown_->info;
                double kr = info.matrix == AVCOL_SPC_BT709 ? .2126 : info.matrix == AVCOL_SPC_BT2020_NCL ? .2627 : .299;
                double kb = info.matrix == AVCOL_SPC_BT709 ? .0722 : info.matrix == AVCOL_SPC_BT2020_NCL ? .0593 : .114;
                double kg = 1-kr-kb;
                bool full = info.range == AVCOL_RANGE_JPEG;
                material_->set_shader_parameter("levels", Vector4(full ? 0 : 16.0/255, full ? 1 : 255.0/219, 128.0/255, full ? 1 : 255.0/224));
                material_->set_shader_parameter("coefficients", Vector4(2*(1-kr), -2*kb*(1-kb)/kg, -2*kr*(1-kr)/kg, 2*(1-kb)));
                material_->set_shader_parameter("nv12", shown_->path == "cpu_nv12");
                material_->set_shader_parameter("plane_u", shown_->textures[1]);
                material_->set_shader_parameter("plane_v", shown_->textures.back());
                bool left = info.chroma == AVCHROMA_LOC_LEFT || info.chroma == AVCHROMA_LOC_TOPLEFT || info.chroma == AVCHROMA_LOC_BOTTOMLEFT || info.chroma == AVCHROMA_LOC_UNSPECIFIED;
                double vertical = info.chroma == AVCHROMA_LOC_TOP || info.chroma == AVCHROMA_LOC_TOPLEFT ? .5 :
                    info.chroma == AVCHROMA_LOC_BOTTOM || info.chroma == AVCHROMA_LOC_BOTTOMLEFT ? -.5 : 0;
                material_->set_shader_parameter("chroma_offset", Vector2(left ? .5/info.width : 0, vertical/info.height));
                set_material(material_);
            }
            queue_redraw();
        }
    }
    if (!upload_->busy.load()) {
        auto frame = session_->core().take_frame();
        if (frame) {
            upload_->busy.store(true);
            Ref<RmCpuUpload> work; work.instantiate(); work->state = upload_; work->frame = std::move(frame);
            work->session = session_;
            RenderingServer::get_singleton()->call_on_render_thread(callable_mp(work.ptr(), &RmCpuUpload::run).bind(work));
        }
    }
}
void RmVideoSurface::_draw() {
    if (!shown_ || shown_->textures.empty()) return;
    const auto &info = shown_->info;
    double ratio = double(info.width) * info.sample_aspect.num / info.sample_aspect.den / info.height;
    auto bounds = get_size();
    double width = std::min(double(bounds.x), double(bounds.y)*ratio);
    Vector2 size(width, width/ratio);
    draw_texture_rect(shown_->textures[0], Rect2((bounds-size)*.5, size), false);
}
