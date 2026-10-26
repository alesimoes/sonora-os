// SPDX-License-Identifier: GPL-3.0-or-later
//
// Contract file (ADR-0007 §4.2, §6). Owners: rt-engineer and driver-engineer
// jointly. The seam between the engine and whatever moves audio for it. No
// backend's own headers (JACK, ALSA, ...) may appear here or in any header
// that implements it.
#pragma once

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "sonora/process_callback.h"

namespace sonora::backend {

// A physical port, as the server names it. Ports are numbered per device:
// PipeWire's Pro Audio profile applies to every ALSA card, so onboard audio
// and the interface both offer capture ports, and one list across them
// would shift every channel after the first card's.
struct PortInfo {
    std::string device;  // the card, as the server names it
    std::string name;    // the full port name, unique across devices
    // Position within `device` once its ports are sorted with natural_less,
    // so capture_AUX2 is before capture_AUX10: registration order and id
    // order are not channel order.
    uint32_t channel;
    // True only when the backend established that the device is on USB.
    // A backend that cannot tell leaves it false, and the device is then
    // treated as onboard; see select_device().
    bool usb = false;
};

struct BackendConfig {
    std::string client_name;
    uint32_t inputs;   // ports registered as in_1..in_N, in that order
    uint32_t outputs;  // out_1..out_N
    // ADR-0003: the session's rate. A backend that cannot run at this rate
    // fails open(), since recording at another would write wrong-pitch files.
    uint32_t required_rate;
    // The device whose capture ports feed the inputs, by PortInfo::device.
    // Empty means the one USB device; open() fails, naming them, if there
    // are several. With none yet, the caller decides after its startup wait
    // (choose_capture_device()). See select_device().
    std::string capture_device;
};

// Everything here except what the ProcessCallback receives is called from
// one non-real-time control thread, unless a member says otherwise.
class AudioBackend {
public:
    virtual ~AudioBackend() = default;

    // Usable before open(), so the session can be sized to the interface.
    // As presented at the current rate: the count can depend on rate (ADAT
    // S/MUX halves an interface's digital inputs at 96 kHz). Grouped by
    // device, in channel order within each. Empty if the server is
    // unreachable or offers nothing.
    virtual std::vector<PortInfo> physical_capture_ports() = 0;
    virtual std::vector<PortInfo> physical_playback_ports() = 0;

    // Connects to the server and registers the ports. `callback` must outlive
    // the backend's activation. On failure returns false with `error` set,
    // and the backend can be destroyed or opened again.
    virtual bool open(const BackendConfig& config, ProcessCallback& callback,
                      std::string& error) = 0;
    // From here on callback.process() runs on a thread the backend controls
    // (drivers/fake's synchronous run_blocks() uses the caller's), never
    // concurrently with itself.
    virtual bool activate(std::string& error) = 0;

    // Connects capture port k of the chosen capture_device to input k.
    // Idempotent. Returns how many inputs are connected in order after the
    // call; an existing link counts. Zero means every track records silence
    // and fewer than the input count means some do, so callers must not
    // ignore it.
    virtual int connect_inputs_in_order() = 0;
    // How many inputs have something connected right now. Asked at record
    // time, since a port can be unpatched after it was connected.
    virtual int connected_input_count() const = 0;
    // Whether input `index` (in_1 is 0) has anything connected right now,
    // so a take can be refused when no armed track would hear anything.
    virtual bool input_connected(uint32_t index) const = 0;

    // Makes `device` the capture device, as if open() had been given it, if
    // none is chosen yet: open() got no name and no USB interface has been
    // connected from since. Otherwise returns false and changes nothing, so
    // the inputs never move to another device mid-session -- that would
    // change the channels under the tracks. For the default policy's
    // fallback after the startup wait.
    virtual bool choose_capture_device(const std::string& device) = 0;

    // After return, process() will not be called again and no call is still
    // running; safe to tear down. Idempotent. The destructor calls it.
    virtual void deactivate() noexcept = 0;

    // Fixed from open() for the life of the session; see fatal().
    virtual uint32_t sample_rate() const = 0;
    // The size admitted by the first period; 0 before it.
    virtual uint32_t buffer_frames() const = 0;
    // Server-reported xruns since open(). Any thread.
    virtual uint32_t xruns() const = 0;
    // How many times since open() the server's frame clock stepped back, or
    // jumped ahead over periods it ran for no one and reported no xrun for
    // (PipeWire's dummy driver woken late). frame_time holds still across a
    // step back and keeps a jump's gap; either way the server's frames after
    // one do not follow on from those before it. Any thread.
    virtual uint32_t clock_discontinuities() const = 0;

