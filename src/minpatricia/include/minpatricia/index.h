#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <type_traits>
#include <utility>
#include <vector>

#include "minpatricia/byte_view.h"
#include "minpatricia/diff.h"
#include "minpatricia/node_page.h"
#include "minpatricia/node_store.h"
#include "minpatricia/position.h"
#include "minpatricia/record_store.h"
#include "minpatricia/status.h"

namespace minpatricia {

struct ProbeResult {
  Position pos = 0;
  bool found = false;
};

struct PutResult {
  Position old_pos = 0;
  bool replaced = false;
};

struct DeleteResult {
  Position pos = 0;
  bool deleted = false;
};

namespace detail {

struct RebuildScratch {
  std::vector<std::uint16_t> diffs;
  std::vector<int> left;
  std::vector<int> right;
  std::vector<int> cart_stack;
  std::vector<RouteFrame> route_stack;
};

struct Cartesian {
  Span<int> left;
  Span<int> right;
  int root = 0;
};

struct SplitRange {
  int start = 0;
  int count = 0;
};

struct InsertSlot {
  int target = 0;
  int slot = 0;
};

struct IterBound {
  ByteView key;
  bool ok = false;
};

struct IterItem {
  ByteView key;
  Position pos = 0;
};

constexpr int kIterStackDepth = 16;

struct IterPath {
  std::array<PutFrame, kIterStackDepth> stack{};
  std::vector<PutFrame> overflow;
  int len = 0;

  void Reset() {
    len = 0;
    overflow.clear();
  }

  void Push(std::uint64_t id, int leaf) { PushNode(id, nullptr, leaf); }

  void PushNode(std::uint64_t id, NodePage* node, int leaf) {
    PutFrame frame{id, node, leaf};
    if (len < static_cast<int>(stack.size())) {
      stack[static_cast<std::size_t>(len)] = frame;
    } else {
      overflow.push_back(frame);
    }
    ++len;
  }

  PutFrame* At(int index) {
    if (index < static_cast<int>(stack.size())) {
      return &stack[static_cast<std::size_t>(index)];
    }
    return &overflow[static_cast<std::size_t>(index - static_cast<int>(stack.size()))];
  }

  const PutFrame* At(int index) const {
    if (index < static_cast<int>(stack.size())) {
      return &stack[static_cast<std::size_t>(index)];
    }
    return &overflow[static_cast<std::size_t>(index - static_cast<int>(stack.size()))];
  }

  void Truncate(int new_len) {
    if (new_len <= static_cast<int>(stack.size())) {
      overflow.clear();
    } else {
      overflow.resize(static_cast<std::size_t>(new_len - static_cast<int>(stack.size())));
    }
    len = new_len;
  }

  std::vector<PutFrame> FramesForInsert() const {
    std::vector<PutFrame> frames;
    frames.reserve(static_cast<std::size_t>(len));
    const int stack_count = std::min(len, static_cast<int>(stack.size()));
    for (int i = 0; i < stack_count; ++i) {
      frames.push_back(stack[static_cast<std::size_t>(i)]);
    }
    for (int i = stack_count; i < len; ++i) {
      frames.push_back(
          overflow[static_cast<std::size_t>(i - static_cast<int>(stack.size()))]);
    }
    return frames;
  }
};

inline int AbsInt(int value) {
  return value < 0 ? -value : value;
}

}  // namespace detail

template <class RecordStore, class NodeStore = HeapNodeStore>
class Index {
 public:
  static_assert(IsRecordStoreLike<RecordStore>::value,
                "RecordStore must provide Result<ByteView> Key(Position)");
  static_assert(IsNodeStoreLike<NodeStore>::value,
                "NodeStore must provide Root/Get/Alloc/Free/LiveNodes with minpatricia types");

  Index() = default;

  static Result<Index> NewWithNodes(RecordStore& records, NodeStore& nodes) {
    return NewIndex(records, nodes, true);
  }

  static Result<Index> OpenWithNodes(RecordStore& records, NodeStore& nodes) {
    return NewIndex(records, nodes, false);
  }

  [[nodiscard]] int Len() const { return count_; }
  [[nodiscard]] int LiveNodes() const { return nodes_->LiveNodes(); }

  Result<ProbeResult> Probe(ByteView key) {
    if (const Status status = CheckKeySize(key); !status.ok()) {
      return status;
    }

    auto root_result = Root();
    if (!root_result.ok()) {
      return root_result.status();
    }
    NodePage* node = root_result.value();
    for (;;) {
      auto [leaf, ok] = node->Lookup(key);
      if (!ok) {
        return ProbeResult{0, false};
      }

      const Rep rep = node->reps[static_cast<std::size_t>(leaf)];
      if (rep.is_child()) {
        auto child = NodeByID(rep.child_id());
        if (!child.ok()) {
          return child.status();
        }
        node = child.value();
        continue;
      }
      return ProbeResult{rep.position(), true};
    }
  }

  Result<ProbeResult> Get(ByteView key) {
    auto probe = Probe(key);
    if (!probe.ok() || !probe.value().found) {
      return probe;
    }

    auto record_key = Key(probe.value().pos);
    if (!record_key.ok()) {
      return record_key.status();
    }
    if (CompareKeys(record_key.value(), key) != 0) {
      return ProbeResult{0, false};
    }
    return probe.value();
  }

  Result<PutResult> Put(ByteView key, Position pos) {
    if (const Status status = CheckKeySize(key); !status.ok()) {
      return status;
    }
    auto rep = Rep::MakeRecord(pos);
    if (!rep.ok()) {
      return rep.status();
    }
    auto result = InsertOrReplace(key, rep.value());
    if (!result.ok()) {
      return result.status();
    }
    if (!result.value().replaced) {
      ++count_;
    }
    return result.value();
  }

  Result<DeleteResult> Delete(ByteView key) {
    if (const Status status = CheckKeySize(key); !status.ok()) {
      return status;
    }
    auto result = DeleteFrom(root_id_, key);
    if (!result.ok()) {
      return result.status();
    }
    if (result.value().deleted) {
      --count_;
      if (const Status status = CompressRoot(); !status.ok()) {
        return status;
      }
    }
    return result.value();
  }

  Status Retarget(ByteView key, Position old_pos, Position new_pos) {
    if (const Status status = CheckKeySize(key); !status.ok()) {
      return status;
    }
    auto rep = Rep::MakeRecord(new_pos);
    if (!rep.ok()) {
      return rep.status();
    }
    return Retarget(key, old_pos, rep.value());
  }

  template <class Fn>
  Status Visit(Fn&& fn) {
    return Ascend(std::forward<Fn>(fn));
  }

  template <class Fn>
  Status Ascend(Fn&& fn) {
    detail::IterPath path;
    auto ok = LeftmostRoot(&path);
    if (!ok.ok() || !ok.value()) {
      return ok.ok() ? OkStatus() : ok.status();
    }
    return AscendFromPath(path, detail::IterBound{}, fn);
  }

  template <class Fn>
  Status AscendRange(ByteView greater_or_equal, ByteView less_than, Fn&& fn) {
    if (const Status status = CheckKeySize(greater_or_equal); !status.ok()) {
      return status;
    }
    if (const Status status = CheckKeySize(less_than); !status.ok()) {
      return status;
    }
    detail::IterPath path;
    auto ok = SeekGreaterOrEqual(greater_or_equal, &path);
    if (!ok.ok() || !ok.value()) {
      return ok.ok() ? OkStatus() : ok.status();
    }
    return AscendFromPath(path, detail::IterBound{less_than, true}, fn);
  }

  template <class Fn>
  Status AscendLessThan(ByteView pivot, Fn&& fn) {
    if (const Status status = CheckKeySize(pivot); !status.ok()) {
      return status;
    }
    detail::IterPath path;
    auto ok = LeftmostRoot(&path);
    if (!ok.ok() || !ok.value()) {
      return ok.ok() ? OkStatus() : ok.status();
    }
    return AscendFromPath(path, detail::IterBound{pivot, true}, fn);
  }

  template <class Fn>
  Status AscendGreaterOrEqual(ByteView pivot, Fn&& fn) {
    if (const Status status = CheckKeySize(pivot); !status.ok()) {
      return status;
    }
    detail::IterPath path;
    auto ok = SeekGreaterOrEqual(pivot, &path);
    if (!ok.ok() || !ok.value()) {
      return ok.ok() ? OkStatus() : ok.status();
    }
    return AscendFromPath(path, detail::IterBound{}, fn);
  }

