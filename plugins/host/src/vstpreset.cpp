// SPDX-License-Identifier: GPL-3.0-or-later
#include "vstpreset.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <system_error>

#include <sys/stat.h>

namespace sonora::plugin_host::vstpreset {

namespace fs = std::filesystem;

namespace {

constexpr int32_t kFormatVersion = 1;
constexpr std::size_t kEntryBytes = 20;
constexpr std::size_t kMaxWalked = 16384;
constexpr int kMaxDepth = 4;
constexpr const char* kExtension = ".vstpreset";

// Little-endian, from bytes the caller has checked are there.
uint64_t u64(std::string_view b, std::size_t at) {
    uint64_t v = 0;
    for (int k = 7; k >= 0; --k) v = (v << 8) | static_cast<unsigned char>(b[at + std::size_t(k)]);
    return v;
}

uint32_t u32(std::string_view b, std::size_t at) {
    uint32_t v = 0;
    for (int k = 3; k >= 0; --k) v = (v << 8) | static_cast<unsigned char>(b[at + std::size_t(k)]);
    return v;
}

std::string printable(std::string_view id) {
    std::string s;
    for (const char c : id) s += std::isprint(static_cast<unsigned char>(c)) ? c : '?';
    return s;
}

std::string upper(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return out;
}

// COM byte order (Windows GUIDs) for the same 16 bytes: the first three
// fields reversed.
std::string other_order(const std::string& hex) {
    if (hex.size() != 32) return hex;
    std::string o = hex;
    const auto swap = [&](std::size_t from, std::size_t bytes) {
        for (std::size_t k = 0; k < bytes; ++k) o.replace(2 * (from + k), 2, hex, 2 * (from + bytes - 1 - k), 2);
    };
    swap(0, 4);
    swap(4, 2);
    swap(6, 2);
    return o;
}

bool one_component(const std::string& s) {
    return !s.empty() && s != "." && s != ".." && s.find('/') == std::string::npos && s.find('\0') == std::string::npos;
}

bool stat_regular(const std::string& path, uint64_t& size, std::string& error) {
    struct stat st {};
    if (::stat(path.c_str(), &st) != 0) {
        error = path + ": " + std::strerror(errno);
        return false;
    }
    if (!S_ISREG(st.st_mode)) {
        error = path + " is not a regular file";
        return false;
    }
    size = static_cast<uint64_t>(st.st_size);
    if (size > kMaxFileBytes) {
        error = path + " is " + std::to_string(size) + " bytes, over the " + std::to_string(kMaxFileBytes) +
                " a preset may be";
        return false;
    }
    return true;
}

bool read_bytes(const std::string& path, std::size_t want, std::string& bytes, std::string& error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = path + " could not be opened";
        return false;
    }
    bytes.assign(want, '\0');
    in.read(bytes.data(), static_cast<std::streamsize>(want));
    bytes.resize(static_cast<std::size_t>(std::max<std::streamsize>(in.gcount(), 0)));
    return true;
}

// `n` bytes at `at`, or false when the file holds fewer there.
bool read_at(std::ifstream& in, uint64_t at, std::size_t n, std::string& out) {
    in.clear();
    in.seekg(static_cast<std::streamoff>(at));
    out.assign(n, '\0');
    if (n == 0) return bool(in);
    in.read(out.data(), static_cast<std::streamsize>(n));
    return in.gcount() == static_cast<std::streamsize>(n);
}

// Chunk data lies between the header and the list. Unsigned, so a
// negative int64 is a huge value and fails the same test.
bool inside_data(uint64_t off, uint64_t size, uint64_t list) {
    return off >= kHeaderBytes && off <= list && size <= list - off;
}

}  // namespace

bool load_info(const std::string& path, std::string& info, std::string& error) {
    uint64_t size = 0;
    if (!stat_regular(path, size, error)) return false;
    std::ifstream in(path, std::ios::binary);
    std::string head, id;
    if (!in || !read_at(in, 0, kHeaderBytes, head)) {
        error = path + " could not be read";
        return false;
    }
    if (!read_header(head, id, error)) return false;
    const uint64_t list = u64(head, 40);
    std::string lh;
    if (list < kHeaderBytes || list > size || size - list < 8 || !read_at(in, list, 8, lh) ||
        lh.compare(0, 4, "List") != 0) {
        error = "the .vstpreset's chunk list is not where its header says";
        return false;
    }
    const auto count = static_cast<int32_t>(u32(lh, 4));
    std::string entries;
    if (count < 0 || count > kMaxEntries || (size - list - 8) / kEntryBytes < std::size_t(count) ||
        !read_at(in, list + 8, std::size_t(count) * kEntryBytes, entries)) {
        error = "the .vstpreset's chunk list is cut short or too long";
        return false;
    }
    int infos = 0;
    uint64_t off = 0, len = 0;
    for (std::size_t e = 0; e < entries.size(); e += kEntryBytes) {
        if (entries.compare(e, 4, "Info") != 0) continue;
        ++infos;
        off = u64(entries, e + 4);
        len = u64(entries, e + 12);
    }
    if (infos != 1) {
        error = infos == 0 ? "the .vstpreset has no 'Info' chunk" : "the .vstpreset has two 'Info' chunks";
        return false;
    }
    if (!inside_data(off, len, list) || len > kMaxInfoBytes || !read_at(in, off, std::size_t(len), info)) {
        error = "the .vstpreset's 'Info' chunk is outside its data or over " + std::to_string(kMaxInfoBytes) + " bytes";
        return false;
    }
    return true;
}

