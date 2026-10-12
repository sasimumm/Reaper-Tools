// Indices into std::vector and other size_t-indexed arrays, from the int counts and positions the
// code works in. Each operand is converted before the arithmetic, as an implicit conversion would.
#pragma once

#include <concepts>
#include <cstddef>

inline size_t idx(std::integral auto i) { return static_cast<size_t>(i); }

// row * width + col, for a row-major array of rows width long.
inline size_t idx(std::integral auto row, std::integral auto width, std::integral auto col) {
    return idx(row) * idx(width) + idx(col);
}