  template <class Fn>
  Status Descend(Fn&& fn) {
    detail::IterPath path;
    auto ok = RightmostRoot(&path);
    if (!ok.ok() || !ok.value()) {
      return ok.ok() ? OkStatus() : ok.status();
    }
    return DescendFromPath(path, detail::IterBound{}, fn);
  }

  template <class Fn>
  Status DescendRange(ByteView less_or_equal, ByteView greater_than, Fn&& fn) {
    if (const Status status = CheckKeySize(less_or_equal); !status.ok()) {
      return status;
    }
    if (const Status status = CheckKeySize(greater_than); !status.ok()) {
      return status;
    }
    detail::IterPath path;
    auto ok = SeekLessOrEqual(less_or_equal, &path);
    if (!ok.ok() || !ok.value()) {
      return ok.ok() ? OkStatus() : ok.status();
    }
    return DescendFromPath(path, detail::IterBound{greater_than, true}, fn);
  }

  template <class Fn>
  Status DescendLessOrEqual(ByteView pivot, Fn&& fn) {
    if (const Status status = CheckKeySize(pivot); !status.ok()) {
      return status;
    }
    detail::IterPath path;
    auto ok = SeekLessOrEqual(pivot, &path);
    if (!ok.ok() || !ok.value()) {
      return ok.ok() ? OkStatus() : ok.status();
    }
    return DescendFromPath(path, detail::IterBound{}, fn);
  }

  template <class Fn>
  Status DescendGreaterThan(ByteView pivot, Fn&& fn) {
    if (const Status status = CheckKeySize(pivot); !status.ok()) {
      return status;
    }
    detail::IterPath path;
    auto ok = RightmostRoot(&path);
    if (!ok.ok() || !ok.value()) {
      return ok.ok() ? OkStatus() : ok.status();
    }
    return DescendFromPath(path, detail::IterBound{pivot, true}, fn);
  }

 private:
  static Result<Index> NewIndex(RecordStore& records, NodeStore& nodes, bool init_root) {
    Index index;
    index.records_ = &records;
    index.nodes_ = &nodes;
    index.root_id_ = nodes.Root();

    auto root = index.NodeByID(index.root_id_);
    if (!root.ok()) {
      return root.status();
    }
    if (init_root) {
      if (const Status status = index.RebuildNode(root.value()); !status.ok()) {
        return status;
      }
    }
    auto count = index.CountRecords(root.value());
    if (!count.ok()) {
      return count.status();
    }
    index.count_ = count.value();
    return index;
  }

  Result<NodePage*> Root() { return NodeByID(root_id_); }

  Result<NodePage*> NodeByID(std::uint64_t id) { return nodes_->Get(id); }

  Result<NodeAlloc> AllocNode() { return nodes_->Alloc(); }

  Status FreeNode(std::uint64_t id) { return nodes_->Free(id); }

  Result<ByteView> Key(Position pos) {
    auto key = records_->Key(pos);
    if (!key.ok()) {
      return key.status();
    }
    if (const Status status = CheckKeySize(key.value()); !status.ok()) {
      return status;
    }
    return key.value();
  }

  Result<int> CountRecords(NodePage* node) {
    int total = 0;
    const int size = static_cast<int>(node->size);
    for (int i = 0; i < size; ++i) {
      const Rep rep = node->reps[static_cast<std::size_t>(i)];
      if (!rep.is_child()) {
        ++total;
        continue;
      }
      auto child = NodeByID(rep.child_id());
      if (!child.ok()) {
        return child.status();
      }
      auto child_count = CountRecords(child.value());
      if (!child_count.ok()) {
        return child_count.status();
      }
      total += child_count.value();
    }
    return total;
  }

  Status RebuildNode(NodePage* node) {
    const int size = static_cast<int>(node->size);
    if (size == 0) {
      node->first_pos = 0;
      node->last_pos = 0;
      ClearRoutes(node);
      return OkStatus();
    }

    auto first_pos = MinPos(node->reps[0]);
    if (!first_pos.ok()) {
      return first_pos.status();
    }
    auto last_pos = MaxPos(node->reps[static_cast<std::size_t>(size - 1)]);
    if (!last_pos.ok()) {
      return last_pos.status();
    }
    node->first_pos = first_pos.value();
    node->last_pos = last_pos.value();

    if (size == 1) {
      ClearRoutes(node);
      return OkStatus();
    }

    auto diffs = BuildDiffs(Span<const Rep>(node->reps.data(), static_cast<std::size_t>(size)));
    if (!diffs.ok()) {
      return diffs.status();
    }
    return RebuildRoutesWithDiffs(node, diffs.value());
  }

  Status RebuildNodeWithDiffs(NodePage* node, Span<const std::uint16_t> diffs) {
    const int size = static_cast<int>(node->size);
    if (size == 0) {
      if (!diffs.empty()) {
        return Status(StatusCode::kCorruptLayout);
      }
      node->first_pos = 0;
      node->last_pos = 0;
      ClearRoutes(node);
      return OkStatus();
    }
    if (diffs.size() != static_cast<std::size_t>(size - 1)) {
      return Status(StatusCode::kCorruptLayout);
    }

    auto first_pos = MinPos(node->reps[0]);
    if (!first_pos.ok()) {
      return first_pos.status();
    }
    auto last_pos = MaxPos(node->reps[static_cast<std::size_t>(size - 1)]);
    if (!last_pos.ok()) {
      return last_pos.status();
    }
    node->first_pos = first_pos.value();
    node->last_pos = last_pos.value();

    if (size == 1) {
      ClearRoutes(node);
      return OkStatus();
    }
    return RebuildRoutesWithDiffs(node, diffs);
  }

  Status RebuildRoutesWithDiffs(NodePage* node, Span<const std::uint16_t> diffs) {
    const int size = static_cast<int>(node->size);
    auto cartesian = BuildCartesian(diffs);
    WritePreorderRoutes(
        Span<Route>(node->routes.data(), diffs.size()), diffs, cartesian.left,
        cartesian.right, cartesian.root, size);
    for (std::size_t i = diffs.size(); i < node->routes.size(); ++i) {
      node->routes[i] = Route{};
    }
    return OkStatus();
  }

  Result<Span<std::uint16_t>> BuildDiffs(Span<const Rep> reps) {
    if (reps.size() < 2) {
      scratch_.diffs.clear();
      return Span<std::uint16_t>{};
    }
    const std::size_t diff_count = reps.size() - 1;
    scratch_.diffs.resize(diff_count);
    for (std::size_t i = 0; i < diff_count; ++i) {
      auto diff = DiffBetweenReps(reps[i], reps[i + 1]);
      if (!diff.ok()) {
        return diff.status();
      }
      scratch_.diffs[i] = diff.value();
    }
    return Span<std::uint16_t>(scratch_.diffs.data(), scratch_.diffs.size());
  }

  Result<std::uint16_t> DiffBetweenReps(Rep left, Rep right) {
    auto left_pos = MaxPos(left);
    if (!left_pos.ok()) {
      return left_pos.status();
    }
    auto right_pos = MinPos(right);
    if (!right_pos.ok()) {
      return right_pos.status();
    }
    auto left_key = Key(left_pos.value());
    if (!left_key.ok()) {
      return left_key.status();
    }
    auto right_key = Key(right_pos.value());
    if (!right_key.ok()) {
      return right_key.status();
    }

    auto diff = CompareAndDiffBit(left_key.value(), right_key.value());
    if (!diff.ok()) {
      if (diff.status() == StatusCode::kEqualKeys) {
        return Status(StatusCode::kDuplicateKey);
      }
      return diff.status();
    }
    if (diff.value().compare > 0) {
      return Status(StatusCode::kUnsortedKeys);
    }
    return diff.value().diff;
  }

  void ClearRoutes(NodePage* node) {
    for (auto& route : node->routes) {
      route = Route{};
    }
  }

  Result<Position> MinPos(Rep rep) {
    if (!rep.is_child()) {
      return rep.position();
    }
    auto child = NodeByID(rep.child_id());
    if (!child.ok()) {
      return child.status();
    }
    if (child.value()->size == 0) {
      return Status(StatusCode::kCorruptLayout);
    }
    return child.value()->first_pos;
  }

  Result<Position> MaxPos(Rep rep) {
    if (!rep.is_child()) {
      return rep.position();
    }
    auto child = NodeByID(rep.child_id());
    if (!child.ok()) {
      return child.status();
    }
    if (child.value()->size == 0) {
      return Status(StatusCode::kCorruptLayout);
    }
    return child.value()->last_pos;
  }

