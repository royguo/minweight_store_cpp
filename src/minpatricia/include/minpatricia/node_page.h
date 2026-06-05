#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>

#include "minpatricia/byte_view.h"
#include "minpatricia/position.h"
#include "minpatricia/status.h"

namespace minpatricia {

class Rep {
 public:
  constexpr Rep() = default;
  constexpr explicit Rep(std::uint64_t bits) : bits_(bits) {}

  static constexpr Result<Rep> MakeRecord(Position pos) {
    if ((pos & kChildTag) != 0) {
      return Status(StatusCode::kPositionTag);
    }
    return Rep(pos);
  }

  static constexpr Result<Rep> MakeChild(std::uint64_t id) {
    if ((id & kChildTag) != 0) {
      return Status(StatusCode::kPositionTag);
    }
    return Rep(kChildTag | id);
  }

  [[nodiscard]] constexpr bool is_child() const { return (bits_ & kChildTag) != 0; }
  [[nodiscard]] constexpr Position position() const { return bits_ & ~kChildTag; }
  [[nodiscard]] constexpr std::uint64_t child_id() const { return bits_ & ~kChildTag; }
  [[nodiscard]] constexpr std::uint64_t bits() const { return bits_; }

  constexpr void clear() { bits_ = 0; }

 private:
  std::uint64_t bits_ = 0;
};

struct RouteFrame {
  int node = 0;
  int leaf_l = 0;
  int leaf_r = 0;
};

struct NodePage;

class Route {
 public:
  static constexpr std::uint32_t kByteMask = (std::uint32_t{1} << 13) - 1;
  static constexpr std::uint32_t kBitOpOffset = 13;
  static constexpr std::uint32_t kBitOpMask = 0xF;
  static constexpr std::uint32_t kTermBitOp = 8;
  static constexpr std::uint32_t kLeftCountOffset = 17;

  constexpr Route() = default;
  constexpr explicit Route(std::uint32_t bits) : bits(bits) {}

  static constexpr Route Make(std::uint16_t diff, std::uint16_t left_count) {
    const std::uint32_t d = diff;
    const std::uint32_t byte_idx = d / 9;
    std::uint32_t bit_op = kTermBitOp;
    if (const std::uint32_t bit_idx = d % 9; bit_idx != 0) {
      bit_op = 8 - bit_idx;
    }
    return Route(byte_idx | (bit_op << kBitOpOffset) |
                 (std::uint32_t{left_count} << kLeftCountOffset));
  }

  [[nodiscard]] constexpr std::uint16_t diff() const {
    const std::uint32_t byte_idx = bits & kByteMask;
    const std::uint32_t bit_op = (bits >> kBitOpOffset) & kBitOpMask;
    if (bit_op == kTermBitOp) {
      return static_cast<std::uint16_t>(byte_idx * 9);
    }
    return static_cast<std::uint16_t>(byte_idx * 9 + 8 - bit_op);
  }

  [[nodiscard]] constexpr std::uint16_t left_count() const {
    return static_cast<std::uint16_t>(bits >> kLeftCountOffset);
  }

  constexpr void inc_left_count() { bits += std::uint32_t{1} << kLeftCountOffset; }
  constexpr void dec_left_count() { bits -= std::uint32_t{1} << kLeftCountOffset; }

  [[nodiscard]] std::uint8_t bit(ByteView key) const {
    const std::size_t byte_idx = bits & kByteMask;
    if (byte_idx >= key.size()) {
      return 0;
    }
    const auto byte = std::to_integer<std::uint8_t>(key[byte_idx]);
    const auto bit_op = static_cast<std::uint8_t>((bits >> kBitOpOffset) & kBitOpMask);
    return static_cast<std::uint8_t>((bit_op >> 3) | ((byte >> (bit_op & 7)) & 1));
  }

