#pragma once
#include <string>
#include <vector>
extern "C" {
#include <libavutil/hwcontext.h>
}

namespace rm {
struct DeviceInfo {
    std::string backend, selector, name, identity, node, pci, error;
    int ordinal = -1;
    uint64_t device_number = 0;
};
struct BackendInfo {
    std::string name, error;
    bool compiled = false;
    std::vector<DeviceInfo> devices;
};
// Enumeration proves identity/visibility, not support for an input's profile
// or a renderer's transfer path. Those are checked using actual output frames.
std::vector<BackendInfo> enumerate_backends();
std::string selector_backend(const std::string &selector);
std::string validate_device_selector(const std::string &backend, const std::string &selector);
std::vector<std::string> decoder_candidates(const std::string &backend, const std::string &transfer,
                                           const std::string &selector);
// On success the caller owns one reference. On failure *device stays null.
void open_hardware_device(const std::string &backend, const std::string &selector,
                          AVBufferRef **device, AVPixelFormat &format, std::string &identity);
}