  detail::Cartesian BuildCartesian(Span<const std::uint16_t> diffs) {
    const int n = static_cast<int>(diffs.size());
    scratch_.left.assign(static_cast<std::size_t>(n), -1);
    scratch_.right.assign(static_cast<std::size_t>(n), -1);
    scratch_.cart_stack.clear();
    scratch_.cart_stack.reserve(static_cast<std::size_t>(n));

    for (int i = 0; i < n; ++i) {
      int last = -1;
      while (!scratch_.cart_stack.empty() &&
             diffs[static_cast<std::size_t>(i)] <=
                 diffs[static_cast<std::size_t>(scratch_.cart_stack.back())]) {
        last = scratch_.cart_stack.back();
        scratch_.cart_stack.pop_back();
      }
      if (last != -1) {
        scratch_.left[static_cast<std::size_t>(i)] = last;
      }
      if (!scratch_.cart_stack.empty()) {
        scratch_.right[static_cast<std::size_t>(scratch_.cart_stack.back())] = i;
      }
      scratch_.cart_stack.push_back(i);
    }
    const int root = scratch_.cart_stack.front();
    scratch_.cart_stack.clear();
    return detail::Cartesian{
        Span<int>(scratch_.left.data(), scratch_.left.size()),
        Span<int>(scratch_.right.data(), scratch_.right.size()), root};
  }

  void WritePreorderRoutes(Span<Route> out, Span<const std::uint16_t> diffs,
                           Span<const int> left, Span<const int> right, int root,
                           int size) {
    scratch_.route_stack.clear();
    scratch_.route_stack.reserve(static_cast<std::size_t>(size));
    scratch_.route_stack.push_back(RouteFrame{root, 0, size});
    int out_idx = 0;

    while (!scratch_.route_stack.empty()) {
      const RouteFrame frame = scratch_.route_stack.back();
      scratch_.route_stack.pop_back();

      const int i = frame.node;
      out[static_cast<std::size_t>(out_idx)] =
          Route::Make(diffs[static_cast<std::size_t>(i)],
                      static_cast<std::uint16_t>(i - frame.leaf_l + 1));
      ++out_idx;

      if (right[static_cast<std::size_t>(i)] != -1) {
        scratch_.route_stack.push_back(
            RouteFrame{right[static_cast<std::size_t>(i)], i + 1, frame.leaf_r});
      }
      if (left[static_cast<std::size_t>(i)] != -1) {
        scratch_.route_stack.push_back(
            RouteFrame{left[static_cast<std::size_t>(i)], frame.leaf_l, i + 1});
      }
    }
  }

  Result<PutResult> InsertOrReplace(ByteView key, Rep new_rep) {
    std::vector<PutFrame> frames;
    frames.reserve(16);
    std::uint64_t id = root_id_;

    for (;;) {
      auto node_result = NodeByID(id);
      if (!node_result.ok()) {
        return node_result.status();
      }
      NodePage* node = node_result.value();
      if (node->size == 0) {
        if (const Status status = InsertAtIncremental(id, 0, new_rep, key, 0); !status.ok()) {
          return status;
        }
        return PutResult{0, false};
      }

      auto [leaf, ok] = node->Lookup(key);
      if (!ok) {
        return Status(StatusCode::kCorruptLayout);
      }
      frames.push_back(PutFrame{id, node, leaf});

      const Rep rep = node->reps[static_cast<std::size_t>(leaf)];
      if (rep.is_child()) {
        id = rep.child_id();
        continue;
      }

      const Position old_pos = rep.position();
      auto record_key = Key(old_pos);
      if (!record_key.ok()) {
        return record_key.status();
      }
      const int cmp = CompareKeys(key, record_key.value());
      if (cmp == 0) {
        const Position old_first = node->first_pos;
        const Position old_last = node->last_pos;
        Position first_pos = old_first;
        Position last_pos = old_last;
        if (leaf == 0) {
          auto min_pos = MinPos(new_rep);
          if (!min_pos.ok()) {
            return min_pos.status();
          }
          first_pos = min_pos.value();
        }
        if (leaf == static_cast<int>(node->size) - 1) {
          auto max_pos = MaxPos(new_rep);
          if (!max_pos.ok()) {
            return max_pos.status();
          }
          last_pos = max_pos.value();
        }
        node->reps[static_cast<std::size_t>(leaf)] = new_rep;
        node->first_pos = first_pos;
        node->last_pos = last_pos;
        if (const Status status =
                PropagateBoundary(frames, static_cast<int>(frames.size()) - 1, old_first,
                                  old_last);
            !status.ok()) {
          return status;
        }
        return PutResult{old_pos, true};
      }

      auto diff = FindDiffBit(key, record_key.value());
      if (!diff.ok()) {
        return diff.status();
      }
      auto slot = InsertSlotFromPath(frames, key, cmp, diff.value());
      if (!slot.ok()) {
        return slot.status();
      }
      if (const Status status =
              InsertAtPath(frames, slot.value().target, slot.value().slot, new_rep, key,
                           diff.value());
          !status.ok()) {
        return status;
      }
      return PutResult{0, false};
    }
  }

  Result<detail::InsertSlot> InsertSlotFromPath(Span<const PutFrame> frames, ByteView key,
                                                int cmp, std::uint16_t diff) {
    for (int i = 0; i < static_cast<int>(frames.size()); ++i) {
      auto node = NodeByID(frames[static_cast<std::size_t>(i)].id);
      if (!node.ok()) {
        return node.status();
      }
      auto slot = node.value()->InsertSlotAbovePath(
          key, diff, frames[static_cast<std::size_t>(i)].leaf);
      if (!slot.ok()) {
        return slot.status();
      }
      if (slot.value().second) {
        return detail::InsertSlot{i, slot.value().first};
      }
    }

    const PutFrame& last = frames.back();
    if (cmp < 0) {
      return detail::InsertSlot{static_cast<int>(frames.size()) - 1, last.leaf};
    }
    return detail::InsertSlot{static_cast<int>(frames.size()) - 1, last.leaf + 1};
  }

  Status InsertAtPath(Span<const PutFrame> frames, int target, int slot, Rep new_rep,
                      ByteView key, std::uint16_t diff) {
    const std::uint64_t target_id = frames[static_cast<std::size_t>(target)].id;
    auto node_result = NodeByID(target_id);
    if (!node_result.ok()) {
      return node_result.status();
    }
    NodePage* node = node_result.value();
    const Position old_first = node->first_pos;
    const Position old_last = node->last_pos;
    const int size = static_cast<int>(node->size);

    if (size < static_cast<int>(kMaxNodeReps)) {
      if (const Status status = InsertAtIncremental(target_id, slot, new_rep, key, diff);
          !status.ok()) {
        return status;
      }
      return PropagateBoundary(frames, target, old_first, old_last);
    }

    std::array<Rep, kMaxNodeReps + 1> buf{};
    for (int i = 0; i < slot; ++i) {
      buf[static_cast<std::size_t>(i)] = node->reps[static_cast<std::size_t>(i)];
    }
    buf[static_cast<std::size_t>(slot)] = new_rep;
    for (int i = slot; i < size; ++i) {
      buf[static_cast<std::size_t>(i + 1)] = node->reps[static_cast<std::size_t>(i)];
    }
    const auto reps = Span<const Rep>(buf.data(), static_cast<std::size_t>(size + 1));

    if (target > 0) {
      const PutFrame& parent_frame = frames[static_cast<std::size_t>(target - 1)];
      auto parent = NodeByID(parent_frame.id);
      if (!parent.ok()) {
        return parent.status();
      }
      if (static_cast<int>(parent.value()->size) < static_cast<int>(kMaxNodeReps)) {
        const Position parent_old_first = parent.value()->first_pos;
        const Position parent_old_last = parent.value()->last_pos;
        if (const Status status =
                PromoteSibling(parent_frame.id, parent_frame.leaf, target_id, reps);
            !status.ok()) {
          return status;
        }
        return PropagateBoundary(frames, target - 1, parent_old_first, parent_old_last);
      }
    }

    auto edge_slot = PathEdgeInsertSlot(frames, target, slot, size);
    if (!edge_slot.ok()) {
      return edge_slot.status();
    }
    std::vector<Rep> rep_vec(reps.begin(), reps.end());
    if (const Status status = SplitAndWriteNodeAt(target_id, std::move(rep_vec), edge_slot.value());
        !status.ok()) {
      return status;
    }
    return PropagateBoundary(frames, target, old_first, old_last);
  }