  std::uint32_t bits = 0;
};

struct PutFrame {
  std::uint64_t id = 0;
  NodePage* node = nullptr;
  int leaf = 0;
};

struct NodePage {
  std::uint16_t size = 0;
  std::byte reserved0[6]{};
  Position first_pos = 0;
  Position last_pos = 0;
  std::array<Route, kMaxNodeReps - 1> routes{};
  std::array<Rep, kMaxNodeReps> reps{};
  std::byte reserved1[8]{};

  [[nodiscard]] std::pair<int, bool> Lookup(ByteView key) const {
    return LookupRouteOnly(key);
  }

  [[nodiscard]] std::pair<int, bool> LookupRouteOnly(ByteView key) const {
    const int page_size = static_cast<int>(size);
    if (page_size == 0) {
      return {0, false};
    }

    int route_idx = 0;
    int leaf_base = 0;
    int leaf_count = page_size;
    while (leaf_count > 1) {
      const Route route = routes[route_idx];
      const int left_count = route.left_count();
      if (route.bit(key) == 0) {
        route_idx++;
        leaf_count = left_count;
      } else {
        route_idx += left_count;
        leaf_base += left_count;
        leaf_count -= left_count;
      }
    }
    return {leaf_base, true};
  }

  Status RouteDiffs(std::span<std::uint16_t> out) const {
    const int page_size = static_cast<int>(size);
    if (static_cast<int>(out.size()) != page_size - 1) {
      return Status(StatusCode::kCorruptLayout);
    }
    if (page_size <= 1) {
      return OkStatus();
    }

    std::array<RouteFrame, kMaxNodeReps> stack{};
    int stack_size = 1;
    stack[0] = RouteFrame{0, 0, page_size};
    while (stack_size > 0) {
      const RouteFrame frame = stack[--stack_size];
      if (frame.node < 0 || frame.node >= page_size - 1) {
        return Status(StatusCode::kCorruptLayout);
      }
      const Route route = routes[frame.node];
      const int left_count = route.left_count();
      const int diff_idx = frame.leaf_l + left_count - 1;
      if (left_count <= 0 || diff_idx < frame.leaf_l || diff_idx >= frame.leaf_r - 1) {
        return Status(StatusCode::kCorruptLayout);
      }
      out[diff_idx] = route.diff();

      const int right_idx = frame.node + left_count;
      if (diff_idx + 1 < frame.leaf_r - 1) {
        stack[stack_size++] = RouteFrame{right_idx, diff_idx + 1, frame.leaf_r};
      }
      if (frame.leaf_l < diff_idx) {
        stack[stack_size++] = RouteFrame{frame.node + 1, frame.leaf_l, diff_idx + 1};
      }
    }
    return OkStatus();
  }

  Result<std::pair<int, bool>> InsertSlotAbovePath(ByteView key, std::uint16_t diff,
                                                   int want_leaf) const;
  Status InsertRoute(int slot, ByteView key, std::uint16_t diff);
  Status DeleteRoute(int slot);
  [[nodiscard]] bool IsDirectRoutePair(int diff_idx) const;

