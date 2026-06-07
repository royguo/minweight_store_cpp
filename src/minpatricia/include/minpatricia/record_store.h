#pragma once

#include <cstddef>
#include <functional>
#include <type_traits>
#include <utility>
#include <vector>

#include "minpatricia/byte_view.h"
#include "minpatricia/position.h"
#include "minpatricia/status.h"

namespace minpatricia {

class RecordStoreFunc {
 public:
  using Fn = std::function<Result<ByteView>(Position)>;

  RecordStoreFunc() = default;
  explicit RecordStoreFunc(Fn fn) : fn_(std::move(fn)) {}

  Result<ByteView> Key(Position pos) {
    if (!fn_) {
      return Status(StatusCode::kMissingKey);
    }
    return fn_(pos);
  }

 private:
  Fn fn_;
};

template <class Store, class = void>
struct IsRecordStoreLike : std::false_type {};

template <class Store>
struct IsRecordStoreLike<
    Store,
    typename std::enable_if<std::is_same<decltype(std::declval<Store&>().Key(
                                            std::declval<Position>())),
                                        Result<ByteView>>::value>::type> : std::true_type {};

template <class V>
struct HeapRecord {
  std::vector<std::byte> key;
  V value{};
  bool live = false;
};

template <class V>
class HeapRecordStore {
 public:
  HeapRecordStore() { records_.push_back(HeapRecord<V>{}); }

  Position Add(ByteView key, V value) {
    if (!free_.empty()) {
      const Position pos = free_.back();
      free_.pop_back();
      auto& record = records_[static_cast<std::size_t>(pos)];
      record.key = CopyBytes(key);
      record.value = std::move(value);
      record.live = true;
      ++live_;
      return pos;
    }

    const Position pos = static_cast<Position>(records_.size());
    records_.push_back(HeapRecord<V>{CopyBytes(key), std::move(value), true});
    ++live_;
    return pos;
  }

  Status Free(Position pos) {
    if (pos == 0 || pos >= records_.size() ||
        !records_[static_cast<std::size_t>(pos)].live) {
      return Status(StatusCode::kMissingKey);
    }
    records_[static_cast<std::size_t>(pos)] = HeapRecord<V>{};
    free_.push_back(pos);
    --live_;
    return OkStatus();
  }

  Result<ByteView> Key(Position pos) {
    if (pos == 0 || pos >= records_.size()) {
      return Status(StatusCode::kMissingKey);
    }
    auto& record = records_[static_cast<std::size_t>(pos)];
    if (!record.live) {
      return Status(StatusCode::kMissingKey);
    }
    return ByteView(record.key.data(), record.key.size());
  }

  Result<ByteView> Key(Position pos) const {
    if (pos == 0 || pos >= records_.size()) {
      return Status(StatusCode::kMissingKey);
    }
    const auto& record = records_[static_cast<std::size_t>(pos)];
    if (!record.live) {
      return Status(StatusCode::kMissingKey);
    }
    return ByteView(record.key.data(), record.key.size());
  }

  Result<V*> Value(Position pos) {
    if (pos == 0 || pos >= records_.size()) {
      return Status(StatusCode::kMissingKey);
    }
    auto& record = records_[static_cast<std::size_t>(pos)];
    if (!record.live) {
      return Status(StatusCode::kMissingKey);
    }
    return &record.value;
  }

  Result<const V*> Value(Position pos) const {
    if (pos == 0 || pos >= records_.size()) {
      return Status(StatusCode::kMissingKey);
    }
    const auto& record = records_[static_cast<std::size_t>(pos)];
    if (!record.live) {
      return Status(StatusCode::kMissingKey);
    }
    return &record.value;
  }

  Result<HeapRecord<V>*> Record(Position pos) {
    if (pos == 0 || pos >= records_.size()) {
      return Status(StatusCode::kMissingKey);
    }
    auto& record = records_[static_cast<std::size_t>(pos)];
    if (!record.live) {
      return Status(StatusCode::kMissingKey);
    }
    return &record;
  }

  [[nodiscard]] int Len() const { return live_; }

 private:
  std::vector<HeapRecord<V>> records_;
  std::vector<Position> free_;
  int live_ = 0;
};

}  // namespace minpatricia