  Status InsertAtIncremental(std::uint64_t id, int slot, Rep new_rep, ByteView key,
                             std::uint16_t diff) {
    auto node_result = NodeByID(id);
    if (!node_result.ok()) {
      return node_result.status();
    }
    NodePage* node = node_result.value();
    const int size = static_cast<int>(node->size);
    if (slot < 0 || slot > size) {
      return Status(StatusCode::kCorruptLayout);
    }
    if (size >= static_cast<int>(kMaxNodeReps)) {
      return InsertFullAt(id, node, slot, new_rep);
    }

    Position first_pos = node->first_pos;
    Position last_pos = node->last_pos;
    if (size == 0) {
      auto min_pos = MinPos(new_rep);
      if (!min_pos.ok()) {
        return min_pos.status();
      }
      auto max_pos = MaxPos(new_rep);
      if (!max_pos.ok()) {
        return max_pos.status();
      }
      first_pos = min_pos.value();
      last_pos = max_pos.value();
    } else {
      if (slot == 0) {
        auto min_pos = MinPos(new_rep);
        if (!min_pos.ok()) {
          return min_pos.status();
        }
        first_pos = min_pos.value();
      }
      if (slot == size) {
        auto max_pos = MaxPos(new_rep);
        if (!max_pos.ok()) {
          return max_pos.status();
        }
        last_pos = max_pos.value();
      }
      if (const Status status = node->InsertRoute(slot, key, diff); !status.ok()) {
        return status;
      }
    }

    for (int i = size; i > slot; --i) {
      node->reps[static_cast<std::size_t>(i)] =
          node->reps[static_cast<std::size_t>(i - 1)];
    }
    node->reps[static_cast<std::size_t>(slot)] = new_rep;
    node->size = static_cast<std::uint16_t>(size + 1);
    node->first_pos = first_pos;
    node->last_pos = last_pos;
    return OkStatus();
  }

  Status InsertFullAt(std::uint64_t id, NodePage* node, int slot, Rep new_rep) {
    const int size = static_cast<int>(node->size);
    std::vector<Rep> reps;
    reps.reserve(static_cast<std::size_t>(size + 1));
    for (int i = 0; i < slot; ++i) {
      reps.push_back(node->reps[static_cast<std::size_t>(i)]);
    }
    reps.push_back(new_rep);
    for (int i = slot; i < size; ++i) {
      reps.push_back(node->reps[static_cast<std::size_t>(i)]);
    }
    return SplitAndWriteNode(id, std::move(reps));
  }

  Result<int> PathEdgeInsertSlot(Span<const PutFrame> frames, int target, int slot,
                                 int old_size) {
    if (slot != 0 && slot != old_size) {
      return -1;
    }
    for (int i = 0; i < target; ++i) {
      if (slot == 0) {
        if (frames[static_cast<std::size_t>(i)].leaf != 0) {
          return -1;
        }
        continue;
      }
      auto node = NodeByID(frames[static_cast<std::size_t>(i)].id);
      if (!node.ok()) {
        return node.status();
      }
      if (frames[static_cast<std::size_t>(i)].leaf != static_cast<int>(node.value()->size) - 1) {
        return -1;
      }
    }
    return slot;
  }

  Status PropagateBoundary(Span<const PutFrame> frames, int changed, Position old_first,
                           Position old_last) {
    auto child_result = NodeByID(frames[static_cast<std::size_t>(changed)].id);
    if (!child_result.ok()) {
      return child_result.status();
    }
    NodePage* child = child_result.value();
    bool first_changed = child->first_pos != old_first;
    bool last_changed = child->last_pos != old_last;
    if (!first_changed && !last_changed) {
      return OkStatus();
    }

    for (int i = changed - 1; i >= 0; --i) {
      auto parent_result = NodeByID(frames[static_cast<std::size_t>(i)].id);
      if (!parent_result.ok()) {
        return parent_result.status();
      }
      NodePage* parent = parent_result.value();
      const int child_slot = frames[static_cast<std::size_t>(i)].leaf;
      const int size = static_cast<int>(parent->size);
      if (child_slot < 0 || child_slot >= size) {
        return Status(StatusCode::kCorruptLayout);
      }
      const Rep child_rep = parent->reps[static_cast<std::size_t>(child_slot)];
      if (!child_rep.is_child() ||
          child_rep.child_id() != frames[static_cast<std::size_t>(i + 1)].id) {
        return Status(StatusCode::kCorruptLayout);
      }

      const Position parent_old_first = parent->first_pos;
      const Position parent_old_last = parent->last_pos;
      if (first_changed && child_slot == 0) {
        parent->first_pos = child->first_pos;
      }
      if (last_changed && child_slot == size - 1) {
        parent->last_pos = child->last_pos;
      }

      child = parent;
      first_changed = parent->first_pos != parent_old_first;
      last_changed = parent->last_pos != parent_old_last;
      if (!first_changed && !last_changed) {
        return OkStatus();
      }
    }
    return OkStatus();
  }

  Status PromoteSibling(std::uint64_t parent_id, int child_slot, std::uint64_t child_id,
                        Span<const Rep> reps) {
    if (reps.size() != kMaxNodeReps + 1) {
      return Status(StatusCode::kCorruptLayout);
    }

    auto parent_result = NodeByID(parent_id);
    if (!parent_result.ok()) {
      return parent_result.status();
    }
    NodePage* parent = parent_result.value();
    const int parent_size = static_cast<int>(parent->size);
    if (child_slot < 0 || child_slot >= parent_size ||
        parent_size >= static_cast<int>(kMaxNodeReps)) {
      return Status(StatusCode::kCorruptLayout);
    }
    const Rep child_rep = parent->reps[static_cast<std::size_t>(child_slot)];
    if (!child_rep.is_child() || child_rep.child_id() != child_id) {
      return Status(StatusCode::kCorruptLayout);
    }

    auto diffs = BuildDiffs(reps);
    if (!diffs.ok()) {
      return diffs.status();
    }
    auto cartesian = BuildCartesian(diffs.value());
    const int split = cartesian.root + 1;
    const std::uint16_t split_diff = diffs.value()[static_cast<std::size_t>(cartesian.root)];

    auto sibling_alloc = AllocNode();
    if (!sibling_alloc.ok()) {
      return sibling_alloc.status();
    }
    const std::uint64_t sibling_id = sibling_alloc.value().id;
    if (const Status status =
            WriteNodeWithDiffs(child_id, reps.subspan(0, static_cast<std::size_t>(split)),
                               diffs.value().subspan(0, static_cast<std::size_t>(split - 1)));
        !status.ok()) {
      return status;
    }
    if (const Status status = WriteNodeWithDiffs(
            sibling_id, reps.subspan(static_cast<std::size_t>(split)),
            diffs.value().subspan(static_cast<std::size_t>(split)));
        !status.ok()) {
      return status;
    }

    auto sibling_rep = Rep::MakeChild(sibling_id);
    if (!sibling_rep.ok()) {
      return sibling_rep.status();
    }

    std::array<Rep, kMaxNodeReps> parent_reps{};
    for (int i = 0; i <= child_slot; ++i) {
      parent_reps[static_cast<std::size_t>(i)] =
          parent->reps[static_cast<std::size_t>(i)];
    }
    parent_reps[static_cast<std::size_t>(child_slot + 1)] = sibling_rep.value();
    for (int i = child_slot + 1; i < parent_size; ++i) {
      parent_reps[static_cast<std::size_t>(i + 1)] =
          parent->reps[static_cast<std::size_t>(i)];
    }

    std::array<std::uint16_t, kMaxNodeReps - 1> old_diff_buf{};
    auto old_diffs =
        Span<std::uint16_t>(old_diff_buf.data(), static_cast<std::size_t>(parent_size - 1));
    if (const Status status = parent->RouteDiffs(old_diffs); !status.ok()) {
      return status;
    }

    std::array<std::uint16_t, kMaxNodeReps - 1> new_diff_buf{};
    auto new_diffs =
        Span<std::uint16_t>(new_diff_buf.data(), static_cast<std::size_t>(parent_size));
    for (int i = 0; i < child_slot; ++i) {
      new_diffs[static_cast<std::size_t>(i)] = old_diffs[static_cast<std::size_t>(i)];
    }
    new_diffs[static_cast<std::size_t>(child_slot)] = split_diff;
    for (int i = child_slot; i < parent_size - 1; ++i) {
      new_diffs[static_cast<std::size_t>(i + 1)] = old_diffs[static_cast<std::size_t>(i)];
    }

    return WriteNodeWithDiffs(parent_id,
                              Span<const Rep>(parent_reps.data(),
                                                   static_cast<std::size_t>(parent_size + 1)),
                              new_diffs);
  }

