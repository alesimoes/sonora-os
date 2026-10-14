// SPDX-License-Identifier: GPL-3.0-or-later
//
// The process boundary between the engine and its interfaces (ADR-0007 §3,
// ADR-0008 §3 "IPC"): a message channel that keeps datagram boundaries and
// order, and a shared region the creator writes and every receiver can only
// read.
//
// POSIX: AF_UNIX SOCK_SEQPACKET under $XDG_RUNTIME_DIR/sonora/, a memfd
// sealed against new writable mappings, passed with SCM_RIGHTS as a
// read-only descriptor and mapped PROT_READ. Windows, later: a message-mode
// named pipe and a section duplicated with FILE_MAP_READ. Elsewhere every
// call fails with a reason, never silently.
//
// Callers never see a descriptor or a handle: engine/control and ui/client
// contain no socket or mapping code (ADR-0008 P9).
//
// Not for the audio thread: everything here makes system calls, and most of
// it allocates.
#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>

namespace sonora::platform {

// False where this build has no implementation; every call below then fails
// and says so.
bool ipc_supported();

// $XDG_RUNTIME_DIR/sonora/engine.sock, or /tmp/sonora-<uid>/engine.sock
// without XDG_RUNTIME_DIR. Empty where IPC is unsupported.
std::string default_control_endpoint();

// Whether something accepts connections at `endpoint` now. A socket file
// left by an engine that died does not count.
bool endpoint_live(const std::string& endpoint);

class SharedRegion {
public:
    struct Impl;

    // A zero-filled region of `bytes`, mapped read-write here. `name` is a
    // label for diagnostics (it shows in /proc/<pid>/maps).
    static std::unique_ptr<SharedRegion> create(const std::string& name, std::size_t bytes,
                                                std::string& error);

    // A zero-filled region of `bytes`, mapped read-write here, for a child
    // process that must write into it too: a plugin host's channel region
    // (ADR-0012 §4). Sealed against growing and shrinking, never against
    // writes, and never sent read-only. Passed with ChildProcess::Options.
    static std::unique_ptr<SharedRegion> create_shared_writable(const std::string& name,
                                                                std::size_t bytes,
                                                                std::string& error);

    explicit SharedRegion(std::unique_ptr<Impl> impl);
    ~SharedRegion();
    SharedRegion(const SharedRegion&) = delete;
    SharedRegion& operator=(const SharedRegion&) = delete;

    // Null in a region that was received: the kernel refuses writes there.
    void* writable_data();
    const void* data() const;
    std::size_t bytes() const;
    bool writable() const;

    // True when every new writable mapping of a create()d region is refused,
    // through any descriptor (F_SEAL_FUTURE_WRITE, Linux 5.1). Without it a
    // receiver can reopen the read-only descriptor writable through /proc,
    // so a caller that shares with an untrusted process refuses to when this
    // is false (ADR-0012, auditor L2 on PR #45).
    bool sealed_against_writes() const;

    // Keeps every page resident (mlock). Faulted in is not kept in: without
    // a lock, pages can be reclaimed and fault again on the audio thread.
    // False, with the reason, when the OS refuses; the caller reports it.
    bool lock(std::string& error);

    Impl& impl() { return *impl_; }
    const Impl& impl() const { return *impl_; }

private:
    std::unique_ptr<Impl> impl_;
};

enum class IoStatus {
    Ok,
    WouldBlock,   // nothing happened within the timeout (0: at once)
    Closed,       // the peer is gone, or shutdown() was called
    Error,        // `error` says what
};

class ControlChannel {
public:
    struct Impl;

    // Connects to a listener. Fails at once, with a reason, when nothing
    // listens there.
    static std::unique_ptr<ControlChannel> connect(const std::string& endpoint,
                                                   std::string& error);

    // A connected pair for a child process (ADR-0012 §4, one per plugin
    // host): `parent` stays here, not inherited by any child; `child_handle`
    // is an inheritable OS handle the spawner puts where the child expects
    // it (descriptor 3 for a host) and then closes here with close_handle.
    static bool pair(std::unique_ptr<ControlChannel>& parent, int& child_handle, std::string& error);
    // The child's half: takes ownership of an inherited handle, which must
    // be a connected message-mode socket. Marks it not inheritable.
    static std::unique_ptr<ControlChannel> adopt(int handle, std::string& error);
    static void close_handle(int handle);

    explicit ControlChannel(std::unique_ptr<Impl> impl);
    ~ControlChannel();

    // For a poll set in platform-level code (the plugin host supervisor
    // polls each host's channel beside its pidfd), never for I/O: readable
    // means receive(..., 0) will not return WouldBlock. -1 where IPC is
    // unsupported.
    int native_handle() const;
    ControlChannel(const ControlChannel&) = delete;
    ControlChannel& operator=(const ControlChannel&) = delete;

    // One datagram, whole or not at all. `timeout_ms`: 0 never waits, -1
    // waits as long as it takes. Any thread, but one sender at a time.
    IoStatus send(std::string_view datagram, int timeout_ms, std::string& error);
    // The same, with `region` attached so that the receiver can map it,
    // read-only.
    IoStatus send_with_region(std::string_view datagram, const SharedRegion& region,
                              int timeout_ms, std::string& error);

    // One datagram. A datagram longer than `max_bytes` is an Error, not a
    // truncated message. A region attached to it is mapped read-only into
    // `*region` when `region` is not null; when it is null, an attached
    // descriptor is closed and the datagram is an Error (the peer sent what
    // nobody asked for). One receiver at a time.
    IoStatus receive(std::string& datagram, std::size_t max_bytes,
                     std::unique_ptr<SharedRegion>* region, int timeout_ms,
                     std::string& error);

    // Wakes a blocked send or receive on any thread, which then return
    // Closed, and tells the peer the channel is gone. Idempotent.
    void shutdown();

private:
    std::unique_ptr<Impl> impl_;
};

class ControlListener {
public:
    struct Impl;

    // Creates the endpoint's directory (owner-only) if it is missing.
    // Refuses an endpoint another live listener holds; replaces a socket
    // file left by one that died.
    static std::unique_ptr<ControlListener> listen(const std::string& endpoint,
                                                   std::string& error);

    explicit ControlListener(std::unique_ptr<Impl> impl);
    // Removes the endpoint.
    ~ControlListener();
    ControlListener(const ControlListener&) = delete;
    ControlListener& operator=(const ControlListener&) = delete;

    // The next connection, or null when none arrived within `timeout_ms`
    // (`error` stays empty) or accepting failed (`error` says why).
    std::unique_ptr<ControlChannel> accept(int timeout_ms, std::string& error);

    const std::string& endpoint() const;

private:
    std::unique_ptr<Impl> impl_;
};

}  // namespace sonora::platform
