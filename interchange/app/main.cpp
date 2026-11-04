// SPDX-License-Identifier: GPL-3.0-or-later
#include <cstdio>
#include <exception>
#include <string>
#include <vector>

#include "sonora/interchange/interchange.h"

int main(int argc, char** argv) {
    std::vector<std::string> args(argv + 1, argv + argc);
    std::string out, err;
    int code = 1;
    // A malformed input must end this process with a message, not take a
    // terminate with it; the process exists to absorb exactly that.
    try {
        code = sonora::interchange::run_convert(args, out, err);
    } catch (const std::exception& e) {
        err += std::string("sonora-convert: ") + e.what() + "\n";
    }
    std::fputs(out.c_str(), stdout);
    std::fputs(err.c_str(), stderr);
    return code;
}