  Status WriteNodeWithDiffs(std::uint64_t id, Span<const Rep> reps,
                            Span<const std::uint16_t> diffs) {
    if (reps.size() > kMaxNodeReps) {
      return Status(StatusCode::kCorruptLayout);
    }
    if (reps.empty()) {
      if (!diffs.empty()) {
        return Status(StatusCode::kCorruptLayout);
      }
    } else if (diffs.size() != reps.size() - 1) {
      return Status(StatusCode::kCorruptLayout);
    }

    auto node_result = NodeByID(id);
    if (!node_result.ok()) {
      return node_result.status();
    }
    NodePage* node = node_result.value();
    node->reps.fill(Rep{});
    for (std::size_t i = 0; i < reps.size(); ++i) {
      node->reps[i] = reps[i];
    }
    node->size = static_cast<std::uint16_t>(reps.size());
    return RebuildNodeWithDiffs(node, diffs);
  }

  Status WriteNode(std::uint64_t id, Span<const Rep> reps) {
    if (reps.size() > kMaxNodeReps) {
      return SplitAndWriteNode(id, std::vector<Rep>(reps.begin(), reps.end()));
    }

    auto node_result = NodeByID(id);
    if (!node_result.ok()) {
      return node_result.status();
    }
    NodePage* node = node_result.value();
    node->reps.fill(Rep{});
    for (std::size_t i = 0; i < reps.size(); ++i) {
      node->reps[i] = reps[i];
    }
    node->size = static_cast<std::uint16_t>(reps.size());
    return RebuildNode(node);
  }

  Status SplitAndWriteNode(std::uint64_t id, std::vector<Rep> reps) {
    return SplitAndWriteNodeAt(id, std::move(reps), -1);
  }

  Status SplitAndWriteNodeAt(std::uint64_t id, std::vector<Rep> reps, int insert_slot) {
    while (reps.size() > kMaxNodeReps) {
      auto range = ChooseSplitRange(Span<const Rep>(reps.data(), reps.size()), insert_slot);
      if (!range.ok()) {
        return range.status();
      }
      const int start = range.value().start;
      const int count = range.value().count;
      const int end = start + count;

      auto child_alloc = AllocNode();
      if (!child_alloc.ok()) {
        return child_alloc.status();
      }
      const std::uint64_t child_id = child_alloc.value().id;
      std::vector<Rep> child_reps(reps.begin() + start, reps.begin() + end);
      if (const Status status =
              WriteNode(child_id,
                        Span<const Rep>(child_reps.data(), child_reps.size()));
          !status.ok()) {
        return status;
      }
      if (child_alloc.value().node->size == 0) {
        return Status(StatusCode::kCorruptLayout);
      }

      auto child_rep = Rep::MakeChild(child_id);
      if (!child_rep.ok()) {
        return child_rep.status();
      }
      reps[static_cast<std::size_t>(start)] = child_rep.value();
      reps.erase(reps.begin() + start + 1, reps.begin() + end);
      insert_slot = RemapInsertSlotAfterSplit(insert_slot, start, end, count);
    }
    return WriteNode(id, Span<const Rep>(reps.data(), reps.size()));
  }

  static int RemapInsertSlotAfterSplit(int slot, int start, int end, int count) {
    if (slot < 0) {
      return slot;
    }
    if (slot < start) {
      return slot;
    }
    if (slot < end) {
      return start;
    }
    return slot - count + 1;
  }

  Result<detail::SplitRange> ChooseSplitRange(Span<const Rep> reps, int insert_slot) {
    if (reps.size() < 4) {
      return Status(StatusCode::kCorruptLayout);
    }
    auto diffs = BuildDiffs(reps);
    if (!diffs.ok()) {
      return diffs.status();
    }

    if (insert_slot == 0 || insert_slot == static_cast<int>(reps.size()) - 1) {
      auto edge = ChooseEdgeSplitRange(diffs.value(), static_cast<int>(reps.size()), insert_slot);
      if (edge.ok()) {
        return edge.value();
      }
    }

    auto cartesian = BuildCartesian(diffs.value());
    const int rep_count = static_cast<int>(reps.size());
    const int target = rep_count / 2;
    constexpr int kPreferredSplitMinRatioNum = 1;
    constexpr int kPreferredSplitMinRatioDen = 3;
    const int min_preferred_count =
        (rep_count * kPreferredSplitMinRatioNum + kPreferredSplitMinRatioDen - 1) /
        kPreferredSplitMinRatioDen;

    int best_start = -1;
    int best_count = 0;
    int best_score = rep_count;
    int preferred_start = -1;
    int preferred_count = 0;

    std::vector<RouteFrame> stack;
    stack.push_back(RouteFrame{cartesian.root, 0, rep_count});
    while (!stack.empty()) {
      const RouteFrame frame = stack.back();
      stack.pop_back();

      const int count = frame.leaf_r - frame.leaf_l;
      if (count >= 2 && count <= rep_count - 2) {
        const int score = detail::AbsInt(count - target);
        if (best_start == -1 || score < best_score) {
          best_start = frame.leaf_l;
          best_count = count;
          best_score = score;
        }
        if (count >= min_preferred_count && count <= target &&
            (preferred_start == -1 || count > preferred_count)) {
          preferred_start = frame.leaf_l;
          preferred_count = count;
        }
      }

      const int i = frame.node;
      if (cartesian.right[static_cast<std::size_t>(i)] != -1) {
        stack.push_back(
            RouteFrame{cartesian.right[static_cast<std::size_t>(i)], i + 1, frame.leaf_r});
      }
      if (cartesian.left[static_cast<std::size_t>(i)] != -1) {
        stack.push_back(
            RouteFrame{cartesian.left[static_cast<std::size_t>(i)], frame.leaf_l, i + 1});
      }
    }

    if (best_start == -1) {
      const int count = rep_count / 2;
      const int start = (rep_count - count) / 2;
      return detail::SplitRange{start, count};
    }
    if (preferred_start != -1) {
      return detail::SplitRange{preferred_start, preferred_count};
    }
    return detail::SplitRange{best_start, best_count};
  }

  Result<detail::SplitRange> ChooseEdgeSplitRange(Span<const std::uint16_t> diffs,
                                                  int rep_count, int insert_slot) {
    auto cartesian = BuildCartesian(diffs);
    int best_start = -1;
    int best_count = 0;

    std::vector<RouteFrame> stack;
    stack.push_back(RouteFrame{cartesian.root, 0, rep_count});
    while (!stack.empty()) {
      const RouteFrame frame = stack.back();
      stack.pop_back();

      const int count = frame.leaf_r - frame.leaf_l;
      if (count >= 2 && count <= rep_count - 2 &&
          (insert_slot < frame.leaf_l || insert_slot >= frame.leaf_r)) {
        if (best_start == -1 || count > best_count) {
          best_start = frame.leaf_l;
          best_count = count;
        }
      }

      const int i = frame.node;
      if (cartesian.right[static_cast<std::size_t>(i)] != -1) {
        stack.push_back(
            RouteFrame{cartesian.right[static_cast<std::size_t>(i)], i + 1, frame.leaf_r});
      }
      if (cartesian.left[static_cast<std::size_t>(i)] != -1) {
        stack.push_back(
            RouteFrame{cartesian.left[static_cast<std::size_t>(i)], frame.leaf_l, i + 1});
      }
    }

    if (best_start == -1) {
      return Status(StatusCode::kMissingKey);
    }
    return detail::SplitRange{best_start, best_count};
  }

  Result<DeleteResult> DeleteFrom(std::uint64_t id, ByteView key) {
    auto node_result = NodeByID(id);
    if (!node_result.ok()) {
      return node_result.status();
    }
    NodePage* node = node_result.value();

    auto [leaf, ok] = node->Lookup(key);
    if (!ok) {
      return DeleteResult{0, false};
    }
    const Rep rep = node->reps[static_cast<std::size_t>(leaf)];
    if (!rep.is_child()) {
      const Position old_pos = rep.position();
      auto record_key = Key(old_pos);
      if (!record_key.ok()) {
        return record_key.status();
      }
      if (CompareKeys(record_key.value(), key) == 0) {
        if (const Status status = DeleteAtIncremental(node, leaf); !status.ok()) {
          return status;
        }
        return DeleteResult{old_pos, true};
      }
    } else {
      return DeleteFromChild(node, leaf, rep.child_id(), key);
    }
    return DeleteResult{0, false};
  }

