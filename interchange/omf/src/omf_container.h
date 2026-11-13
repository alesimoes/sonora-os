// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Bounds-checked view of an OMFI container (ADR-0160). Layout as this reader
// understands it (big or little endian, marker in the header):
//   header : 8 magic bytes A4 'C' 'M' A5 'H' 'd' 'r' D7, u16 order (0 big,
//            1 little), u16 version, u32 toc_offset, u32 toc_size
//   toc    : u32 object_count, then per object u32 id, u32 class fourcc,
//            u32 prop_count, then per property u32 name fourcc, u32 offset,
//            u32 length (offset into the file, value bytes)
// Every access goes through Cursor/prop_bytes, which refuse out-of-range
// reads; the TOC walk is bounded by the file size, so no input loops.

#include <cstdint>
#include <string>
#include <vector>

namespace sonora::interchange {

constexpr std::uint32_t fourcc(const char (&s)[5]) {
    return (std::uint32_t(std::uint8_t(s[0])) << 24) | (std::uint32_t(std::uint8_t(s[1])) << 16) |
           (std::uint32_t(std::uint8_t(s[2])) << 8) | std::uint32_t(std::uint8_t(s[3]));
}

struct OmfProp {
    std::uint32_t name = 0, offset = 0, length = 0;
};

struct OmfObject {
    std::uint32_t id = 0, cls = 0;
    std::vector<OmfProp> props;
    const OmfProp* find(std::uint32_t name) const;
};

class OmfContainer {
public:
    // False with `error` set on any malformed or truncated input.
    bool parse(std::vector<std::uint8_t> bytes, std::string& error);

    const std::vector<OmfObject>& objects() const { return objects_; }
    const OmfObject* object(std::uint32_t id) const;
    std::uint16_t version() const { return version_; }

    bool prop_bytes(const OmfProp& p, const std::uint8_t*& data, std::size_t& len) const;
    bool prop_u32(const OmfObject& o, std::uint32_t name, std::uint32_t& v) const;
    bool prop_i64(const OmfObject& o, std::uint32_t name, std::int64_t& v) const;
    bool prop_string(const OmfObject& o, std::uint32_t name, std::string& v) const;
    // A list of u32 object ids; false when the length is not a multiple of 4.
    bool prop_ids(const OmfObject& o, std::uint32_t name, std::vector<std::uint32_t>& v) const;

private:
    bool u32_at(std::size_t off, std::uint32_t& v) const;
    bool le_ = false;
    std::uint16_t version_ = 0;
    std::vector<std::uint8_t> bytes_;
    std::vector<OmfObject> objects_;
};

}  // namespace sonora::interchange
