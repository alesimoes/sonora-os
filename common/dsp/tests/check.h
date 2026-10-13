// SPDX-License-Identifier: GPL-3.0-or-later
//
// The tests' one assertion. Prints and fails the test function; the
// driver counts failures so one run reports every broken case.
#pragma once

#include <cstdio>

#define CHECK(cond)                                                      \
    do {                                                                 \
        if (!(cond)) {                                                   \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, \
                         #cond);                                         \
            return 1;                                                    \
        }                                                                \
    } while (0)

// Like CHECK, with the measured value and the bar in the message.
#define CHECK_LE(value, bar, what)                                                  \
    do {                                                                            \
        const double v_ = (value), b_ = (bar);                                      \
        if (!(v_ <= b_)) {                                                          \
            std::fprintf(stderr, "FAIL %s:%d: %s = %.6g, bar <= %.6g\n", __FILE__, \
                         __LINE__, what, v_, b_);                                   \
            return 1;                                                               \
        }                                                                           \
    } while (0)
