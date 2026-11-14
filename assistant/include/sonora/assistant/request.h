// SPDX-License-Identifier: GPL-3.0-or-later
//
// Turning a FeatureBundle into the bytes that leave the machine (ADR-0155
// §3). The only input is the bundle: build_request takes no sample buffer,
// no snapshot and no key, so the request can carry neither audio nor the
// operator's credential. The key travels beside it, to the transport.
#pragma once

#include <cstddef>
#include <string>

#include "sonora/assistant/feature_bundle.h"

namespace sonora::assistant {

struct Request {
    // The JSON body, complete. Logged only through its size.
    std::string body;
};

// False, with `error`, when the body would exceed `max_bytes` even after
// dropping per-band spectra and insert names (in that order, named in the
// error). A bundle is never silently truncated to a different session: a
// request either describes every track or is not sent (Unavailable::TooLarge).
bool build_request(const FeatureBundle& bundle, std::size_t max_bytes, Request& out,
                   std::string& error);

}  // namespace sonora::assistant
