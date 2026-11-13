// SPDX-License-Identifier: GPL-3.0-or-later
#include "omf_container.h"

#include <cstring>

namespace sonora::interchange {

namespace {
const std::uint8_t kMagic[8] = {0xA4, 'C', 'M', 0xA5, 'H', 'd', 'r', 0xD7};
constexpr std::size_t kHeader = 20;
constexpr std::uint32_t kMaxProps = 4096;
}  // namespace

const OmfProp* OmfObject::find(std::uint32_t name) const {
    for (const auto& p : props)
        if (p.name == name) return &p;
    return nullptr;
}

bool OmfContainer::u32_at(std::size_t off, std::uint32_t& v) const {
    if (off > bytes_.size() || bytes_.size() - off < 4) return false;
    const std::uint8_t* b = bytes_.data() + off;
    v = le_ ? (std::uint32_t(b[3]) << 24) | (std::uint32_t(b[2]) << 16) |
                  (std::uint32_t(b[1]) << 8) | b[0]
            : (std::uint32_t(b[0]) << 24) | (std::uint32_t(b[1]) << 16) |
                  (std::uint32_t(b[2]) << 8) | b[3];
    return true;
}

bool OmfContainer::parse(std::vector<std::uint8_t> bytes, std::string& error) {
    objects_.clear();
    bytes_ = std::move(bytes);
    if (bytes_.size() < kHeader || std::memcmp(bytes_.data(), kMagic, 8) != 0) {
        error = "not an OMF interchange file (bad header)";
        return false;
    }
    if (bytes_[8] != 0 || bytes_[9] > 1) {
        error = "OMF header has an unknown byte order";
        return false;
    }
    le_ = bytes_[9] == 1;
    version_ = le_ ? std::uint16_t(bytes_[10] | (bytes_[11] << 8))
                   : std::uint16_t((bytes_[10] << 8) | bytes_[11]);
    std::uint32_t toc = 0, toc_size = 0, count = 0;
    if (!u32_at(12, toc) || !u32_at(16, toc_size) || toc < kHeader || toc > bytes_.size() ||
        toc_size > bytes_.size() - toc || !u32_at(toc, count)) {
        error = "OMF table of contents is outside the file";
        return false;
    }
    const std::size_t end = std::size_t(toc) + toc_size;
    std::size_t pos = std::size_t(toc) + 4;
    // Each object takes at least 12 bytes, so a count beyond that is a lie.
    if (count > toc_size / 12) {
        error = "OMF object count exceeds the table size";
        return false;
    }
    for (std::uint32_t i = 0; i < count; ++i) {
        OmfObject o;
        std::uint32_t np = 0;
        if (pos + 12 > end || !u32_at(pos, o.id) || !u32_at(pos + 4, o.cls) ||
            !u32_at(pos + 8, np) || np > kMaxProps || std::size_t(np) * 12 > end - pos - 12) {
            error = "OMF object index is truncated or corrupt";
            return false;
        }
        pos += 12;
        for (std::uint32_t k = 0; k < np; ++k, pos += 12) {
            OmfProp p;
            u32_at(pos, p.name);
            u32_at(pos + 4, p.offset);
            u32_at(pos + 8, p.length);
            if (p.offset > bytes_.size() || p.length > bytes_.size() - p.offset) {
                error = "OMF property points outside the file";
                return false;
            }
            o.props.push_back(p);
        }
        objects_.push_back(std::move(o));
    }
    return true;
}

const OmfObject* OmfContainer::object(std::uint32_t id) const {
    for (const auto& o : objects_)
        if (o.id == id) return &o;
    return nullptr;
}

bool OmfContainer::prop_bytes(const OmfProp& p, const std::uint8_t*& data,
                              std::size_t& len) const {
    if (p.offset > bytes_.size() || p.length > bytes_.size() - p.offset) return false;
    data = bytes_.data() + p.offset;
    len = p.length;
    return true;
}

bool OmfContainer::prop_u32(const OmfObject& o, std::uint32_t name, std::uint32_t& v) const {
    const OmfProp* p = o.find(name);
    return p && p->length == 4 && u32_at(p->offset, v);
}

bool OmfContainer::prop_i64(const OmfObject& o, std::uint32_t name, std::int64_t& v) const {
    const OmfProp* p = o.find(name);
    std::uint32_t a = 0, b = 0;
    if (!p || p->length != 8 || !u32_at(p->offset, a) || !u32_at(p->offset + 4, b)) return false;
    const std::uint64_t hi = le_ ? b : a, lo = le_ ? a : b;
    v = std::int64_t((hi << 32) | lo);
    return true;
}

bool OmfContainer::prop_string(const OmfObject& o, std::uint32_t name, std::string& v) const {
    const OmfProp* p = o.find(name);
    const std::uint8_t* d = nullptr;
    std::size_t n = 0;
    if (!p || !prop_bytes(*p, d, n)) return false;
    v.assign(reinterpret_cast<const char*>(d), n);
    const auto z = v.find('\0');
    if (z != std::string::npos) v.resize(z);
    return true;
}

bool OmfContainer::prop_ids(const OmfObject& o, std::uint32_t name,
                            std::vector<std::uint32_t>& v) const {
    const OmfProp* p = o.find(name);
    if (!p || p->length % 4 != 0) return false;
    v.clear();
    for (std::uint32_t i = 0; i < p->length; i += 4) {
        std::uint32_t id = 0;
        if (!u32_at(p->offset + i, id)) return false;
        v.push_back(id);
    }
    return true;
}

}  // namespace sonora::interchange
