// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "sonora/audio_backend.h"

namespace sonora {

// An AudioBackend with no audio server behind it (ADR-0007 §4.2, M3). Not a
// mock: it keeps the contract the way drivers/jack does, through the same
// drivers/support pieces (PeriodGuard, FrameClock, describe_ports,
// SourceWatch), and the backend conformance suite holds both to it. What it
// adds is control: the devices it presents, the signal on every capture
// port, and the faults that cost this project defects, on demand.
//
// Platform-neutral (ADR-0008): standard C++17 only, so every engine test
// that needs a backend runs on any OS.
class FakeBackend final : public backend::AudioBackend {
public:
    // Who drives the periods. Synchronous: run_blocks() on the caller's
    // thread, deterministic, with a virtual clock that advances by each
    // period's length. Thread: a thread of the backend's own at the
    // period's real-time rate, as a server would.
    enum class Mode { Synchronous, Thread };

    // What every capture port carries. Tones: channel c is a 0.5-amplitude
    // sine at 250 + 317 c Hz, the frequencies tools/verify-recording.py
    // checks, so a recording made through this backend verifies there too.
    // Impulses: 1.0 on every channel at each multiple of impulse_interval,
    // so tracks that are not sample-aligned show it.
    enum class Signal { Tones, Silence, Impulses };

    // How the capture and playback ports are named: capture_AUX0.. as
    // PipeWire's Pro Audio profile does, or capture_1.. as a card whose
    // channel positions are not set (measured: snd-aloop without
    // audio.position, ADR-0007 first update, item 5).
    enum class Naming { Aux, Numbered };

    // The order the "server" lists ports in. jack_get_ports() promises none;
    // Lexicographic is the order that put AUX10 before AUX2.
    enum class ListOrder { Natural, Lexicographic, Reversed };

    struct Device {
        std::string name;  // PortInfo::device, the client part of each port name
        uint32_t capture = 0;
        uint32_t playback = 0;
        bool usb = false;
        // Absent until the backend's clock reaches this, as a USB interface
        // enumerating after boot. 0: present from the start.
        uint64_t appears_after_ns = 0;
    };

    // The target: a Behringer UMC1820, 18 in / 20 out over USB. The name is
    // the node description pw-jack gives a Pro Audio node, which is what a
    // JACK client sees as the client part of a port name.
    static Device umc1820();
    // A laptop's onboard audio: 2 in / 2 out, not USB.
    static Device onboard();

    struct Config {
        std::vector<Device> devices{umc1820()};
        uint32_t rate = 48000;
        uint32_t quantum = 128;
        Mode mode = Mode::Synchronous;
        Signal signal = Signal::Tones;
        uint32_t impulse_interval = 4800;
        Naming naming = Naming::Aux;
        ListOrder list_order = ListOrder::Natural;
        // Registry ids not in channel order (measured for monitor ports,
        // ADR-0007 first update, item 5). Natural: ids ascend with channels.
        bool ids_out_of_order = false;
        // The server's 32-bit frame counter at the first period, so a test
        // can start just short of its wrap.
        uint32_t first_frame = 0;
        // Frames of each output kept for read_output(); 0 keeps none.
        std::size_t output_capture_frames = 0;
        // An unconnected input is nullptr in the block unless this is set,
        // when it is a buffer of silence: the contract allows both, so a
        // callback has to cope with both.
        bool silence_for_unconnected = false;
    };

    // Two-input onboard shape in one call: Config with devices {onboard()}.
    static Config onboard_config();

    FakeBackend();
    explicit FakeBackend(Config config);
    ~FakeBackend() override;
    FakeBackend(const FakeBackend&) = delete;
    FakeBackend& operator=(const FakeBackend&) = delete;

