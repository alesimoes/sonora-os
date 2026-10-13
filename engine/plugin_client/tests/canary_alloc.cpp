// SPDX-License-Identifier: GPL-3.0-or-later
// Must FAIL tests/rt_reachability.cmake: an "audio thread" object that
// allocates. Never linked.
int* canary_alloc() { return new int(1); }