bool read_header(std::string_view b, std::string& class_id, std::string& error) {
    if (b.size() < kHeaderBytes) {
        error = "a .vstpreset of " + std::to_string(b.size()) + " bytes is shorter than its header";
        return false;
    }
    if (b.substr(0, 4) != "VST3") {
        error = "not a .vstpreset (no 'VST3' at its start)";
        return false;
    }
    const auto version = static_cast<int32_t>(u32(b, 4));
    if (version != kFormatVersion) {
        error = "a .vstpreset of format version " + std::to_string(version) + ", not " + std::to_string(kFormatVersion);
        return false;
    }
    const std::string_view id = b.substr(8, 32);
    for (const char c : id)
        if (!std::isxdigit(static_cast<unsigned char>(c))) {
            error = "the .vstpreset's class id \"" + printable(id) + "\" is not hexadecimal";
            return false;
        }
    class_id = upper(id);
    return true;
}

bool parse(std::string_view b, File& out, std::string& error) {
    File f;
    if (!read_header(b, f.class_id, error)) return false;
    if (b.size() > kMaxFileBytes) {
        error = "a .vstpreset of " + std::to_string(b.size()) + " bytes, over the cap of " + std::to_string(kMaxFileBytes);
        return false;
    }
    const uint64_t list = u64(b, 40);
    // Unsigned, so a negative int64 is a huge offset and fails the same test.
    if (list < kHeaderBytes || list > b.size() || b.size() - list < 8) {
        error = "the .vstpreset's chunk list offset " + std::to_string(list) + " is outside its " +
                std::to_string(b.size()) + " bytes";
        return false;
    }
    if (b.substr(std::size_t(list), 4) != "List") {
        error = "the .vstpreset has no 'List' where its chunk list should be";
        return false;
    }
    const auto count = static_cast<int32_t>(u32(b, std::size_t(list) + 4));
    if (count < 0 || count > kMaxEntries) {
        error = "the .vstpreset lists " + std::to_string(count) + " chunks; at most " + std::to_string(kMaxEntries) +
                " are read";
        return false;
    }
    const std::size_t entries = std::size_t(list) + 8;
    if ((b.size() - entries) / kEntryBytes < std::size_t(count)) {
        error = "the .vstpreset's chunk list is cut short";
        return false;
    }
    std::set<std::string> seen;
    bool has_comp = false;
    for (int32_t k = 0; k < count; ++k) {
        const std::size_t e = entries + std::size_t(k) * kEntryBytes;
        const std::string id(b.substr(e, 4));
        const uint64_t off = u64(b, e + 4);
        const uint64_t size = u64(b, e + 12);
        if (!seen.insert(id).second) {
            error = "the .vstpreset has two '" + printable(id) + "' chunks";
            return false;
        }
        if (!inside_data(off, size, list)) {
            error = "the .vstpreset's '" + printable(id) + "' chunk (" + std::to_string(size) + " bytes at " +
                    std::to_string(off) + ") is outside its data";
            return false;
        }
        const std::string_view data = b.substr(std::size_t(off), std::size_t(size));
        if (id == "Comp") {
            f.component = std::string(data);
            has_comp = true;
        } else if (id == "Cont") {
            f.controller = std::string(data);
            f.has_controller = true;
        }
    }
    if (!has_comp) {
        error = "the .vstpreset has no component state ('Comp' chunk)";
        return false;
    }
    out = std::move(f);
    return true;
}

bool same_class(std::string_view file_class_id, std::string_view plugin_id) {
    constexpr std::string_view kPrefix = "vst3:";
    if (plugin_id.substr(0, kPrefix.size()) != kPrefix) return false;
    const std::string want = upper(plugin_id.substr(kPrefix.size()));
    const std::string got = upper(file_class_id);
    return want.size() == 32 && got.size() == 32 && (got == want || got == other_order(want));
}