  Result<DeleteResult> DeleteFromChild(NodePage* node, int slot, std::uint64_t child_id,
                                       ByteView key) {
    auto child_result = NodeByID(child_id);
    if (!child_result.ok()) {
      return child_result.status();
    }
    NodePage* child = child_result.value();
    if (child->size == 0) {
      return Status(StatusCode::kCorruptLayout);
    }
    const Position old_first = child->first_pos;
    const Position old_last = child->last_pos;

    auto result = DeleteFrom(child_id, key);
    if (!result.ok() || !result.value().deleted) {
      return result;
    }
    const int parent_size = static_cast<int>(node->size);
    if (child->size == 0) {
      if (const Status status = DeleteAtIncremental(node, slot); !status.ok()) {
        return status;
      }
      if (const Status status = FreeNode(child_id); !status.ok()) {
        return status;
      }
      return result.value();
    }

    const int child_size = static_cast<int>(child->size);
    if (parent_size - 1 + child_size <= static_cast<int>(kMaxNodeReps)) {
      if (const Status status = MergeChildIntoParent(node, slot, child_id, child);
          !status.ok()) {
        return status;
      }
      return result.value();
    }

    if (child_size < static_cast<int>(kMaxNodeReps) / 32) {
      auto merged = MergeChildWithSibling(node, slot, child_id, child);
      if (!merged.ok()) {
        return merged.status();
      }
      if (merged.value()) {
        return result.value();
      }
    }

    if (child->first_pos == old_first && child->last_pos == old_last) {
      return result.value();
    }
    if (const Status status =
            RebuildParentAfterChildBoundary(node, slot, child, old_first, old_last);
        !status.ok()) {
      return status;
    }
    return result.value();
  }

  Status DeleteAtIncremental(NodePage* node, int slot) {
    const int size = static_cast<int>(node->size);
    if (slot < 0 || slot >= size) {
      return Status(StatusCode::kCorruptLayout);
    }
    if (size == 1) {
      node->reps[0] = Rep{};
      node->size = 0;
      node->first_pos = 0;
      node->last_pos = 0;
      return OkStatus();
    }

    Position first_pos = node->first_pos;
    Position last_pos = node->last_pos;
    if (slot == 0) {
      auto min_pos = MinPos(node->reps[1]);
      if (!min_pos.ok()) {
        return min_pos.status();
      }
      first_pos = min_pos.value();
    }
    if (slot == size - 1) {
      auto max_pos = MaxPos(node->reps[static_cast<std::size_t>(size - 2)]);
      if (!max_pos.ok()) {
        return max_pos.status();
      }
      last_pos = max_pos.value();
    }
    if (const Status status = node->DeleteRoute(slot); !status.ok()) {
      return status;
    }

    for (int i = slot; i < size - 1; ++i) {
      node->reps[static_cast<std::size_t>(i)] =
          node->reps[static_cast<std::size_t>(i + 1)];
    }
    node->reps[static_cast<std::size_t>(size - 1)] = Rep{};
    node->size = static_cast<std::uint16_t>(size - 1);
    node->first_pos = first_pos;
    node->last_pos = last_pos;
    return OkStatus();
  }

  Status RebuildParentAfterChildBoundary(NodePage* node, int slot, NodePage* child,
                                         Position old_first, Position old_last) {
    const int size = static_cast<int>(node->size);
    const bool first_changed = child->first_pos != old_first;
    const bool last_changed = child->last_pos != old_last;
    if (first_changed && slot == 0) {
      node->first_pos = child->first_pos;
    }
    if (last_changed && slot == size - 1) {
      node->last_pos = child->last_pos;
    }
    return OkStatus();
  }

  Status CompressRoot() {
    auto root_result = Root();
    if (!root_result.ok()) {
      return root_result.status();
    }
    NodePage* root = root_result.value();
    while (root->size == 1 && root->reps[0].is_child()) {
      const std::uint64_t child_id = root->reps[0].child_id();
      auto child = NodeByID(child_id);
      if (!child.ok()) {
        return child.status();
      }
      *root = *child.value();
      if (const Status status = FreeNode(child_id); !status.ok()) {
        return status;
      }
    }
    return OkStatus();
  }

  Status MergeChildIntoParent(NodePage* node, int slot, std::uint64_t child_id,
                              NodePage* child) {
    const int parent_size = static_cast<int>(node->size);
    const int child_size = static_cast<int>(child->size);
    const int new_size = parent_size - 1 + child_size;

    std::array<std::uint16_t, kMaxNodeReps - 1> old_parent_diff_buf{};
    auto old_parent_diffs = Span<std::uint16_t>(
        old_parent_diff_buf.data(), static_cast<std::size_t>(parent_size - 1));
    if (const Status status = node->RouteDiffs(old_parent_diffs); !status.ok()) {
      return status;
    }

    std::array<std::uint16_t, kMaxNodeReps - 1> new_diff_buf{};
    auto new_diffs =
        Span<std::uint16_t>(new_diff_buf.data(), static_cast<std::size_t>(new_size - 1));
    for (int i = 0; i < slot; ++i) {
      new_diffs[static_cast<std::size_t>(i)] =
          old_parent_diffs[static_cast<std::size_t>(i)];
    }
    if (child_size > 1) {
      if (const Status status = child->RouteDiffs(
              new_diffs.subspan(static_cast<std::size_t>(slot),
                                static_cast<std::size_t>(child_size - 1)));
          !status.ok()) {
        return status;
      }
    }
    for (int i = slot; i < parent_size - 1; ++i) {
      new_diffs[static_cast<std::size_t>(slot + child_size - 1 + (i - slot))] =
          old_parent_diffs[static_cast<std::size_t>(i)];
    }

    for (int i = parent_size - 1; i > slot; --i) {
      node->reps[static_cast<std::size_t>(i + child_size - 1)] =
          node->reps[static_cast<std::size_t>(i)];
    }
    for (int i = 0; i < child_size; ++i) {
      node->reps[static_cast<std::size_t>(slot + i)] =
          child->reps[static_cast<std::size_t>(i)];
    }
    for (int i = new_size; i < parent_size; ++i) {
      node->reps[static_cast<std::size_t>(i)] = Rep{};
    }
    node->size = static_cast<std::uint16_t>(new_size);
    if (const Status status = RebuildNodeWithDiffs(node, new_diffs); !status.ok()) {
      return status;
    }
    return FreeNode(child_id);
  }

  Result<bool> MergeChildWithSibling(NodePage* node, int slot, std::uint64_t child_id,
                                     NodePage* child) {
    const int parent_size = static_cast<int>(node->size);
    const int child_size = static_cast<int>(child->size);
    const int left_slot = slot - 1;
    const int right_slot = slot + 1;
    int left_size = -1;
    int right_size = -1;
    std::uint64_t left_id = 0;
    std::uint64_t right_id = 0;
    NodePage* left = nullptr;
    NodePage* right = nullptr;

    if (left_slot >= 0) {
      const Rep rep = node->reps[static_cast<std::size_t>(left_slot)];
      if (rep.is_child()) {
        left_id = rep.child_id();
        auto left_result = NodeByID(left_id);
        if (!left_result.ok()) {
          return left_result.status();
        }
        left = left_result.value();
        left_size = static_cast<int>(left->size);
      }
    }
    if (right_slot < parent_size) {
      const Rep rep = node->reps[static_cast<std::size_t>(right_slot)];
      if (rep.is_child()) {
        right_id = rep.child_id();
        auto right_result = NodeByID(right_id);
        if (!right_result.ok()) {
          return right_result.status();
        }
        right = right_result.value();
        right_size = static_cast<int>(right->size);
      }
    }

    const bool can_merge_left =
        left_size >= 0 && left_size + child_size <= static_cast<int>(kMaxNodeReps) &&
        node->IsDirectRoutePair(left_slot);
    const bool can_merge_right =
        right_size >= 0 && child_size + right_size <= static_cast<int>(kMaxNodeReps) &&
        node->IsDirectRoutePair(slot);
    if (!can_merge_left && !can_merge_right) {
      return false;
    }
    if (can_merge_right && (!can_merge_left || right_size > left_size)) {
      if (const Status status =
              MergeRightSibling(node, slot, child_id, child, right_slot, right_id, right);
          !status.ok()) {
        return status;
      }
      return true;
    }
    if (const Status status =
            MergeLeftSibling(node, left_slot, left_id, left, slot, child_id, child);
        !status.ok()) {
      return status;
    }
    return true;
  }

