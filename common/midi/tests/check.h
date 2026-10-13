// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdio>

// The tree's CHECK pattern, shared by this module's tests. Each case is a
// function returning int, so a failure ends that case and main stops there.
#define CHECK(cond)                                                      \
    do {                                                                 \
        if (!(cond)) {                                                   \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, \
                         #cond);                                         \
            return 1;                                                    \
        }                                                                \
    } while (0)

#define RUN(test)                                     \
    do {                                              \
        if (const int rc = test()) {                  \
            std::fprintf(stderr, "  in %s\n", #test); \
            return rc;                                \
        }                                             \
    } while (0)
