#include "hevc_config.hpp"
#include <algorithm>
#include <limits>

namespace rm {
namespace {
[[noreturn]] void invalid(const char *message) { throw MediaError(MediaFailure::Input, message); }
struct Nal { size_t start, payload, end; int type; };
std::vector<Nal> split(const uint8_t *data, size_t size) {
    std::vector<Nal> result;
    for (size_t i = 0; i + 4 < size;) {
        size_t prefix = 0;
        if (!data[i] && !data[i+1]) {
            if (data[i+2] == 1) prefix = 3;
            else if (!data[i+2] && data[i+3] == 1) prefix = 4;
        }
        if (!prefix) { ++i; continue; }
        if (i+prefix+2 > size || (data[i+prefix] & 128) || !(data[i+prefix+1] & 7)) invalid("Invalid HEVC NAL header");
        if (!result.empty()) result.back().end = i;
        result.push_back({i, i+prefix+2, size, int((data[i+prefix] >> 1) & 63)});
        if (result.size() > 1024) invalid("HEVC NAL count exceeds access-unit budget");
        i += prefix+2;
    }
    return result;
}
class Bits {
    std::vector<uint8_t> bytes_;
    size_t offset_ = 0;
public:
    Bits(const uint8_t *data, size_t size) {
        // Only header fields are needed; never copy an entire coded picture.
        size = std::min(size, size_t(256));
        int zeros = 0;
        for (size_t i = 0; i < size; ++i) {
            if (zeros >= 2 && data[i] == 3) { zeros = 0; continue; }
            bytes_.push_back(data[i]); zeros = data[i] == 0 ? zeros+1 : 0;
        }
    }
    uint32_t read(unsigned count) {
        if (count > 32 || offset_+count > bytes_.size()*8) invalid("Truncated HEVC parameter/slice header");
        uint32_t value = 0;
        while (count--) { value = (value<<1) | ((bytes_[offset_/8] >> (7-offset_%8)) & 1); ++offset_; }
        return value;
    }
    void skip(unsigned count) { while (count > 32) { read(32); count -= 32; } read(count); }
    uint32_t ue() {
        unsigned leading = 0;
        while (!read(1)) if (++leading > 30) invalid("HEVC Exp-Golomb field exceeds bound");
        return ((1U<<leading)-1) + read(leading);
    }
};
}
void HevcConfiguration::clear() { for (auto &x:vps_) x.reset(); for(auto &x:sps_) x.reset(); for(auto &x:pps_) x.reset(); bytes_=0; ++revision_; }
void HevcConfiguration::put(std::optional<Parameter> &slot, const uint8_t *data, size_t size, int parent) {
    size_t previous = slot ? slot->bytes.size() : 0;
    if (size > 65536 || bytes_-previous+size > 256*1024) invalid("HEVC parameter cache exceeds budget");
    Parameter value; value.parent = parent; value.bytes.assign(data, data+size);
    if (slot && slot->parent == parent && slot->bytes == value.bytes) return;
    bytes_ = bytes_-previous+size; slot = std::move(value); ++revision_;
}
HevcPacketInfo HevcConfiguration::observe(const uint8_t *data, size_t size) {
    HevcPacketInfo info;
    for (const auto &nal : split(data,size)) {
        if (nal.type > 34) continue;
        Bits bits(data+nal.payload, nal.end-nal.payload);
        if (nal.type == 32) {
            int id = bits.read(4); put(vps_[id],data+nal.start,nal.end-nal.start,-1);
        } else if (nal.type == 33) {
            int parent = bits.read(4), sublayers = bits.read(3); bits.read(1);
            if (sublayers > 6) invalid("Invalid HEVC SPS sublayer count");
            bits.skip(96); // general_profile_tier_level and general_level_idc.
            bool profile[7]{}, level[7]{};
            for(int i=0;i<sublayers;++i){profile[i]=bits.read(1);level[i]=bits.read(1);}
            if (sublayers) for(int i=sublayers;i<8;++i) bits.read(2);
            for(int i=0;i<sublayers;++i){if(profile[i])bits.skip(88);if(level[i])bits.skip(8);}
            auto id = bits.ue(); if (id >= sps_.size()) invalid("HEVC SPS id outside supported range");
            put(sps_[id],data+nal.start,nal.end-nal.start,parent);
        } else if (nal.type == 34) {
            auto id=bits.ue(),parent=bits.ue();
            if(id>=pps_.size()||parent>=sps_.size())invalid("HEVC PPS reference outside supported range");
            put(pps_[id],data+nal.start,nal.end-nal.start,int(parent));
        } else if (nal.type <= 31) {
            info.has_vcl = true; info.rasl |= nal.type == 8 || nal.type == 9;
            info.leading |= nal.type >= 6 && nal.type <= 9;
            bool first = bits.read(1);
            if (nal.type >= 16 && nal.type <= 23) {
                bits.read(1);
                if (first) {
                    auto pps = bits.ue(); if (pps >= pps_.size()) invalid("HEVC slice PPS id outside range");
                    info.random_access = nal.type; info.pps_id = int(pps);
                }
            }
        }
    }
    return info;
}
std::vector<uint8_t> HevcConfiguration::for_picture(int pps_id) const {
    if (pps_id < 0 || pps_id >= int(pps_.size()) || !pps_[pps_id]) return {};
    const auto &pps = *pps_[pps_id];
    if (!sps_[pps.parent]) return {};
    const auto &sps = *sps_[pps.parent];
    if (!vps_[sps.parent]) return {};
    std::vector<uint8_t> result;
    for (const auto *set : {&*vps_[sps.parent], &sps, &pps}) result.insert(result.end(),set->bytes.begin(),set->bytes.end());
    return result;
}
std::vector<uint8_t> HevcConfiguration::all() const {
    std::vector<uint8_t> result;
    auto append = [&](const auto &sets) { for (const auto &set : sets) if (set) result.insert(result.end(), set->bytes.begin(), set->bytes.end()); };
    append(vps_); append(sps_); append(pps_); return result;
}
}
