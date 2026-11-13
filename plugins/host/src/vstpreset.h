// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The VST3 preset file (.vstpreset), read for ADR-0029 §5 from the layout
// Steinberg's SDK publishes (vstpresetfile.h, MIT since 3.8;
// brain/research/plugin-host-vst3.md):
//
//   0   'VST3'                         4 bytes
//   4   format version                 int32, little-endian
//   8   class id, ASCII hex            32 bytes
//   40  offset of the chunk list       int64
//   ... chunk data
//   list: 'List', entry count (int32), then per entry
//         chunk id (4 bytes), offset (int64), size (int64)
//
// 'Comp' (the component's state) and 'Cont' (the controller's) become a
// StateBlob's two sections; 'Info' is read for the preset's display name
// only (ADR-0144) and 'Prog' is not read. The file is
// untrusted input (anyone can drop one into ~/.vst3/presets), so nothing
// here trusts an offset, a size or a count before checking it against the
// bytes that are actually there, and no plugin code runs. No SDK, no lilv:
// the fuzz test builds this alone.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace sonora::plugin_host::vstpreset {

inline constexpr std::size_t kHeaderBytes = 48;
// A StateBlob's cap (host_control.h kMaxStateBytes) and room for the
// header, the list and an 'Info' chunk. A file over it is refused before a
// byte of it is read.
inline constexpr std::size_t kMaxFileBytes = (std::size_t{16} << 20) + (std::size_t{1} << 20);
// The SDK writes four entries at most.
inline constexpr int32_t kMaxEntries = 64;

struct File {
    std::string class_id;     // 32 hex digits, upper case
    std::string component;    // 'Comp'
    std::string controller;   // 'Cont', empty when absent
    bool has_controller = false;
};

// The class id of a header (the first kHeaderBytes of a file), without
// reading the rest: what listing checks. False, with the reason, when the
// bytes are not a .vstpreset header.
bool read_header(std::string_view bytes, std::string& class_id, std::string& error);

// A whole file in memory. False, with the reason, for anything but a
// well-formed file: short, another magic or version, a class id that is
// not hex, a chunk list or chunk outside the bytes or inside the header,
// two chunks of one id, no 'Comp'.
bool parse(std::string_view bytes, File& out, std::string& error);

// Whether a file's class id names the plugin `plugin_hex` ("vst3:" and 32
// hex digits, as vst3_host.h has it). Either byte order is the same class:
// the SDK writes FUID::toString, and a Windows-made file of a bridged
// plugin (ADR-0017) may spell it in COM order.
bool same_class(std::string_view file_class_id, std::string_view plugin_id);

// Reads `path` whole: a regular file, at most kMaxFileBytes (checked with
// stat before it is opened for reading, so a hostile 8 GB file costs
// nothing). False, with the reason.
bool load(const std::string& path, std::string& bytes, std::string& error);
// The first kHeaderBytes of `path`, the same checks.
bool load_header(const std::string& path, std::string& bytes, std::string& error);

// The 'Info' chunk is XML the SDK's PresetFile writes, a <MetaInfo> of
// <Attr id="..." value="..."/> entries. Only the Attr whose id is "Name" is
// used (ADR-0144): category and the rest would need a probe record version
// 4. An Info over kMaxInfoBytes is not scanned; a name over kMaxNameBytes is
// cut at a character boundary.
inline constexpr std::size_t kMaxInfoBytes = std::size_t{64} << 10;
inline constexpr std::size_t kMaxNameBytes = 128;

// The display name an Info chunk carries, decoded: the five predefined
// entities and character references only. False for anything else: no
// Name, two of them, a DOCTYPE or declared entity (never expanded), an
// unknown entity, a tag cut short, a name that is not valid UTF-8 or holds
// a control character, an empty name, an Info over the cap. A bounded,
// single forward pass: no recursion, nothing allocated past the chunk's size.
bool info_name(std::string_view xml, std::string& name);

// The bytes of `path`'s one 'Info' chunk, read through the header and the
// chunk list alone (never the whole file). False, with the reason, when
// there is none, two, or one outside the data or over kMaxInfoBytes.
bool load_info(const std::string& path, std::string& info, std::string& error);

// The VST3 preset roots the SDK documents for Linux, in this order:
// $HOME/.vst3/presets, /usr/share/vst3/presets, /usr/local/share/vst3/presets.
std::vector<std::string> default_roots();

struct Listed {
    std::string relative;  // to its root, '/'-separated: the id after "vst3-file:"
    // The Info chunk's Name; the file name without ".vstpreset" when the
    // file has no usable one. A bad Info never drops the preset.
    std::string name;
};

// Every .vstpreset under <root>/<vendor>/<plugin name>/ (subfolders, four
// deep, are the SDK's categories) whose header names `plugin_id`, at most
// `cap` in all, a relative path seen under an earlier root shadowing a
// later one. Symlinks are not followed out of the folder, at most 16384
// entries are walked per root, and a vendor or name that is not one path
// component lists nothing.
std::vector<Listed> list(const std::vector<std::string>& roots, const std::string& vendor,
                         const std::string& plugin_name, const std::string& plugin_id, std::size_t cap);

// The file a "vst3-file:" id names: the first root under which `relative`
// is a regular .vstpreset whose real path stays inside that root. False,
// with the reason, for an absolute path, a ".." or an empty component, or
// one found under no root.
bool resolve(const std::vector<std::string>& roots, const std::string& relative, std::string& path,
             std::string& error);

}  // namespace sonora::plugin_host::vstpreset
