#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>

namespace minpatricia {

using Position = std::uint64_t;

inline constexpr std::size_t kNodeSize = 4096;
inline constexpr std::size_t kMaxNodeReps = 339;
inline constexpr std::uint64_t kChildTag = std::uint64_t{1} << 63;
inline constexpr std::uint16_t kMaxDiff = std::numeric_limits<std::uint16_t>::max();
inline constexpr std::size_t kMaxKeySize = kMaxDiff / 9;

}  // namespace minpatricia