    // Server loss, loss of a physical capture port an input was connected
    // from (the interface unplugged), no period for one second while
    // active, or a rate or buffer size change. A period whose rate or size
    // differs from the first is refused before it reaches process(), so no
    // block at the wrong rate or size reaches the session. No process() call starts after
    // fatal() first returns true; one already running may finish, and
    // deactivate() is the barrier. The engine closes the take cleanly and
    // says why; it never adapts silently. Any thread.
    virtual bool fatal() const = 0;
    // Why, with the specifics ("48000 -> 96000 Hz", the device's name).
    // Empty until fatal() is true. Control thread only, never the audio
    // thread: it allocates.
    virtual std::string fatal_reason() const = 0;

    // The capture device the inputs come from, by PortInfo::device; empty
    // while none is chosen. With the rate and the period it keys the
    // machine's round trip (ADR-0019, update of 2026-10-26).
    virtual std::string capture_device() const { return {}; }
    // The round trip in frames the server reports for the session's ports:
    // its capture latency into the inputs plus its playback latency out of
    // the outputs. 0 when it reports none, or cannot until an input and an
    // output are connected. It leaves out the converters' group delay, so
    // it is only the fallback for a measured L (ADR-0019, Context).
    virtual uint32_t reported_latency() const { return 0; }
};

// The ports of `all` that BackendConfig::capture_device selects, in channel
// order. Default policy, revised by the owner 2026-10-13 (ADR-0007, M2
// note, item 8):
//  - a named device is used whatever its bus;
//  - an empty name means the USB device;
//  - several USB devices and no name: false, with `error` naming them;
//  - no USB device while the startup wait runs (`wait_over` false): nothing
//    yet, since an interface enumerates after boot;
//  - no USB device once it is over: the one non-USB device (onboard, PCI,
//    HDMI), which until the interface is connected is the laptop's own
//    audio; several of those and no name: false, with `error` naming them.
// A device not present selects nothing and is not an error: the caller
// waits, or keeps looking, and says why with describe_absent().
inline bool select_device(const std::vector<PortInfo>& all,
                          const std::string& device,
                          std::vector<PortInfo>& selected, std::string& error,
                          bool wait_over = false) {
    selected.clear();
    std::string wanted = device;
    if (wanted.empty()) {
        std::vector<std::string> usb, other;
        for (const PortInfo& p : all) {
            std::vector<std::string>& bus = p.usb ? usb : other;
            if (std::find(bus.begin(), bus.end(), p.device) == bus.end()) {
                bus.push_back(p.device);
            }
        }
        const auto ambiguous = [&error](const std::vector<std::string>& devices,
                                        const char* what) {
            error = std::to_string(devices.size()) + what +
                    "; name the one to record from with --device:";
            for (const std::string& d : devices) error += " \"" + d + "\"";
            return false;
        };
        if (usb.size() > 1) return ambiguous(usb, " USB audio interfaces");
        if (usb.size() == 1) {
            wanted = usb.front();
        } else if (!wait_over || other.empty()) {
            return true;
        } else if (other.size() > 1) {
            return ambiguous(other, " capture devices and no USB interface");
        } else {
            wanted = other.front();
        }
    }
    for (const PortInfo& p : all) {
        if (p.device == wanted) selected.push_back(p);
    }
    return true;
}

// Why select_device() found nothing, for the operator: the device asked
// for, or the USB interface the default wants, and every device present.
inline std::string describe_absent(const std::vector<PortInfo>& all,
                                   const std::string& device) {
    std::string present;
    std::vector<std::string> seen;
    for (const PortInfo& p : all) {
        if (std::find(seen.begin(), seen.end(), p.device) != seen.end()) continue;
        seen.push_back(p.device);
        present += (present.empty() ? "" : ", ") + p.device + (p.usb ? " (USB)" : "");
    }
    std::string why = device.empty()
                          ? std::string("no USB audio interface is present")
                          : "capture device \"" + device + "\" is not present";
    if (present.empty()) return why + ", and no capture device is";
    why += "; present: " + present;
    if (device.empty()) why += ". Name one with --device to record from it";
    return why;
}

}  // namespace sonora::backend
