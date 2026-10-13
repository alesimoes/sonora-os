// SPDX-License-Identifier: GPL-3.0-or-later
// Internal to the POSIX implementation: how process.cpp finds the descriptor
// of a region it passes to a child, without SharedRegion's Impl leaving
// ipc.cpp.
#pragma once

#include "sonora/platform/ipc.h"

namespace sonora::platform {

// The read-write memfd of a create_shared_writable region when `writable`,
// else the read-only descriptor of a create()d one; -1 when the region has
// no such descriptor.
int region_descriptor(const SharedRegion& region, bool writable);

}  // namespace sonora::platform
