// SPDX-License-Identifier: GPL-3.0-or-later
#include "file_key_store.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>

namespace sonora::assistant {

namespace {
std::string trim(std::string s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
    return s;
}
}  // namespace

bool FileKeyStore::load(std::string& key) const {
    key.clear();
    int fd = ::open(path_.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    struct stat st {};
    // A key file other users can read is refused, not used: the mode is the
    // promise made to the operator.
    if (::fstat(fd, &st) != 0 || (st.st_mode & 077) != 0 || st.st_size > 4096) {
        ::close(fd);
        return false;
    }
    char buf[4097];
    ssize_t n = ::read(fd, buf, sizeof buf - 1);
    ::close(fd);
    if (n <= 0) return false;
    key = trim(std::string(buf, static_cast<std::size_t>(n)));
    return !key.empty();
}

bool FileKeyStore::has_key() const {
    std::string k;
    return load(k);
}

bool FileKeyStore::save(const std::string& key, std::string& error) {
    if (key.empty()) {
        error = "empty API key";
        return false;
    }
    // Written to a 0600 temp file, then renamed: the key is never on disk
    // with a wider mode, nor half-written under the real name.
    const std::string tmp = path_ + ".tmp";
    ::unlink(tmp.c_str());
    int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0) {
        error = "cannot create " + tmp + ": " + std::strerror(errno);
        return false;
    }
    ::fchmod(fd, 0600);
    std::size_t off = 0;
    bool ok = true;
    while (off < key.size()) {
        ssize_t w = ::write(fd, key.data() + off, key.size() - off);
        if (w <= 0) {
            ok = false;
            break;
        }
        off += static_cast<std::size_t>(w);
    }
    ok = ok && ::fsync(fd) == 0;
    ::close(fd);
    if (!ok || ::rename(tmp.c_str(), path_.c_str()) != 0) {
        error = "cannot write " + path_ + ": " + std::strerror(errno);
        ::unlink(tmp.c_str());
        return false;
    }
    return true;
}

void FileKeyStore::clear() { ::unlink(path_.c_str()); }

}  // namespace sonora::assistant
