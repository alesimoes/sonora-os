// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/varispeed_command.h"

#include "sonora/api/varispeed.h"
#include "sonora/session.h"

namespace sonora {

bool apply_varispeed(Session& session, const std::string& argument, std::string& why) {
    double ratio = 1.0;
    if (!api::parse_varispeed(argument, ratio, why)) return false;
    return session.set_varispeed(static_cast<float>(ratio), &why);
}

}  // namespace sonora