  Status MergeLeftSibling(NodePage* node, int left_slot, std::uint64_t left_id, NodePage* left,
                          int slot, std::uint64_t child_id, NodePage* child) {
    const int parent_size = static_cast<int>(node->size);
    const int left_size = static_cast<int>(left->size);
    const int child_size = static_cast<int>(child->size);
    std::array<Rep, kMaxNodeReps> rep_buf{};
    auto reps =
        Span<Rep>(rep_buf.data(), static_cast<std::size_t>(left_size + child_size));
    for (int i = 0; i < left_size; ++i) {
      reps[static_cast<std::size_t>(i)] = left->reps[static_cast<std::size_t>(i)];
    }
    for (int i = 0; i < child_size; ++i) {
      reps[static_cast<std::size_t>(left_size + i)] =
          child->reps[static_cast<std::size_t>(i)];
    }

    std::array<std::uint16_t, kMaxNodeReps - 1> old_parent_diff_buf{};
    auto old_parent_diffs = Span<std::uint16_t>(
        old_parent_diff_buf.data(), static_cast<std::size_t>(parent_size - 1));
    if (const Status status = node->RouteDiffs(old_parent_diffs); !status.ok()) {
      return status;
    }

    std::array<std::uint16_t, kMaxNodeReps - 1> diff_buf{};
    auto diffs =
        Span<std::uint16_t>(diff_buf.data(), static_cast<std::size_t>(reps.size() - 1));
    if (left_size > 1) {
      if (const Status status =
              left->RouteDiffs(diffs.subspan(0, static_cast<std::size_t>(left_size - 1)));
          !status.ok()) {
        return status;
      }
    }
    diffs[static_cast<std::size_t>(left_size - 1)] =
        old_parent_diffs[static_cast<std::size_t>(left_slot)];
    if (child_size > 1) {
      if (const Status status =
              child->RouteDiffs(diffs.subspan(static_cast<std::size_t>(left_size)));
          !status.ok()) {
        return status;
      }
    }

    if (const Status status = WriteNodeWithDiffs(left_id, reps, diffs); !status.ok()) {
      return status;
    }
    if (const Status status = RemoveMergedParentRep(node, slot, left_slot); !status.ok()) {
      return status;
    }
    return FreeNode(child_id);
  }

  Status MergeRightSibling(NodePage* node, int slot, std::uint64_t child_id, NodePage* child,
                           int right_slot, std::uint64_t right_id, NodePage* right) {
    const int parent_size = static_cast<int>(node->size);
    const int child_size = static_cast<int>(child->size);
    const int right_size = static_cast<int>(right->size);
    std::array<Rep, kMaxNodeReps> rep_buf{};
    auto reps =
        Span<Rep>(rep_buf.data(), static_cast<std::size_t>(child_size + right_size));
    for (int i = 0; i < child_size; ++i) {
      reps[static_cast<std::size_t>(i)] = child->reps[static_cast<std::size_t>(i)];
    }
    for (int i = 0; i < right_size; ++i) {
      reps[static_cast<std::size_t>(child_size + i)] =
          right->reps[static_cast<std::size_t>(i)];
    }

    std::array<std::uint16_t, kMaxNodeReps - 1> old_parent_diff_buf{};
    auto old_parent_diffs = Span<std::uint16_t>(
        old_parent_diff_buf.data(), static_cast<std::size_t>(parent_size - 1));
    if (const Status status = node->RouteDiffs(old_parent_diffs); !status.ok()) {
      return status;
    }

    std::array<std::uint16_t, kMaxNodeReps - 1> diff_buf{};
    auto diffs =
        Span<std::uint16_t>(diff_buf.data(), static_cast<std::size_t>(reps.size() - 1));
    if (child_size > 1) {
      if (const Status status =
              child->RouteDiffs(diffs.subspan(0, static_cast<std::size_t>(child_size - 1)));
          !status.ok()) {
        return status;
      }
    }
    diffs[static_cast<std::size_t>(child_size - 1)] =
        old_parent_diffs[static_cast<std::size_t>(slot)];
    if (right_size > 1) {
      if (const Status status =
              right->RouteDiffs(diffs.subspan(static_cast<std::size_t>(child_size)));
          !status.ok()) {
        return status;
      }
    }

    if (const Status status = WriteNodeWithDiffs(child_id, reps, diffs); !status.ok()) {
      return status;
    }
    if (const Status status = RemoveMergedParentRep(node, right_slot, slot); !status.ok()) {
      return status;
    }
    return FreeNode(right_id);
  }

  Status RemoveMergedParentRep(NodePage* node, int slot, int drop_diff) {
    const int size = static_cast<int>(node->size);
    if (slot < 0 || slot >= size || drop_diff < 0 || drop_diff >= size - 1) {
      return Status(StatusCode::kCorruptLayout);
    }

    std::array<std::uint16_t, kMaxNodeReps - 1> old_diff_buf{};
    auto old_diffs =
        Span<std::uint16_t>(old_diff_buf.data(), static_cast<std::size_t>(size - 1));
    if (const Status status = node->RouteDiffs(old_diffs); !status.ok()) {
      return status;
    }
    std::array<std::uint16_t, kMaxNodeReps - 1> new_diff_buf{};
    auto new_diffs =
        Span<std::uint16_t>(new_diff_buf.data(), static_cast<std::size_t>(size - 2));
    for (int i = 0; i < drop_diff; ++i) {
      new_diffs[static_cast<std::size_t>(i)] = old_diffs[static_cast<std::size_t>(i)];
    }
    for (int i = drop_diff + 1; i < size - 1; ++i) {
      new_diffs[static_cast<std::size_t>(i - 1)] = old_diffs[static_cast<std::size_t>(i)];
    }

    for (int i = slot; i < size - 1; ++i) {
      node->reps[static_cast<std::size_t>(i)] =
          node->reps[static_cast<std::size_t>(i + 1)];
    }
    node->reps[static_cast<std::size_t>(size - 1)] = Rep{};
    node->size = static_cast<std::uint16_t>(size - 1);
    return RebuildNodeWithDiffs(node, new_diffs);
  }

  Status Retarget(ByteView key, Position old_pos, Rep new_rep) {
    std::vector<PutFrame> frames;
    frames.reserve(16);
    std::uint64_t id = root_id_;

    for (;;) {
      auto node_result = NodeByID(id);
      if (!node_result.ok()) {
        return node_result.status();
      }
      NodePage* node = node_result.value();
      auto [leaf, ok] = node->Lookup(key);
      if (!ok) {
        return Status(StatusCode::kPositionMismatch);
      }
      frames.push_back(PutFrame{id, node, leaf});

      const Rep rep = node->reps[static_cast<std::size_t>(leaf)];
      if (rep.is_child()) {
        id = rep.child_id();
        continue;
      }
      if (rep.position() != old_pos) {
        return Status(StatusCode::kPositionMismatch);
      }

      const Position old_first = node->first_pos;
      const Position old_last = node->last_pos;
      const Position new_pos = new_rep.position();
      if (leaf == 0) {
        node->first_pos = new_pos;
      }
      if (leaf == static_cast<int>(node->size) - 1) {
        node->last_pos = new_pos;
      }
      node->reps[static_cast<std::size_t>(leaf)] = new_rep;
      return PropagateBoundary(frames, static_cast<int>(frames.size()) - 1, old_first,
                               old_last);
    }
  }

  Result<bool> SeekGreaterOrEqual(ByteView key, detail::IterPath* path) {
    path->Reset();
    std::uint64_t id = root_id_;

    for (;;) {
      auto node_result = NodeByID(id);
      if (!node_result.ok()) {
        return node_result.status();
      }
      NodePage* node = node_result.value();
      if (node->size == 0) {
        return false;
      }

      auto [leaf, ok] = node->Lookup(key);
      if (!ok) {
        return Status(StatusCode::kCorruptLayout);
      }
      path->PushNode(id, node, leaf);

      const Rep rep = node->reps[static_cast<std::size_t>(leaf)];
      if (rep.is_child()) {
        id = rep.child_id();
        continue;
      }

      const Position pos = rep.position();
      auto record_key = Key(pos);
      if (!record_key.ok()) {
        return record_key.status();
      }
      auto cmp = CompareAndDiffBit(key, record_key.value());
      if (!cmp.ok()) {
        if (cmp.status() == StatusCode::kEqualKeys) {
          return true;
        }
        return cmp.status();
      }

      auto frames = path->FramesForInsert();
      auto slot = InsertSlotFromPath(frames, key, cmp.value().compare, cmp.value().diff);
      if (!slot.ok()) {
        return slot.status();
      }
      return PositionAtOrAfter(path, slot.value().target, slot.value().slot);
    }
  }

