// SPDX-License-Identifier: GPL-3.0-or-later
#include "socket_transport.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdlib>

#ifdef SONORA_ASSISTANT_TLS
#include <openssl/err.h>
#include <openssl/ssl.h>
#endif

namespace sonora::assistant {

namespace {

using Clock = std::chrono::steady_clock;

struct Url {
    bool tls = false;
    std::string host, port, path;
};

bool parse_url(const std::string& u, Url& o) {
    std::string rest;
    if (u.rfind("https://", 0) == 0) {
        o.tls = true;
        rest = u.substr(8);
    } else if (u.rfind("http://", 0) == 0) {
        rest = u.substr(7);
    } else {
        return false;
    }
    auto slash = rest.find('/');
    std::string hp = rest.substr(0, slash);
    o.path = slash == std::string::npos ? "/" : rest.substr(slash);
    auto colon = hp.rfind(':');
    o.host = colon == std::string::npos ? hp : hp.substr(0, colon);
    o.port = colon == std::string::npos ? (o.tls ? "443" : "80") : hp.substr(colon + 1);
    return !o.host.empty();
}

int remaining(Clock::time_point deadline) {
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now());
    return ms.count() < 0 ? 0 : static_cast<int>(ms.count());
}

enum class Wait { Ready, Timeout, Error };

Wait wait_fd(int fd, short ev, Clock::time_point deadline) {
    pollfd p{fd, ev, 0};
    for (;;) {
        int r = ::poll(&p, 1, remaining(deadline));
        if (r > 0) return Wait::Ready;
        if (r == 0) return Wait::Timeout;
        if (errno != EINTR) return Wait::Error;
    }
}

// One connection, plain or TLS, every operation bounded by the deadline.
class Conn {
public:
    ~Conn() {
#ifdef SONORA_ASSISTANT_TLS
        if (ssl_) SSL_free(ssl_);
        if (ctx_) SSL_CTX_free(ctx_);
#endif
        if (fd_ >= 0) ::close(fd_);
    }

    TransportStatus open(const Url& u, Clock::time_point deadline) {
        addrinfo hints{};
        hints.ai_socktype = SOCK_STREAM;
        addrinfo* res = nullptr;
        if (::getaddrinfo(u.host.c_str(), u.port.c_str(), &hints, &res) != 0 || !res)
            return TransportStatus::Offline;
        TransportStatus st = TransportStatus::Offline;
        for (addrinfo* a = res; a; a = a->ai_next) {
            st = connect_one(a, deadline);
            if (st == TransportStatus::Ok || st == TransportStatus::Timeout) break;
        }
        ::freeaddrinfo(res);
        if (st != TransportStatus::Ok) return st;
        return u.tls ? handshake(u.host, deadline) : TransportStatus::Ok;
    }

    TransportStatus send_all(const std::string& d, Clock::time_point deadline) {
        std::size_t off = 0;
        while (off < d.size()) {
            long n = raw_write(d.data() + off, d.size() - off, deadline);
            if (n == -2) return TransportStatus::Timeout;
            if (n < 0) return TransportStatus::Failed;
            off += static_cast<std::size_t>(n);
        }
        return TransportStatus::Ok;
    }

    TransportStatus read_all(std::string& out, Clock::time_point deadline) {
        char buf[4096];
        for (;;) {
            long n = raw_read(buf, sizeof buf, deadline);
            if (n == -2) return TransportStatus::Timeout;
            if (n < 0) return TransportStatus::Failed;
            if (n == 0) return TransportStatus::Ok;
            out.append(buf, static_cast<std::size_t>(n));
            if (complete(out)) return TransportStatus::Ok;
        }
    }

private:
    // A response with Content-Length is complete without waiting for close.
    static bool complete(const std::string& r) {
        auto he = r.find("\r\n\r\n");
        if (he == std::string::npos) return false;
        auto cl = r.find("Content-Length:");
        if (cl == std::string::npos || cl > he) cl = r.find("content-length:");
        if (cl == std::string::npos || cl > he) return false;
        std::size_t len = std::strtoul(r.c_str() + cl + 15, nullptr, 10);
        return r.size() >= he + 4 + len;
    }

    TransportStatus reset(TransportStatus s) {
        ::close(fd_);
        fd_ = -1;
        return s;
    }

    TransportStatus connect_one(addrinfo* a, Clock::time_point deadline) {
        fd_ = ::socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (fd_ < 0) return TransportStatus::Failed;
        ::fcntl(fd_, F_SETFL, ::fcntl(fd_, F_GETFL, 0) | O_NONBLOCK);
        if (::connect(fd_, a->ai_addr, a->ai_addrlen) == 0) return TransportStatus::Ok;
        if (errno != EINPROGRESS) return reset(TransportStatus::Offline);
        Wait w = wait_fd(fd_, POLLOUT, deadline);
        if (w == Wait::Timeout) return reset(TransportStatus::Timeout);
        int err = 0;
        socklen_t l = sizeof err;
        if (w == Wait::Error || ::getsockopt(fd_, SOL_SOCKET, SO_ERROR, &err, &l) != 0 || err != 0)
            return reset(TransportStatus::Offline);
        return TransportStatus::Ok;
    }

#ifdef SONORA_ASSISTANT_TLS
    Wait want(int r, Clock::time_point deadline) {
        int e = SSL_get_error(ssl_, r);
        if (e == SSL_ERROR_WANT_READ) return wait_fd(fd_, POLLIN, deadline);
        if (e == SSL_ERROR_WANT_WRITE) return wait_fd(fd_, POLLOUT, deadline);
        return Wait::Error;
    }