 private:
  void InsertRouteAt(int route_idx, std::uint16_t diff, std::uint16_t left_count,
                     std::span<const std::uint16_t> left_ancestors);
  void DeleteRouteAt(int route_idx, std::span<const std::uint16_t> left_ancestors);
};

static_assert(sizeof(Rep) == 8);
static_assert(sizeof(Route) == 4);
static_assert(sizeof(NodePage) == kNodeSize);
static_assert(alignof(NodePage) == 8);
static_assert(offsetof(NodePage, first_pos) == 8);
static_assert(offsetof(NodePage, last_pos) == 16);
static_assert(offsetof(NodePage, routes) == 24);
static_assert(offsetof(NodePage, reps) == 1376);

constexpr int AlignUp(int value, int align) {
  return (value + align - 1) & ~(align - 1);
}

constexpr int LayoutBytes(int rep_count) {
  if (rep_count <= 0) {
    return 24;
  }
  return AlignUp(24 + (rep_count - 1) * 4, 8) + rep_count * 8;
}

inline std::uint8_t GetDiffBit(ByteView key, std::uint16_t diff) {
  const std::size_t byte_idx = diff / 9;
  const unsigned bit_idx = diff % 9;
  if (bit_idx == 0) {
    return byte_idx < key.size() ? 1 : 0;
  }
  if (byte_idx >= key.size()) {
    return 0;
  }
  const auto byte = std::to_integer<std::uint8_t>(key[byte_idx]);
  return static_cast<std::uint8_t>((byte >> (8 - bit_idx)) & 1);
}

inline Result<std::pair<int, bool>> NodePage::InsertSlotAbovePath(ByteView key,
                                                                  std::uint16_t diff,
                                                                  int want_leaf) const {
  const int page_size = static_cast<int>(size);
  if (page_size == 0) {
    return std::pair<int, bool>{0, false};
  }
  if (want_leaf < 0 || want_leaf >= page_size) {
    return Status(StatusCode::kCorruptLayout);
  }

  int route_idx = 0;
  int leaf_base = 0;
  int leaf_count = page_size;
  while (leaf_count > 1) {
    if (route_idx < 0 || route_idx >= page_size - 1) {
      return Status(StatusCode::kCorruptLayout);
    }
    const Route route = routes[route_idx];
    const int left_count = route.left_count();
    if (left_count <= 0 || left_count >= leaf_count) {
      return Status(StatusCode::kCorruptLayout);
    }
    if (diff < route.diff()) {
      if (GetDiffBit(key, diff) == 0) {
        return std::pair<int, bool>{leaf_base, true};
      }
      return std::pair<int, bool>{leaf_base + leaf_count, true};
    }

    if (route.bit(key) == 0) {
      route_idx++;
      leaf_count = left_count;
    } else {
      route_idx += left_count;
      leaf_base += left_count;
      leaf_count -= left_count;
    }
  }

  if (leaf_base != want_leaf) {
    return Status(StatusCode::kCorruptLayout);
  }
  return std::pair<int, bool>{0, false};
}

inline Status NodePage::InsertRoute(int slot, ByteView key, std::uint16_t diff) {
  const int page_size = static_cast<int>(size);
  if (page_size <= 0 || page_size >= static_cast<int>(kMaxNodeReps)) {
    return Status(StatusCode::kCorruptLayout);
  }

  std::array<std::uint16_t, kMaxNodeReps> left_ancestors{};
  int left_ancestor_count = 0;
  int route_idx = 0;
  int leaf_base = 0;
  int leaf_count = page_size;
  while (leaf_count > 1) {
    if (route_idx < 0 || route_idx >= page_size - 1) {
      return Status(StatusCode::kCorruptLayout);
    }
    const Route route = routes[route_idx];
    const int left_count = route.left_count();
    if (left_count <= 0 || left_count >= leaf_count) {
      return Status(StatusCode::kCorruptLayout);
    }

    if (diff < route.diff()) {
      std::uint16_t new_left_count = 1;
      int expected_slot = leaf_base;
      if (GetDiffBit(key, diff) != 0) {
        new_left_count = static_cast<std::uint16_t>(leaf_count);
        expected_slot = leaf_base + leaf_count;
      }
      if (slot != expected_slot) {
        return Status(StatusCode::kCorruptLayout);
      }
      InsertRouteAt(route_idx, diff, new_left_count,
                    std::span<const std::uint16_t>(left_ancestors.data(), left_ancestor_count));
      return OkStatus();
    }

    if (route.bit(key) == 0) {
      left_ancestors[left_ancestor_count++] = static_cast<std::uint16_t>(route_idx);
      route_idx++;
      leaf_count = left_count;
    } else {
      route_idx += left_count;
      leaf_base += left_count;
      leaf_count -= left_count;
    }
  }

  int expected_slot = leaf_base;
  if (GetDiffBit(key, diff) != 0) {
    expected_slot = leaf_base + 1;
  }
  if (slot != expected_slot) {
    return Status(StatusCode::kCorruptLayout);
  }
  InsertRouteAt(route_idx, diff, 1,
                std::span<const std::uint16_t>(left_ancestors.data(), left_ancestor_count));
  return OkStatus();
}

inline void NodePage::InsertRouteAt(int route_idx, std::uint16_t diff, std::uint16_t left_count,
                                    std::span<const std::uint16_t> left_ancestors) {
  const int page_size = static_cast<int>(size);
  for (int i = page_size - 1; i > route_idx; --i) {
    routes[i] = routes[i - 1];
  }
  routes[route_idx] = Route::Make(diff, left_count);
  for (const auto ancestor : left_ancestors) {
    routes[ancestor].inc_left_count();
  }
}

inline Status NodePage::DeleteRoute(int slot) {
  const int page_size = static_cast<int>(size);
  if (page_size <= 1 || slot < 0 || slot >= page_size) {
    return Status(StatusCode::kCorruptLayout);
  }

  std::array<std::uint16_t, kMaxNodeReps> left_ancestors{};
  int left_ancestor_count = 0;
  int route_idx = 0;
  int leaf_base = 0;
  int leaf_count = page_size;
  int parent_route_idx = -1;

  while (leaf_count > 1) {
    if (route_idx < 0 || route_idx >= page_size - 1) {
      return Status(StatusCode::kCorruptLayout);
    }
    const Route route = routes[route_idx];
    const int left_count = route.left_count();
    if (left_count <= 0 || left_count >= leaf_count) {
      return Status(StatusCode::kCorruptLayout);
    }

    parent_route_idx = route_idx;
    if (slot < leaf_base + left_count) {
      if (left_count > 1) {
        left_ancestors[left_ancestor_count++] = static_cast<std::uint16_t>(route_idx);
      }
      route_idx++;
      leaf_count = left_count;
    } else {
      route_idx += left_count;
      leaf_base += left_count;
      leaf_count -= left_count;
    }
  }

  if (leaf_base != slot || parent_route_idx < 0) {
    return Status(StatusCode::kCorruptLayout);
  }
  DeleteRouteAt(parent_route_idx,
                std::span<const std::uint16_t>(left_ancestors.data(), left_ancestor_count));
  return OkStatus();
}

inline void NodePage::DeleteRouteAt(int route_idx,
                                    std::span<const std::uint16_t> left_ancestors) {
  const int page_size = static_cast<int>(size);
  for (int i = route_idx; i < page_size - 2; ++i) {
    routes[i] = routes[i + 1];
  }
  routes[page_size - 2] = Route{};
  for (const auto ancestor : left_ancestors) {
    routes[ancestor].dec_left_count();
  }
}

inline bool NodePage::IsDirectRoutePair(int diff_idx) const {
  const int page_size = static_cast<int>(size);
  if (diff_idx < 0 || diff_idx >= page_size - 1) {
    return false;
  }

  std::array<RouteFrame, kMaxNodeReps> stack{};
  int stack_size = 1;
  stack[0] = RouteFrame{0, 0, page_size};
  while (stack_size > 0) {
    const RouteFrame frame = stack[--stack_size];
    const Route route = routes[frame.node];
    const int left_count = route.left_count();
    const int current_diff = frame.leaf_l + left_count - 1;
    if (current_diff == diff_idx) {
      return frame.leaf_l == diff_idx && frame.leaf_r == diff_idx + 2;
    }
    const int right_idx = frame.node + left_count;
    if (current_diff + 1 < frame.leaf_r - 1) {
      stack[stack_size++] = RouteFrame{right_idx, current_diff + 1, frame.leaf_r};
    }
    if (frame.leaf_l < current_diff) {
      stack[stack_size++] = RouteFrame{frame.node + 1, frame.leaf_l, current_diff + 1};
    }
  }
  return false;
}

}  // namespace minpatricia