  Result<bool> SeekLessOrEqual(ByteView key, detail::IterPath* path) {
    path->Reset();
    std::uint64_t id = root_id_;

    for (;;) {
      auto node_result = NodeByID(id);
      if (!node_result.ok()) {
        return node_result.status();
      }
      NodePage* node = node_result.value();
      if (node->size == 0) {
        return false;
      }

      auto [leaf, ok] = node->Lookup(key);
      if (!ok) {
        return Status(StatusCode::kCorruptLayout);
      }
      path->PushNode(id, node, leaf);

      const Rep rep = node->reps[static_cast<std::size_t>(leaf)];
      if (rep.is_child()) {
        id = rep.child_id();
        continue;
      }

      const Position pos = rep.position();
      auto record_key = Key(pos);
      if (!record_key.ok()) {
        return record_key.status();
      }
      auto cmp = CompareAndDiffBit(key, record_key.value());
      if (!cmp.ok()) {
        if (cmp.status() == StatusCode::kEqualKeys) {
          return true;
        }
        return cmp.status();
      }

      auto frames = path->FramesForInsert();
      auto slot = InsertSlotFromPath(frames, key, cmp.value().compare, cmp.value().diff);
      if (!slot.ok()) {
        return slot.status();
      }
      return PositionAtOrBefore(path, slot.value().target, slot.value().slot);
    }
  }

  Result<NodePage*> NodeForFrame(PutFrame* frame) {
    if (frame->node != nullptr) {
      return frame->node;
    }
    return NodeByID(frame->id);
  }

  Result<bool> PositionAtOrAfter(detail::IterPath* path, int target, int slot) {
    if (target < 0 || target >= path->len) {
      return Status(StatusCode::kCorruptLayout);
    }
    path->Truncate(target + 1);

    for (;;) {
      PutFrame* frame = path->At(path->len - 1);
      auto node_result = NodeForFrame(frame);
      if (!node_result.ok()) {
        return node_result.status();
      }
      NodePage* node = node_result.value();
      const int size = static_cast<int>(node->size);
      if (slot < 0 || slot > size) {
        return Status(StatusCode::kCorruptLayout);
      }
      if (slot < size) {
        frame->leaf = slot;
        const Rep rep = node->reps[static_cast<std::size_t>(slot)];
        if (rep.is_child()) {
          return LeftmostRecord(path, rep.child_id());
        }
        return true;
      }

      if (path->len == 1) {
        return false;
      }
      path->Truncate(path->len - 1);
      slot = path->At(path->len - 1)->leaf + 1;
    }
  }

  Result<bool> PositionAtOrBefore(detail::IterPath* path, int target, int slot) {
    if (target < 0 || target >= path->len) {
      return Status(StatusCode::kCorruptLayout);
    }
    path->Truncate(target + 1);

    for (;;) {
      PutFrame* frame = path->At(path->len - 1);
      auto node_result = NodeForFrame(frame);
      if (!node_result.ok()) {
        return node_result.status();
      }
      NodePage* node = node_result.value();
      const int size = static_cast<int>(node->size);
      if (slot < 0 || slot > size) {
        return Status(StatusCode::kCorruptLayout);
      }
      if (slot > 0) {
        const int prev = slot - 1;
        frame->leaf = prev;
        const Rep rep = node->reps[static_cast<std::size_t>(prev)];
        if (rep.is_child()) {
          return RightmostRecord(path, rep.child_id());
        }
        return true;
      }

      if (path->len == 1) {
        return false;
      }
      path->Truncate(path->len - 1);
      slot = path->At(path->len - 1)->leaf;
    }
  }

  Result<bool> LeftmostRoot(detail::IterPath* path) {
    path->Reset();
    auto root = Root();
    if (!root.ok()) {
      return root.status();
    }
    if (root.value()->size == 0) {
      return false;
    }
    return LeftmostRecord(path, root_id_);
  }

  Result<bool> RightmostRoot(detail::IterPath* path) {
    path->Reset();
    auto root = Root();
    if (!root.ok()) {
      return root.status();
    }
    if (root.value()->size == 0) {
      return false;
    }
    return RightmostRecord(path, root_id_);
  }

  Result<bool> LeftmostRecord(detail::IterPath* path, std::uint64_t id) {
    for (;;) {
      auto node_result = NodeByID(id);
      if (!node_result.ok()) {
        return node_result.status();
      }
      NodePage* node = node_result.value();
      if (node->size == 0) {
        return Status(StatusCode::kCorruptLayout);
      }
      path->PushNode(id, node, 0);

      const Rep rep = node->reps[0];
      if (rep.is_child()) {
        id = rep.child_id();
        continue;
      }
      return true;
    }
  }

  Result<bool> RightmostRecord(detail::IterPath* path, std::uint64_t id) {
    for (;;) {
      auto node_result = NodeByID(id);
      if (!node_result.ok()) {
        return node_result.status();
      }
      NodePage* node = node_result.value();
      if (node->size == 0) {
        return Status(StatusCode::kCorruptLayout);
      }
      const int leaf = static_cast<int>(node->size) - 1;
      path->PushNode(id, node, leaf);

      const Rep rep = node->reps[static_cast<std::size_t>(leaf)];
      if (rep.is_child()) {
        id = rep.child_id();
        continue;
      }
      return true;
    }
  }

  Result<bool> NextPath(detail::IterPath* path) {
    while (path->len > 0) {
      PutFrame* frame = path->At(path->len - 1);
      auto node_result = NodeForFrame(frame);
      if (!node_result.ok()) {
        return node_result.status();
      }
      NodePage* node = node_result.value();
      const int next = frame->leaf + 1;
      if (next < static_cast<int>(node->size)) {
        frame->leaf = next;
        const Rep rep = node->reps[static_cast<std::size_t>(next)];
        if (rep.is_child()) {
          return LeftmostRecord(path, rep.child_id());
        }
        return true;
      }
      path->Truncate(path->len - 1);
    }
    return false;
  }

  Result<bool> PrevPath(detail::IterPath* path) {
    while (path->len > 0) {
      PutFrame* frame = path->At(path->len - 1);
      auto node_result = NodeForFrame(frame);
      if (!node_result.ok()) {
        return node_result.status();
      }
      NodePage* node = node_result.value();
      const int prev = frame->leaf - 1;
      if (prev >= 0) {
        frame->leaf = prev;
        const Rep rep = node->reps[static_cast<std::size_t>(prev)];
        if (rep.is_child()) {
          return RightmostRecord(path, rep.child_id());
        }
        return true;
      }
      path->Truncate(path->len - 1);
    }
    return false;
  }

  Result<detail::IterItem> CurrentRecord(detail::IterPath* path) {
    if (path->len == 0) {
      return Status(StatusCode::kCorruptLayout);
    }
    PutFrame* frame = path->At(path->len - 1);
    auto node_result = NodeForFrame(frame);
    if (!node_result.ok()) {
      return node_result.status();
    }
    NodePage* node = node_result.value();
    if (frame->leaf < 0 || frame->leaf >= static_cast<int>(node->size)) {
      return Status(StatusCode::kCorruptLayout);
    }
    const Rep rep = node->reps[static_cast<std::size_t>(frame->leaf)];
    if (rep.is_child()) {
      return Status(StatusCode::kCorruptLayout);
    }
    const Position pos = rep.position();
    auto key = Key(pos);
    if (!key.ok()) {
      return key.status();
    }
    return detail::IterItem{key.value(), pos};
  }

  template <class Fn>
  Status AscendFromPath(detail::IterPath& path, detail::IterBound upper, Fn& fn) {
    while (path.len > 0) {
      auto item = CurrentRecord(&path);
      if (!item.ok()) {
        return item.status();
      }
      if (upper.ok && CompareKeys(item.value().key, upper.key) >= 0) {
        return OkStatus();
      }
      if (!static_cast<bool>(std::invoke(fn, item.value().key, item.value().pos))) {
        return OkStatus();
      }
      auto ok = NextPath(&path);
      if (!ok.ok() || !ok.value()) {
        return ok.ok() ? OkStatus() : ok.status();
      }
    }
    return OkStatus();
  }

  template <class Fn>
  Status DescendFromPath(detail::IterPath& path, detail::IterBound lower, Fn& fn) {
    while (path.len > 0) {
      auto item = CurrentRecord(&path);
      if (!item.ok()) {
        return item.status();
      }
      if (lower.ok && CompareKeys(item.value().key, lower.key) <= 0) {
        return OkStatus();
      }
      if (!static_cast<bool>(std::invoke(fn, item.value().key, item.value().pos))) {
        return OkStatus();
      }
      auto ok = PrevPath(&path);
      if (!ok.ok() || !ok.value()) {
        return ok.ok() ? OkStatus() : ok.status();
      }
    }
    return OkStatus();
  }

  RecordStore* records_ = nullptr;
  NodeStore* nodes_ = nullptr;
  std::uint64_t root_id_ = 0;
  int count_ = 0;
  detail::RebuildScratch scratch_;
};

}  // namespace minpatricia
