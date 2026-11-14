// SPDX-License-Identifier: GPL-3.0-or-later
//
// The beat pass's onset stage (ADR-0114): a log-compressed spectral flux
// (Bello et al., "A tutorial on onset detection in music signals", IEEE
// Trans. Speech Audio Process. 13(5), 2005, §III) over a Hann STFT of the
// decimated mono signal, then an adaptive threshold (the flux minus its local
// mean, scaled by the strong peaks) and peak picking. Offline; it allocates.
#pragma once

#include <atomic>
#include <cstdint>
#include <vector>

namespace sonora::analyze::beat {

constexpr double kRate = 11025.0;  // analysis rate, as the key pass's
constexpr int kWindow = 512;       // 46 ms: onsets stay sharp in time
constexpr int kHop = 128;          // 11.6 ms, 86.1 frames per second
constexpr double kFrameRate = kRate / kHop;

// One flux value per hop (flux[0] is 0); false when `cancel` was set.
bool spectral_flux(const std::vector<float>& signal, const std::atomic<bool>* cancel, std::vector<float>& flux);

// Flux minus its local mean (about 0.9 s wide), half-wave rectified and
// divided by the mean of its top 5 % so that 1 is a strong onset whatever the
// level. False when nothing rises above the mean (silence, a constant tone).
bool adaptive_onset(const std::vector<float>& flux, std::vector<float>& onset);

// Frames that are the maximum within +-3 frames and above 0.25.
std::vector<int> pick_peaks(const std::vector<float>& onset);

}  // namespace sonora::analyze::beat
