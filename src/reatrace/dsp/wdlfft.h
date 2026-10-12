// WDL's FFT, with its permutation table as a span.
#pragma once

#include <span>

#include "fft.h"
#include "index.h"

// WDL's permutation table for an FFT of n complex points: n entries.
inline std::span<const int> permuteTab(int n) {
    // WDL hands out its static table as a bare pointer.
#pragma clang unsafe_buffer_usage begin
    return std::span(WDL_fft_permute_tab(n), idx(n));
#pragma clang unsafe_buffer_usage end
}
