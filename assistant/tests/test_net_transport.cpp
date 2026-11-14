// SPDX-License-Identifier: GPL-3.0-or-later
// Loopback tests of the real transport and key file (ADR-0155 §5, §6).
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

#include "../src/net/file_key_store.h"
#include "../src/net/socket_transport.h"

#define CHECK(c)                                                    \
    do {                                                            \
        if (!(c)) {                                                 \
            std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); \
            std::exit(1);                                           \
        }                                                           \
    } while (0)

using namespace sonora::assistant;

namespace {
const std::string kKey = "sk-test-SECRET-1234567890";

// Listens on an ephemeral loopback port; reply == nullptr means accept and stay silent.
struct Server {
    int fd = -1;
    int port = 0;
    std::string seen;
    std::thread th;
    std::atomic<bool> stop{false};
    explicit Server(const char* reply) {
        fd = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        ::bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof a);
        socklen_t l = sizeof a;
        ::getsockname(fd, reinterpret_cast<sockaddr*>(&a), &l);
        port = ntohs(a.sin_port);
        ::listen(fd, 1);
        th = std::thread([this, reply] {
            int c = ::accept(fd, nullptr, nullptr);
            if (c < 0) return;
            if (reply) {
                char b[8192];
                ssize_t n = ::recv(c, b, sizeof b, 0);
                if (n > 0) seen.assign(b, static_cast<std::size_t>(n));
                ::send(c, reply, std::char_traits<char>::length(reply), 0);
                ::close(c);
            } else {
                while (!stop) ::usleep(1000);
                ::close(c);
            }
        });
    }
    ~Server() {
        stop = true;
        ::shutdown(fd, SHUT_RDWR);
        ::close(fd);
        if (th.joinable()) th.join();
    }
    std::string url() const { return "http://127.0.0.1:" + std::to_string(port) + "/v1/messages"; }
};
}  // namespace

int main() {
    std::string logs;
    auto log = [&](const std::string& m) { logs += m + "\n"; };

    {  // a reply comes back; the key is a header, never in the body
        Server s("HTTP/1.1 200 OK\r\nContent-Length: 4\r\n\r\npong");
        SocketTransport t({s.url(), 2000, "x-api-key", log});
        Response r;
        CHECK(t.post(Request{"{\"a\":1}"}, kKey, r) == TransportStatus::Ok);
        CHECK(r.http_status == 200 && r.body == "pong");
        CHECK(s.seen.find("x-api-key: " + kKey) != std::string::npos);
        CHECK(s.seen.substr(s.seen.find("\r\n\r\n")).find(kKey) == std::string::npos);
    }
    {  // a server that never answers: Timeout within bounds, no retry
        Server s(nullptr);
        SocketTransport t({s.url(), 300, "x-api-key", log});
        Response r;
        auto t0 = std::chrono::steady_clock::now();
        CHECK(t.post(Request{"{}"}, kKey, r) == TransportStatus::Timeout);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now() - t0)
                      .count();
        CHECK(ms < 3000);
    }
    {  // nothing listening: Offline
        int port;
        {
            Server s("x");
            port = s.port;
        }
        SocketTransport t({"http://127.0.0.1:" + std::to_string(port) + "/", 1000, "x-api-key", log});
        Response r;
        CHECK(t.post(Request{"{}"}, kKey, r) == TransportStatus::Offline);
    }
    {  // a bad url fails without sending
        SocketTransport t({"ftp://x/", 500, "x-api-key", log});
        Response r;
        CHECK(t.post(Request{"{}"}, kKey, r) == TransportStatus::Failed);
    }
    CHECK(logs.find(kKey) == std::string::npos);
    CHECK(logs.find("SECRET") == std::string::npos);

    {  // key file: 0600, round trip, wide mode refused, empty refused, clear
        std::string path = "/tmp/sonora_key_test_" + std::to_string(::getpid());
        FileKeyStore ks(path);
        std::string err, got;
        CHECK(!ks.has_key());
        CHECK(!ks.save("", err));
        CHECK(ks.save(kKey, err));
        struct stat st {};
        CHECK(::stat(path.c_str(), &st) == 0);
        CHECK((st.st_mode & 0777) == 0600);
        CHECK(ks.load(got) && got == kKey);
        ::chmod(path.c_str(), 0644);
        CHECK(!ks.load(got) && got.empty());
        ::chmod(path.c_str(), 0600);
        CHECK(ks.has_key());
        ks.clear();
        CHECK(!ks.has_key());
        CHECK(::stat((path + ".tmp").c_str(), &st) != 0);
    }
    std::puts("assistant net: ok");
    return 0;
}