    // --- AudioBackend ---
    std::vector<backend::PortInfo> physical_capture_ports() override;
    std::vector<backend::PortInfo> physical_playback_ports() override;
    bool open(const backend::BackendConfig& config,
              backend::ProcessCallback& callback, std::string& error) override;
    bool activate(std::string& error) override;
    int connect_inputs_in_order() override;
    int connected_input_count() const override;
    bool input_connected(uint32_t index) const override;
    bool choose_capture_device(const std::string& device) override;
    void deactivate() noexcept override;
    uint32_t sample_rate() const override;
    uint32_t buffer_frames() const override;
    uint32_t xruns() const override;
    uint32_t clock_discontinuities() const override;
    bool fatal() const override;
    std::string fatal_reason() const override;

    // --- driving it (Synchronous mode) ---
    // Runs `n` server periods on the calling thread and returns how many
    // reached process(). Does nothing unless active. Not concurrently with
    // itself; deactivate() from another thread waits for the period in
    // progress.
    uint32_t run_blocks(uint32_t n);
    // Lets time pass with no period, for late enumeration and stalls.
    void advance_time(uint64_t ns);
    // The backend's clock: virtual in Synchronous mode, steady time since
    // construction in Thread mode.
    uint64_t now_ns() const;

    // --- patching by hand, as an operator in a patchbay ---
    // Connects capture port `port` (its full name) to input `index`,
    // replacing what fed it. False if either does not exist.
    bool patch(uint32_t index, const std::string& port);
    void unpatch(uint32_t index);

    // --- faults. Each takes effect at the next period, from any one
    // control thread, in either mode. ---
    // The server loses `frames`: one xrun reported, and frame_time jumps.
    void inject_xrun(uint32_t frames);
    // The server dies: no period runs again and the ports are gone.
    void inject_server_loss();
    // The server runs its next period at `rate`. With `notify` the server
    // also says so first, as JACK's sample-rate callback does; without it
    // the period itself is all there is, which is the case PeriodGuard
    // checks the rate in every period for.
    void inject_rate_change(uint32_t rate, bool notify = false);
    // The server's next period is `frames` long.
    void inject_quantum_change(uint32_t frames);
    // The server's frame counter steps back by `frames` (a clock source
    // change). Not fatal: counted, and frame_time holds still.
    void inject_clock_step_back(uint32_t frames);
    // The server's frame counter jumps `frames` ahead with no xrun reported,
    // as PipeWire's dummy driver does when woken late. Counted as a clock
    // step; frame_time keeps the gap.
    void inject_unreported_skip(uint32_t frames);
    // While set, no period runs although time passes.
    void set_stalled(bool stalled);
    // The device's ports go away: unlinked first, then unregistered, in the
    // order pw-jack 1.4.2 delivers them. Fatal if one fed an input. False
    // if no such device is present.
    bool unplug(const std::string& device);

    // --- observing it ---
    uint64_t periods() const;        // periods the server ran, refused included
    uint64_t process_calls() const;  // periods that reached process()
    // Periods in which process() left an output unwritten: the contract has
    // the callback write every output, silence included.
    uint64_t unwritten_output_periods() const;
    // Reads up to `n` frames of what process() wrote to output `index`, in
    // order, oldest first. Needs Config::output_capture_frames. Returns how
    // many were read; a full buffer drops the newest (output_dropped()).
    std::size_t read_output(uint32_t index, float* dst, std::size_t n);
    uint64_t output_dropped() const;
    // The registry id of a port by full name, or UINT32_MAX.
    uint32_t port_id(const std::string& name) const;
    // The capture device inputs come from; empty while none is chosen.
    std::string capture_device() const;
    // The last complaint connect_inputs_in_order() would have printed: a
    // channel count that does not match the input count, or why nothing was
    // connected. The fake prints nothing.
    std::string last_warning() const;

    // What Signal::Tones puts on capture channel `channel` at server frame
    // `frame` (the block's frame_time plus the offset in the block).
    static float tone(uint32_t channel, uint64_t frame, uint32_t rate);
    static double tone_hz(uint32_t channel);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace sonora
