#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <vector>

namespace rm {
enum class MediaFailure { Input, Unsupported, Backend };
class MediaError : public std::runtime_error {
public:
    MediaFailure kind;
    MediaError(MediaFailure type, const std::string &message) : std::runtime_error(message), kind(type) {}
};
struct HevcPacketInfo {
    bool has_vcl = false, rasl = false, leading = false;
    int random_access = -1, pps_id = -1;
};
// Parameter sets are selected by the slice's PPS -> SPS -> VPS references.
// The cache is session-local and has hard count/byte bounds.
class HevcConfiguration {
public:
    HevcPacketInfo observe(const uint8_t *data, size_t size);
    std::vector<uint8_t> for_picture(int pps_id) const;
    std::vector<uint8_t> all() const;
    void clear();
    uint64_t revision() const { return revision_; }
private:
    struct Parameter { std::vector<uint8_t> bytes; int parent = -1; };
    std::array<std::optional<Parameter>, 16> vps_, sps_;
    std::array<std::optional<Parameter>, 64> pps_;
    size_t bytes_ = 0;
    uint64_t revision_ = 0;
    void put(std::optional<Parameter> &, const uint8_t *, size_t, int parent);
};
}