    TransportStatus handshake(const std::string& host, Clock::time_point deadline) {
        ctx_ = SSL_CTX_new(TLS_client_method());
        if (!ctx_) return TransportStatus::Failed;
        SSL_CTX_set_default_verify_paths(ctx_);
        SSL_CTX_set_verify(ctx_, SSL_VERIFY_PEER, nullptr);
        ssl_ = SSL_new(ctx_);
        if (!ssl_) return TransportStatus::Failed;
        SSL_set_fd(ssl_, fd_);
        SSL_set_tlsext_host_name(ssl_, host.c_str());
        SSL_set1_host(ssl_, host.c_str());
        for (;;) {
            int r = SSL_connect(ssl_);
            if (r == 1) return TransportStatus::Ok;
            Wait w = want(r, deadline);
            if (w == Wait::Timeout) return TransportStatus::Timeout;
            if (w == Wait::Error) return TransportStatus::Failed;
        }
    }
#else
    TransportStatus handshake(const std::string&, Clock::time_point) {
        // No TLS in this build: refuse, never send the key in clear.
        return TransportStatus::Failed;
    }
#endif

    // -2 timeout, -1 error, otherwise bytes.
    long raw_write(const char* p, std::size_t n, Clock::time_point deadline) {
        for (;;) {
#ifdef SONORA_ASSISTANT_TLS
            if (ssl_) {
                int r = SSL_write(ssl_, p, static_cast<int>(n));
                if (r > 0) return r;
                Wait w = want(r, deadline);
                if (w == Wait::Timeout) return -2;
                if (w == Wait::Error) return -1;
                continue;
            }
#endif
            long r = ::send(fd_, p, n, MSG_NOSIGNAL);
            if (r >= 0) return r;
            if (errno != EAGAIN && errno != EINTR) return -1;
            Wait w = wait_fd(fd_, POLLOUT, deadline);
            if (w == Wait::Timeout) return -2;
            if (w == Wait::Error) return -1;
        }
    }

    long raw_read(char* p, std::size_t n, Clock::time_point deadline) {
        for (;;) {
#ifdef SONORA_ASSISTANT_TLS
            if (ssl_) {
                int r = SSL_read(ssl_, p, static_cast<int>(n));
                if (r > 0) return r;
                if (SSL_get_error(ssl_, r) == SSL_ERROR_ZERO_RETURN) return 0;
                Wait w = want(r, deadline);
                if (w == Wait::Timeout) return -2;
                if (w == Wait::Error) return -1;
                continue;
            }
#endif
            long r = ::recv(fd_, p, n, 0);
            if (r >= 0) return r;
            if (errno != EAGAIN && errno != EINTR) return -1;
            Wait w = wait_fd(fd_, POLLIN, deadline);
            if (w == Wait::Timeout) return -2;
            if (w == Wait::Error) return -1;
        }
    }

    int fd_ = -1;
#ifdef SONORA_ASSISTANT_TLS
    SSL_CTX* ctx_ = nullptr;
    SSL* ssl_ = nullptr;
#endif
};

bool split_response(const std::string& raw, Response& out) {
    if (raw.rfind("HTTP/", 0) != 0) return false;
    auto sp = raw.find(' ');
    if (sp == std::string::npos) return false;
    out.http_status = std::atoi(raw.c_str() + sp + 1);
    auto he = raw.find("\r\n\r\n");
    out.body = he == std::string::npos ? std::string() : raw.substr(he + 4);
    return out.http_status > 0;
}

}  // namespace

TransportStatus SocketTransport::post(const Request& request, const std::string& key,
                                      Response& out) {
    out = Response{};
    auto say = [&](const std::string& m) {
        if (cfg_.log) cfg_.log(m);
    };
    Url u;
    if (!parse_url(cfg_.url, u)) {
        say("assistant: bad endpoint url");
        return out.status = TransportStatus::Failed;
    }
    const auto deadline = Clock::now() + std::chrono::milliseconds(cfg_.timeout_ms);
    Conn c;
    TransportStatus st = c.open(u, deadline);
    if (st == TransportStatus::Ok) {
        std::string head = "POST " + u.path + " HTTP/1.1\r\nHost: " + u.host +
                           "\r\nContent-Type: application/json\r\n" + cfg_.key_header + ": " +
                           key + "\r\nContent-Length: " + std::to_string(request.body.size()) +
                           "\r\nConnection: close\r\n\r\n";
        st = c.send_all(head + request.body, deadline);
        head.assign(head.size(), '\0');
    }
    std::string raw;
    if (st == TransportStatus::Ok) st = c.read_all(raw, deadline);
    if (st == TransportStatus::Ok && !split_response(raw, out)) st = TransportStatus::Failed;
    // Size and outcome only: neither the body nor the key is ever logged.
    say("assistant: POST " + u.host + " status " + std::to_string(static_cast<int>(st)) +
        " http " + std::to_string(out.http_status) + " sent " +
        std::to_string(request.body.size()) + " bytes");
    return out.status = st;
}

}  // namespace sonora::assistant
