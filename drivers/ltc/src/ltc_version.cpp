// SPDX-License-Identifier: GPL-3.0-or-later
// Keeps sonora_ltc a real library before the codec and driver sources land
// (plan drivers.sync.ltc §3): a static library with no object is refused by
// some generators.
namespace sonora::ltc {

extern const int kLtcModuleVersion;
const int kLtcModuleVersion = 1;

}  // namespace sonora::ltc
