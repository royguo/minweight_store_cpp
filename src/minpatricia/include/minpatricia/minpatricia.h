#pragma once

#include <memory>
#include <utility>

#include "minpatricia/byte_view.h"
#include "minpatricia/diff.h"
#include "minpatricia/index.h"
#include "minpatricia/node_page.h"
#include "minpatricia/node_store.h"
#include "minpatricia/position.h"
#include "minpatricia/record_store.h"
#include "minpatricia/status.h"

namespace minpatricia {

template <class RecordStore>
struct IndexWithOwnedNodes {
  static_assert(IsRecordStoreLike<RecordStore>::value,
                "RecordStore must provide Result<ByteView> Key(Position)");
  std::unique_ptr<HeapNodeStore> nodes;
  Index<RecordStore, HeapNodeStore> index;
};

template <class RecordStore>
Result<IndexWithOwnedNodes<RecordStore>> NewWithRecords(RecordStore& records) {
  static_assert(IsRecordStoreLike<RecordStore>::value,
                "RecordStore must provide Result<ByteView> Key(Position)");
  auto nodes = std::make_unique<HeapNodeStore>();
  auto index = Index<RecordStore, HeapNodeStore>::NewWithNodes(records, *nodes);
  if (!index.ok()) {
    return index.status();
  }
  return IndexWithOwnedNodes<RecordStore>{std::move(nodes), index.take_value()};
}

template <class V>
struct HeapIndex {
  std::unique_ptr<HeapRecordStore<V>> records;
  std::unique_ptr<HeapNodeStore> nodes;
  Index<HeapRecordStore<V>, HeapNodeStore> index;
};

template <class V>
Result<HeapIndex<V>> NewHeap() {
  auto records = std::make_unique<HeapRecordStore<V>>();
  auto nodes = std::make_unique<HeapNodeStore>();
  auto index = Index<HeapRecordStore<V>, HeapNodeStore>::NewWithNodes(*records, *nodes);
  if (!index.ok()) {
    return index.status();
  }
  return HeapIndex<V>{std::move(records), std::move(nodes), index.take_value()};
}

}  // namespace minpatricia
