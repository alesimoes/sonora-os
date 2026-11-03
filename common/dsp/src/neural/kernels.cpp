// SPDX-License-Identifier: GPL-3.0-or-later
//
// The generic kernel table (kernels.h): plain C++, vectorised by the
// compiler at the baseline width.
#include "kernels.h"

#include "sonora/rt.h"
#include "vec_math.h"

namespace sonora::dsp::neural::detail {

namespace {

// Four output rows share each pass over an input row, so the inner loop
// (vectorised over t) loads the input once per four multiply-adds. On the
// "standard" convolution (16 x 48 x 128) this was the fastest of four forms
// tried in the build image with GCC 14 -O3: 21 us against 34 us one row at
// a time and 63 us for 4 x 8 register tiles, which GCC did not vectorise.
// Each sum starts at 0 and adds its terms in input order, so the grouping
// changes no bit of the result.
void matmul_rows(const float* w, int rows, int nin, const float* const* in, int n, float* out,
                 std::size_t out_stride) noexcept SONORA_RT {
    const std::size_t stride_w = static_cast<std::size_t>(nin);
    int o0 = 0;
    for (; o0 + 4 <= rows; o0 += 4) {
        const float* w0 = w + static_cast<std::size_t>(o0) * stride_w;
        float* r0 = out + static_cast<std::size_t>(o0) * out_stride;
        float* r1 = r0 + out_stride;
        float* r2 = r1 + out_stride;
        float* r3 = r2 + out_stride;
        for (int t = 0; t < n; ++t) {
            r0[t] = 0.0f;
            r1[t] = 0.0f;
            r2[t] = 0.0f;
            r3[t] = 0.0f;
        }
        for (int i = 0; i < nin; ++i) {
            const float c0 = w0[i];
            const float c1 = w0[stride_w + static_cast<std::size_t>(i)];
            const float c2 = w0[2 * stride_w + static_cast<std::size_t>(i)];
            const float c3 = w0[3 * stride_w + static_cast<std::size_t>(i)];
            const float* x = in[i];
            for (int t = 0; t < n; ++t) {
                const float v = x[t];
                r0[t] += c0 * v;
                r1[t] += c1 * v;
                r2[t] += c2 * v;
                r3[t] += c3 * v;
            }
        }
    }
    for (; o0 < rows; ++o0) {
        const float* wr = w + static_cast<std::size_t>(o0) * stride_w;
        float* r = out + static_cast<std::size_t>(o0) * out_stride;
        for (int t = 0; t < n; ++t) r[t] = 0.0f;
        for (int i = 0; i < nin; ++i) {
            const float c = wr[i];
            const float* x = in[i];
            for (int t = 0; t < n; ++t) r[t] += c * x[t];
        }
    }
}

void tanh_rows(float* x, int n) noexcept SONORA_RT {
    for (int i = 0; i < n; ++i) x[i] = tanh_approx(x[i]);
}

void sigmoid_rows(float* x, int n) noexcept SONORA_RT {
    for (int i = 0; i < n; ++i) x[i] = sigmoid_approx(x[i]);
}

const Kernels kGeneric = {"generic", matmul_rows, tanh_rows, sigmoid_rows};

}  // namespace

const Kernels& generic_kernels() noexcept { return kGeneric; }

}  // namespace sonora::dsp::neural::detail
