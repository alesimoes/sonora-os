# RT tuning for USB audio on the installed system

Status: configuration shipped, tested against fakes. Effect on xruns is
**unmeasured**; no hardware has booted this yet.

What was already there: `rtprio 95` / `memlock unlimited` (limits), `threadirqs`
on the kernel command line, `vm.swappiness`/dirty ratios, PipeWire pinned to one
quantum and 48 kHz (data thread `rt.prio 88`). Nothing set IRQ-thread priorities,
the governor, the C-state limit or USB power management, and no earlier
measurement in `brain/research/` covers them.

## What was added and why

| Piece | File | Reason | Source |
|---|---|---|---|
| IRQ thread priorities | `sonora-rtirq` + `sonora-rtirq.service` | With `threadirqs` every handler is a thread `irq/<n>-<name>` at SCHED_FIFO 50. UAC2 isochronous completions run in the xHCI IRQ, so it is raised to 85: above the default 50 of every other IRQ thread, below PipeWire's data thread (88) so the graph still preempts the interrupt that feeds it. All `xhci_hcd` vectors are raised: mapping the UMC1820 to its controller is possible through sysfs but a wrong guess leaves the interface on a priority-50 thread, and an extra raised controller costs nothing. Override with `SONORA_RTIRQ_PRIO` / `SONORA_RTIRQ_NAMES`. | Linux `Documentation/admin-guide/kernel-parameters.txt` (`threadirqs`); rtirq (rncbc.org) does the same by name; Linux Audio wiki "Realtime Specific Configuration". |
| C-state limit, governor | `sonora-cpu-latency` + service | PM QoS: holding `/dev/cpu_dma_latency` open with a u32 of microseconds caps idle exit latency while the fd lives. Default 10 us (C1), set in `/etc/default/sonora-rt` as `SONORA_CPU_LATENCY_US`. Governor `performance` on every cpufreq CPU. Deep C-state wakeups are hundreds of us; a 64-frame quantum is 1333 us. The service is `PartOf` the session, so stopping the engine releases the limit. | Linux `Documentation/power/pm_qos_interface.rst`; `Documentation/admin-guide/pm/cpuidle.rst`. |
| USB autosuspend off | `90-sonora-usb-audio.rules` | Writes `on` to `power/control` of the parent of any `bInterfaceClass 01` interface. No vendor id assumed. | Linux `Documentation/driver-api/usb/power-management.rst`. |

All three are `Wants=` of `sonora-session.service`, never `Requires=`: a failed
tuning unit must not keep the engine down. Each script exits 0 and logs when
the hardware lacks the feature (no xHCI, no cpufreq, no `cpu_dma_latency`).

## Optional boot options (not default)

`isolcpus=`, `nohz_full=`, `rcu_nocbs=` pin kernel noise away from chosen cores
and need a machine that has spare cores; on a 2 to 4 core laptop they starve
the UI. Document only. Example for an 8-thread CPU, keeping 0 to 3 for the
system and 4 to 7 for the engine and PipeWire:

    isolcpus=domain,managed_irq,4-7 nohz_full=4-7 rcu_nocbs=4-7

then run PipeWire and the engine with `taskset`/`CPUAffinity=4-7` in a drop-in.
Add them to the GRUB entry by hand, measure, and keep them only if xruns drop.
Source: Linux `Documentation/admin-guide/kernel-parameters.txt`.

## Tests (fakes only)

`tools/test-rt-tuning.sh`: fake `/proc/interrupts` plus `/proc/<pid>/comm` and
a stubbed `chrt` (asserts exactly xhci threads get `-f -p 85`, not the NIC, HDA
or `irq/1280-`); fake sysfs and a regular file for `cpu_dma_latency` (asserts
the u32); `udevadm verify` and `systemd-analyze verify` in `debian:trixie`.

## Hardware measurement, for the owner (unverified)

On the UMC1820 box, installed image, as `sonora`:

    sudo sonora-acceptance quick                      # governor, RT, PipeWire pin, short take; reports xruns
    sudo sonora-acceptance soak 1                     # one hour of 18-channel recording

Compare against the same commands with the tuning off:

    sudo systemctl stop sonora-cpu-latency; sudo systemctl disable --now sonora-rtirq
    # (set the IRQ threads back: chrt -f -p 50 $(pgrep -f 'irq/.*xhci'))

Check the result with `journalctl -u sonora-rtirq -u sonora-cpu-latency`,
`chrt -p $(pgrep -f 'irq/.*xhci')` (expect 85) and `cat /sys/bus/usb/devices/*/power/control`
for the interface. Record the machine, quantum (64 and 128, edit
`audio-stack/pipewire-sonora.conf`) and the xrun count here.

| Machine | Quantum | Tuning | xruns / hour |
|---|---|---|---|
| (not measured) | | | |
