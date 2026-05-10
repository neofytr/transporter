// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <cmath>
#include <complex>
#include <cstddef>
#include <numbers>

namespace transporter::gui {

// Radix-2 Cooley-Tukey FFT in-place. N must be a power of two.
template<std::size_t N>
void fft(std::array<std::complex<float>, N>& x) {
    // Bit-reversal permutation
    for (std::size_t i = 1, j = 0; i < N; ++i) {
        std::size_t bit = N >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(x[i], x[j]);
    }
    // Cooley-Tukey iterative FFT
    for (std::size_t len = 2; len <= N; len <<= 1) {
        const float ang = -2.0f * std::numbers::pi_v<float> / static_cast<float>(len);
        const std::complex<float> wlen{std::cos(ang), std::sin(ang)};
        for (std::size_t i = 0; i < N; i += len) {
            std::complex<float> w{1.0f, 0.0f};
            for (std::size_t j = 0; j < len / 2; ++j) {
                const auto u = x[i + j];
                const auto v = x[i + j + len/2] * w;
                x[i + j]         = u + v;
                x[i + j + len/2] = u - v;
                w *= wlen;
            }
        }
    }
}

// Hann window applied in-place to N float samples.
template<std::size_t N>
void apply_hann(std::array<float, N>& s) {
    for (std::size_t i = 0; i < N; ++i) {
        const float w = 0.5f * (1.0f - std::cos(
            2.0f * std::numbers::pi_v<float> * static_cast<float>(i) / static_cast<float>(N - 1)));
        s[i] *= w;
    }
}

} // namespace transporter::gui
