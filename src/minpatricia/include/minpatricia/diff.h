#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>

#include "minpatricia/byte_view.h"
#include "minpatricia/position.h"
#include "minpatricia/status.h"

namespace minpatricia {

struct CompareDiff {
  int compare = 0;
  std::uint16_t diff = 0;
};

inline Status CheckKeySize(ByteView key) {
  if (key.size() > kMaxKeySize) {
    return Status(StatusCode::kKeyTooLarge);
  }
  return OkStatus();
}

inline int CompareKeys(ByteView a, ByteView b) {
  if (a.data() == b.data()) {
    if (a.size() < b.size()) {
      return -1;
    }
    if (a.size() > b.size()) {
      return 1;
    }
    return 0;
  }
  const std::size_t n = a.size() < b.size() ? a.size() : b.size();
  if (n != 0) {
    const int cmp = std::memcmp(a.data(), b.data(), n);
    if (cmp < 0) {
      return -1;
    }
    if (cmp > 0) {
      return 1;
    }
  }
  if (a.size() < b.size()) {
    return -1;
  }
  if (a.size() > b.size()) {
    return 1;
  }
  return 0;
}

inline Result<CompareDiff> CompareAndDiffBit(ByteView a, ByteView b) {
  if (const Status status = CheckKeySize(a); !status.ok()) {
    return status;
  }
  if (const Status status = CheckKeySize(b); !status.ok()) {
    return status;
  }

  const std::size_t n = a.size() < b.size() ? a.size() : b.size();
  std::size_t i = 0;
  while (i < n && a[i] == b[i]) {
    ++i;
  }

  if (i == n) {
    if (a.size() == b.size()) {
      return Status(StatusCode::kEqualKeys);
    }
    const auto diff = static_cast<std::uint16_t>(i * 9);
    if (a.size() < b.size()) {
      return CompareDiff{-1, diff};
    }
    return CompareDiff{1, diff};
  }

  const auto ax = std::to_integer<std::uint8_t>(a[i]);
  const auto bx = std::to_integer<std::uint8_t>(b[i]);
  const auto x = static_cast<unsigned>(ax ^ bx);
  const auto leading = static_cast<unsigned>(
      std::countl_zero(x) - (std::numeric_limits<unsigned>::digits - 8));
  const auto diff = static_cast<std::uint16_t>(i * 9 + leading + 1);
  if (ax < bx) {
    return CompareDiff{-1, diff};
  }
  return CompareDiff{1, diff};
}

inline Result<std::uint16_t> FindDiffBit(ByteView a, ByteView b) {
  auto diff = CompareAndDiffBit(a, b);
  if (!diff.ok()) {
    return diff.status();
  }
  return diff.value().diff;
}

}  // namespace minpatricia
