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

// --- The Info chunk's XML, scanned for one attribute ------------------------

bool at_word(std::string_view s, std::size_t at, std::string_view w) {
    return at <= s.size() && s.size() - at >= w.size() && s.substr(at, w.size()) == w;
}

bool xml_space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

bool name_char(char c) {
    const auto u = static_cast<unsigned char>(c);
    return std::isalnum(u) || c == '_' || c == '-' || c == '.' || c == ':' || u >= 0x80;
}

void append_utf8(std::string& out, uint32_t cp) {
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

// "#65" or "#x41": a Unicode scalar value, never NUL or a surrogate.
bool char_ref(std::string_view ref, uint32_t& cp) {
    const bool hex = ref.size() > 1 && ref[1] == 'x';
    const std::string_view digits = ref.substr(hex ? 2 : 1);
    if (digits.empty() || digits.size() > 7) return false;
    cp = 0;
    for (const char c : digits) {
        const auto u = static_cast<unsigned char>(c);
        if (hex ? !std::isxdigit(u) : !std::isdigit(u)) return false;
        cp = cp * (hex ? 16 : 10) + uint32_t(std::isdigit(u) ? c - '0' : std::toupper(u) - 'A' + 10);
    }
    return cp != 0 && cp <= 0x10FFFF && !(cp >= 0xD800 && cp <= 0xDFFF);
}

// An attribute value as XML reads it: the predefined entities and character
// references, whitespace normalised to spaces. Any other entity is refused,
// never looked up, so a declared one cannot expand.
bool decode_value(std::string_view v, std::string& out) {
    out.clear();
    for (std::size_t i = 0; i < v.size();) {
        const char c = v[i];
        if (c == '<') return false;
        if (c != '&') {
            out += xml_space(c) ? ' ' : c;
            ++i;
            continue;
        }
        const std::size_t end = v.find(';', i);
        if (end == std::string_view::npos || end - i > 10) return false;
        const std::string_view ent = v.substr(i + 1, end - i - 1);
        uint32_t cp = 0;
        if (ent == "amp") out += '&';
        else if (ent == "lt") out += '<';
        else if (ent == "gt") out += '>';
        else if (ent == "quot") out += '"';
        else if (ent == "apos") out += '\'';
        else if (!ent.empty() && ent[0] == '#' && char_ref(ent, cp)) append_utf8(out, cp);
        else return false;
        i = end + 1;
    }
    return true;
}

// Strict UTF-8 (no overlong form, no surrogate, nothing past U+10FFFF) with
// no control character: a name the list shows as it is.
bool printable_utf8(std::string_view s) {
    for (std::size_t i = 0; i < s.size();) {
        const auto b = static_cast<unsigned char>(s[i]);
        if (b < 0x80) {
            if (b < 0x20 || b == 0x7F) return false;
            ++i;
            continue;
        }
        const std::size_t n = b >= 0xF0 ? 4 : b >= 0xE0 ? 3 : b >= 0xC2 ? 2 : 0;
        if (n == 0 || b > 0xF4 || s.size() - i < n) return false;
        uint32_t cp = b & (n == 2 ? 0x1F : n == 3 ? 0x0F : 0x07);
        for (std::size_t k = 1; k < n; ++k) {
            const auto t = static_cast<unsigned char>(s[i + k]);
            if ((t & 0xC0) != 0x80) return false;
            cp = (cp << 6) | (t & 0x3F);
        }
        if ((n == 3 && cp < 0x800) || (n == 4 && (cp < 0x10000 || cp > 0x10FFFF)) || (cp >= 0xD800 && cp <= 0xDFFF) ||
            (cp >= 0x80 && cp <= 0x9F))
            return false;
        i += n;
    }
    return true;
}

std::string_view trimmed(std::string_view s) {
    while (!s.empty() && s.front() == ' ') s.remove_prefix(1);
    while (!s.empty() && s.back() == ' ') s.remove_suffix(1);
    return s;
}

// One start tag from `i` (at its '<'): its name, and the raw `id` and
// `value` attributes; `i` ends past its '>'. False for a malformed tag.
struct Tag {
    std::string_view name, id, value;
    bool has_id = false, has_value = false;
};

bool scan_tag(std::string_view x, std::size_t& i, Tag& t) {
    std::size_t j = i + 1;
    const std::size_t start = j;
    while (j < x.size() && name_char(x[j])) ++j;
    t = Tag{};
    t.name = x.substr(start, j - start);
    if (t.name.empty()) return false;
    for (;;) {
        while (j < x.size() && xml_space(x[j])) ++j;
        if (j >= x.size()) return false;
        if (x[j] == '>') break;
        if (x[j] == '/') {
            if (!at_word(x, j, "/>")) return false;
            ++j;
            break;
        }
        const std::size_t a = j;
        while (j < x.size() && name_char(x[j])) ++j;
        const std::string_view attr = x.substr(a, j - a);
        while (j < x.size() && xml_space(x[j])) ++j;
        if (attr.empty() || j >= x.size() || x[j] != '=') return false;
        ++j;
        while (j < x.size() && xml_space(x[j])) ++j;
        if (j >= x.size() || (x[j] != '"' && x[j] != '\'')) return false;
        const std::size_t close = x.find(x[j], j + 1);
        if (close == std::string_view::npos) return false;
        const std::string_view value = x.substr(j + 1, close - j - 1);
        j = close + 1;
        if (attr == "id" || attr == "value") {
            bool& has = attr == "id" ? t.has_id : t.has_value;
            if (has) return false;
            has = true;
            (attr == "id" ? t.id : t.value) = value;
        }
    }
    i = j + 1;
    return true;
}

// Past a comment, CDATA section or processing instruction at `i`; false when
// it never closes. `i` is unchanged when none starts there.
bool skip_markup(std::string_view x, std::size_t& i, bool& skipped) {
    static constexpr std::string_view kMarkup[][2] = {{"<!--", "-->"}, {"<![CDATA[", "]]>"}, {"<?", "?>"}};
    skipped = false;
    for (const auto& m : kMarkup) {
        const std::string_view open = m[0], close = m[1];
        if (!at_word(x, i, open)) continue;
        const std::size_t end = x.find(close, i + open.size());
        if (end == std::string_view::npos) return false;
        i = end + close.size();
        skipped = true;
        return true;
    }
    return true;
}

}  // namespace

