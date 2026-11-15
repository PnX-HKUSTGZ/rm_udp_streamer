#pragma once
#include "media/frame.hpp"
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <cstring>
#include <stdexcept>

// Render-thread owned. PackedByteArray's writable accessor detaches when an
// Image or the renderer still references the previous pixels. Reuse therefore
// cannot overwrite an outstanding consumer; that case gets fresh storage.
class RmCpuUploadBuffers {
    std::vector<godot::PackedByteArray> planes_;
public:
    uint64_t storage_changes = 0, copied_bytes = 0, images_created = 0;
    void clear() { planes_.clear(); }
    uint64_t cached_bytes() const {
        uint64_t total = 0;
        for (const auto &plane : planes_) total += uint64_t(plane.size());
        return total;
    }
    godot::Ref<godot::Image> copy(size_t index, const rm::CpuPlane &plane) {
        if (index >= 3 || plane.width <= 0 || plane.height <= 0 ||
            (plane.channels != 1 && plane.channels != 2 && plane.channels != 4) ||
            plane.pixels.size() % plane.channels != 0 ||
            uint64_t(plane.width)*uint64_t(plane.height) != plane.pixels.size()/plane.channels)
            throw std::runtime_error("Invalid CPU upload plane layout");
        if (planes_.size() <= index) planes_.resize(index+1);
        auto &packed = planes_[index];
        const auto *previous = packed.ptr();
        if (packed.size() != int64_t(plane.pixels.size()) && packed.resize(int64_t(plane.pixels.size())) != 0)
            throw std::runtime_error("Cannot allocate CPU upload storage");
        auto *destination = packed.ptrw();
        if (!destination) throw std::runtime_error("Cannot obtain writable CPU upload storage");
        if (destination != previous) ++storage_changes;
        std::memcpy(destination, plane.pixels.data(), plane.pixels.size());
        copied_bytes += plane.pixels.size();
        auto format = plane.channels == 4 ? godot::Image::FORMAT_RGBA8 : plane.channels == 2 ? godot::Image::FORMAT_RG8 : godot::Image::FORMAT_R8;
        auto image = godot::Image::create_from_data(plane.width, plane.height, false, format, packed);
        if (image.is_null() || image->is_empty()) throw std::runtime_error("Cannot allocate CPU upload image");
        ++images_created;
        return image;
    }
};
