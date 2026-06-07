#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <type_traits>
#include <utility>
#include <vector>

#include "minpatricia/node_page.h"
#include "minpatricia/position.h"
#include "minpatricia/status.h"

namespace minpatricia {

struct NodeAlloc {
  std::uint64_t id = 0;
  NodePage* node = nullptr;
};

template <class Store, class = void>
struct IsNodeStoreLike : std::false_type {};

template <class Store>
struct IsNodeStoreLike<
    Store,
    typename std::enable_if<
        std::is_same<decltype(std::declval<Store&>().Root()), std::uint64_t>::value &&
        std::is_same<decltype(std::declval<Store&>().Get(std::declval<std::uint64_t>())),
                     Result<NodePage*>>::value &&
        std::is_same<decltype(std::declval<Store&>().Alloc()), Result<NodeAlloc>>::value &&
        std::is_same<decltype(std::declval<Store&>().Free(std::declval<std::uint64_t>())),
                     Status>::value &&
        std::is_same<decltype(std::declval<Store&>().LiveNodes()), int>::value>::type>
    : std::true_type {};

class HeapNodeStore {
 public:
  HeapNodeStore() {
    nodes_.push_back(std::make_unique<NodePage>());
    live_ = 1;
  }

  [[nodiscard]] std::uint64_t Root() const { return 0; }

  Result<NodePage*> Get(std::uint64_t id) {
    if (id >= nodes_.size() || nodes_[static_cast<std::size_t>(id)] == nullptr) {
      return Status(StatusCode::kCorruptLayout);
    }
    return nodes_[static_cast<std::size_t>(id)].get();
  }

  Result<NodeAlloc> Alloc() {
    if (!free_.empty()) {
      const std::uint64_t id = free_.back();
      free_.pop_back();
      nodes_[static_cast<std::size_t>(id)] = std::make_unique<NodePage>();
      ++live_;
      return NodeAlloc{id, nodes_[static_cast<std::size_t>(id)].get()};
    }

    const std::uint64_t id = nodes_.size();
    if ((id & kChildTag) != 0) {
      return Status(StatusCode::kPositionTag);
    }
    nodes_.push_back(std::make_unique<NodePage>());
    ++live_;
    return NodeAlloc{id, nodes_.back().get()};
  }

  Status Free(std::uint64_t id) {
    if (id == Root() || id >= nodes_.size() ||
        nodes_[static_cast<std::size_t>(id)] == nullptr) {
      return Status(StatusCode::kCorruptLayout);
    }
    nodes_[static_cast<std::size_t>(id)].reset();
    free_.push_back(id);
    --live_;
    return OkStatus();
  }

  [[nodiscard]] int LiveNodes() const { return live_; }

 private:
  std::vector<std::unique_ptr<NodePage>> nodes_;
  std::vector<std::uint64_t> free_;
  int live_ = 0;
};

}  // namespace minpatricia