bool info_name(std::string_view x, std::string& name) {
    if (x.size() > kMaxInfoBytes) return false;
    std::string found, id;
    bool has = false;
    for (std::size_t i = x.find('<'); i != std::string_view::npos; i = x.find('<', i)) {
        bool skipped = false;
        if (!skip_markup(x, i, skipped)) return false;
        if (skipped) continue;
        // A DOCTYPE, ENTITY or any other declaration: refused, not expanded.
        if (at_word(x, i, "<!")) return false;
        if (at_word(x, i, "</")) {
            const std::size_t end = x.find('>', i);
            if (end == std::string_view::npos) return false;
            i = end + 1;
            continue;
        }
        Tag t;
        if (!scan_tag(x, i, t)) return false;
        if (t.name != "Attr" || !t.has_id || !decode_value(t.id, id) || id != "Name") continue;
        if (has || !t.has_value || !decode_value(t.value, found)) return false;
        has = true;
    }
    if (!has || !printable_utf8(found)) return false;
    std::string_view n = trimmed(found);
    if (n.size() > kMaxNameBytes) {
        std::size_t cut = kMaxNameBytes;
        while (cut > 0 && (static_cast<unsigned char>(n[cut]) & 0xC0) == 0x80) --cut;
        n = trimmed(n.substr(0, cut));
    }
    if (n.empty()) return false;
    name = std::string(n);
    return true;
}

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
