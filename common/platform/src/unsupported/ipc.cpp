// SPDX-License-Identifier: GPL-3.0-or-later
// Lets a neutral build link. It is not a port (ADR-0008): every call fails
// and says why. The Windows design (named pipe, read-only section) is in
// ADR-0008 §6.
#include "sonora/platform/ipc.h"

namespace sonora::platform {

namespace {
const char* const kUnsupported =
    "the engine's control socket and shared state are not available on this platform yet"
    " (ADR-0008 §6)";
}

struct SharedRegion::Impl {};
struct ControlChannel::Impl {};
struct ControlListener::Impl {
    std::string endpoint;
};

bool ipc_supported() { return false; }
std::string default_control_endpoint() { return {}; }
bool endpoint_live(const std::string&) { return false; }

std::unique_ptr<SharedRegion> SharedRegion::create(const std::string&, std::size_t,
                                                   std::string& error) {
    error = kUnsupported;
    return nullptr;
}
SharedRegion::SharedRegion(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
SharedRegion::~SharedRegion() = default;
void* SharedRegion::writable_data() { return nullptr; }
const void* SharedRegion::data() const { return nullptr; }
std::size_t SharedRegion::bytes() const { return 0; }
std::unique_ptr<SharedRegion> SharedRegion::create_shared_writable(const std::string&, std::size_t,
                                                                   std::string& error) {
    error = kUnsupported;
    return nullptr;
}
bool SharedRegion::writable() const { return false; }
bool SharedRegion::sealed_against_writes() const { return false; }
bool SharedRegion::lock(std::string& error) {
    error = kUnsupported;
    return false;
}

std::unique_ptr<ControlChannel> ControlChannel::connect(const std::string&, std::string& error) {
    error = kUnsupported;
    return nullptr;
}
ControlChannel::ControlChannel(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
ControlChannel::~ControlChannel() = default;
int ControlChannel::native_handle() const { return -1; }
bool ControlChannel::pair(std::unique_ptr<ControlChannel>&, int& child_handle, std::string& error) {
    child_handle = -1;
    error = kUnsupported;
    return false;
}
std::unique_ptr<ControlChannel> ControlChannel::adopt(int, std::string& error) {
    error = kUnsupported;
    return nullptr;
}
void ControlChannel::close_handle(int) {}
IoStatus ControlChannel::send(std::string_view, int, std::string& error) {
    error = kUnsupported;
    return IoStatus::Error;
}
IoStatus ControlChannel::send_with_region(std::string_view, const SharedRegion&, int,
                                          std::string& error) {
    error = kUnsupported;
    return IoStatus::Error;
}
IoStatus ControlChannel::receive(std::string&, std::size_t, std::unique_ptr<SharedRegion>*, int,
                                 std::string& error) {
    error = kUnsupported;
    return IoStatus::Error;
}
void ControlChannel::shutdown() {}

std::unique_ptr<ControlListener> ControlListener::listen(const std::string&, std::string& error) {
    error = kUnsupported;
    return nullptr;
}
ControlListener::ControlListener(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
ControlListener::~ControlListener() = default;
std::unique_ptr<ControlChannel> ControlListener::accept(int, std::string& error) {
    error = kUnsupported;
    return nullptr;
}
const std::string& ControlListener::endpoint() const { return impl_->endpoint; }

}  // namespace sonora::platform