bool load(const std::string& path, std::string& bytes, std::string& error) {
    uint64_t size = 0;
    if (!stat_regular(path, size, error)) return false;
    if (!read_bytes(path, std::size_t(size), bytes, error)) return false;
    if (bytes.size() != size) {
        error = path + " changed while it was read";
        return false;
    }
    return true;
}

bool load_header(const std::string& path, std::string& bytes, std::string& error) {
    uint64_t size = 0;
    if (!stat_regular(path, size, error)) return false;
    return read_bytes(path, kHeaderBytes, bytes, error);
}

std::vector<std::string> default_roots() {
    std::vector<std::string> roots;
    if (const char* home = std::getenv("HOME"); home && *home == '/') roots.push_back(std::string(home) + "/.vst3/presets");
    roots.push_back("/usr/share/vst3/presets");
    roots.push_back("/usr/local/share/vst3/presets");
    return roots;
}

std::vector<Listed> list(const std::vector<std::string>& roots, const std::string& vendor,
                         const std::string& plugin_name, const std::string& plugin_id, std::size_t cap) {
    std::vector<Listed> out;
    if (!one_component(vendor) || !one_component(plugin_name)) return out;
    std::set<std::string> taken;
    for (const std::string& root : roots) {
        std::error_code ec;
        const fs::path dir = fs::path(root) / vendor / plugin_name;
        if (!fs::is_directory(dir, ec)) continue;
        std::vector<Listed> here;
        std::size_t walked = 0;
        fs::recursive_directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec);
        for (; !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (++walked > kMaxWalked) break;
            if (it.depth() >= kMaxDepth) it.disable_recursion_pending();
            const fs::directory_entry& e = *it;
            std::error_code fe;
            // A symlink is neither followed into nor listed: it could lead
            // anywhere, and resolve() would refuse it anyway.
            if (e.is_symlink(fe) || !e.is_regular_file(fe)) continue;
            const std::string file = e.path().filename().string();
            if (file.size() <= std::strlen(kExtension) ||
                file.compare(file.size() - std::strlen(kExtension), std::string::npos, kExtension) != 0)
                continue;
            std::string header, why, class_id;
            if (!load_header(e.path().string(), header, why) || !read_header(header, class_id, why) ||
                !same_class(class_id, plugin_id))
                continue;
            const std::string rel = e.path().lexically_relative(root).generic_string();
            if (rel.empty() || rel.rfind("..", 0) == 0) continue;
            std::string name = file.substr(0, file.size() - std::strlen(kExtension)), info, display;
            if (load_info(e.path().string(), info, why) && info_name(info, display)) name = std::move(display);
            here.push_back({rel, std::move(name)});
        }
        // Directory order is the filesystem's: sort, so a listing is the
        // same on every machine.
        std::sort(here.begin(), here.end(), [](const Listed& a, const Listed& b) { return a.relative < b.relative; });
        for (Listed& l : here) {
            if (out.size() >= cap) return out;
            if (taken.insert(l.relative).second) out.push_back(std::move(l));
        }
    }
    return out;
}

bool resolve(const std::vector<std::string>& roots, const std::string& relative, std::string& path,
             std::string& error) {
    if (relative.empty() || relative.front() == '/' || relative.find('\0') != std::string::npos) {
        error = "\"" + relative + "\" is not a path relative to a VST3 preset folder";
        return false;
    }
    for (const fs::path& part : fs::path(relative)) {
        const std::string p = part.string();
        if (p.empty() || p == "." || p == "..") {
            error = "\"" + relative + "\" leaves the VST3 preset folders";
            return false;
        }
    }
    if (relative.size() <= std::strlen(kExtension) ||
        relative.compare(relative.size() - std::strlen(kExtension), std::string::npos, kExtension) != 0) {
        error = "\"" + relative + "\" is not a .vstpreset";
        return false;
    }
    for (const std::string& root : roots) {
        std::error_code ec;
        const fs::path real_root = fs::canonical(root, ec);
        if (ec) continue;
        const fs::path real = fs::canonical(fs::path(root) / relative, ec);
        if (ec || !fs::is_regular_file(real, ec)) continue;
        const std::string r = real_root.string() + "/";
        if (real.string().compare(0, r.size(), r) != 0) {
            error = "\"" + relative + "\" leads out of " + root;
            return false;
        }
        path = real.string();
        return true;
    }
    error = "no VST3 preset folder holds \"" + relative + "\"";
    return false;
}

}  // namespace sonora::plugin_host::vstpreset
