// SPDX-License-Identifier: GPL-3.0-or-later
//
// One thread's sleep that ends on whichever comes first: another thread's
// notify(), one of the handles it was given becoming ready, or a timeout.
// The engine's control thread sleeps on it, with each plug-in host's pidfd
// and, while frames wait to leave, its socket (ADR-0012 §4, M3 on #78): a
// host that exits, or a socket that takes the next part of a large Load,
// wakes the thread at once instead of at its next 50 ms turn.
//
// Not for the audio thread. The handles stay the caller's: wait() only
// polls them, on the caller's own thread, so they are never polled after
// their owner closed them. Readiness is level-triggered, so a caller lists
// only handles it services on the turn after a wake (an exited child is
// reaped; a writable socket is written to), or it wakes again at once.
#pragma once

#include <cstddef>
#include <memory>

namespace sonora::platform {

class WaitSet {
public:
    // Readable: input waits, or the handle is an exit handle that fired
    // (ChildProcess::exit_handle()). Writable: a send would not block.
    // Either way an error or a hang-up on the handle also ends the wait:
    // the caller's next read or write is what reports it.
    enum class When : unsigned char { Readable, Writable };
    struct Watch {
        int handle = -1;
        When when = When::Readable;
    };
    enum class Woke : unsigned char { Notified, Handle, Timeout };

    WaitSet();
    ~WaitSet();
    WaitSet(const WaitSet&) = delete;
    WaitSet& operator=(const WaitSet&) = delete;

    // Any thread, any number of times. A notify() that lands before the
    // wait starts ends that wait at once: none is lost, and many before one
    // wait end only that one.
    void notify();

    // The owning thread. Notified when a notify() came (it is consumed),
    // Handle when one of `watches` is ready, Timeout after `timeout_ms`
    // (-1: none). Entries with a negative handle are ignored. Where the OS
    // has no poll set (the unsupported stub, or a descriptor the kernel
    // would not give), handles are not watched: only notify() and the
    // timeout end the wait, which is the caller's old cadence.
    Woke wait(const Watch* watches, std::size_t count, int timeout_ms);

    // False when handles cannot be watched here (see wait()).
    bool watches_handles() const;

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

}  // namespace sonora::platform
