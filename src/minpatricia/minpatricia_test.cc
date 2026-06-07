#include "minpatricia/minpatricia.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <random>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {

#define EXPECT_TRUE(expr)                                                        \
  do {                                                                          \
    if (!(expr)) {                                                              \
      std::cerr << "EXPECT_TRUE failed at " << __FILE__ << ":" << __LINE__     \
                << ": " #expr << "\n";                                         \
      std::abort();                                                             \
    }                                                                           \
  } while (false)

#define EXPECT_FALSE(expr) EXPECT_TRUE(!(expr))

#define EXPECT_EQ(a, b)                                                          \
  do {                                                                          \
    const auto va = (a);                                                         \
    const auto vb = (b);                                                         \
    if (!(va == vb)) {                                                           \
      std::cerr << "EXPECT_EQ failed at " << __FILE__ << ":" << __LINE__       \
                << ": " #a " != " #b << "\n";                                  \
      std::abort();                                                             \
    }                                                                           \
  } while (false)

#define EXPECT_STATUS(status, code_value)                                        \
  do {                                                                          \
    const auto st = (status);                                                    \
    if (!(st == (code_value))) {                                                 \
      std::cerr << "EXPECT_STATUS failed at " << __FILE__ << ":" << __LINE__   \
                << ": got " << static_cast<int>(st.code()) << " want "          \
                << static_cast<int>(code_value) << "\n";                        \
      std::abort();                                                             \
    }                                                                           \
  } while (false)

std::vector<std::byte> Bytes(std::string_view value) {
  return minpatricia::CopyBytes(minpatricia::AsBytes(value));
}

std::string TestDataPath(std::string_view name) {
#ifdef MINPATRICIA_TESTDATA_DIR
  return std::string(MINPATRICIA_TESTDATA_DIR) + "/" + std::string(name);
#else
  return "src/minpatricia/testdata/" + std::string(name);
#endif
}

std::vector<std::string> ReadDataLines(std::string_view name) {
  std::ifstream in(TestDataPath(name));
  EXPECT_TRUE(in.good());
  std::vector<std::string> lines;
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty() || line[0] == '#') {
      continue;
    }
    lines.push_back(line);
  }
  return lines;
}

std::vector<std::byte> HexBytes(const std::string& hex) {
  if (hex == "-") {
    return {};
  }
  EXPECT_EQ(hex.size() % 2, static_cast<std::size_t>(0));
  std::vector<std::byte> out;
  out.reserve(hex.size() / 2);
  for (std::size_t i = 0; i < hex.size(); i += 2) {
    const auto value = static_cast<unsigned>(std::stoul(hex.substr(i, 2), nullptr, 16));
    out.push_back(static_cast<std::byte>(value));
  }
  return out;
}

std::string KeyFor(std::uint64_t a, std::uint64_t b, std::string_view prefix = "k") {
  std::ostringstream out;
  out << prefix << "-" << std::hex << std::setw(16) << std::setfill('0') << a << "-"
      << std::setw(16) << b;
  return out.str();
}

struct MemKeys {
  std::map<minpatricia::Position, std::vector<std::byte>> keys;
  minpatricia::Position next_pos = 1;

  minpatricia::Position Add(std::string_view key) {
    const minpatricia::Position pos = next_pos++;
    keys[pos] = Bytes(key);
    return pos;
  }

  minpatricia::Position AddBytes(std::vector<std::byte> key) {
    const minpatricia::Position pos = next_pos++;
    keys[pos] = std::move(key);
    return pos;
  }

  minpatricia::Result<minpatricia::ByteView> Key(minpatricia::Position pos) {
    auto it = keys.find(pos);
    if (it == keys.end()) {
      return minpatricia::Status(minpatricia::StatusCode::kMissingKey);
    }
    return minpatricia::ByteView(it->second.data(), it->second.size());
  }
};

class NonZeroRootNodeStore {
 public:
  explicit NonZeroRootNodeStore(std::uint64_t root) : root_(root), nodes_(root + 1) {
    nodes_[static_cast<std::size_t>(root_)] = std::make_unique<minpatricia::NodePage>();
    live_ = 1;
  }

  std::uint64_t Root() { return root_; }

  minpatricia::Result<minpatricia::NodePage*> Get(std::uint64_t id) {
    if (id >= nodes_.size() || nodes_[static_cast<std::size_t>(id)] == nullptr) {
      return minpatricia::Status(minpatricia::StatusCode::kCorruptLayout);
    }
    return nodes_[static_cast<std::size_t>(id)].get();
  }

  minpatricia::Result<minpatricia::NodeAlloc> Alloc() {
    const std::uint64_t id = nodes_.size();
    nodes_.push_back(std::make_unique<minpatricia::NodePage>());
    ++live_;
    return minpatricia::NodeAlloc{id, nodes_.back().get()};
  }

  minpatricia::Status Free(std::uint64_t id) {
    if (id == root_ || id >= nodes_.size() ||
        nodes_[static_cast<std::size_t>(id)] == nullptr) {
      return minpatricia::Status(minpatricia::StatusCode::kCorruptLayout);
    }
    nodes_[static_cast<std::size_t>(id)].reset();
    --live_;
    return minpatricia::OkStatus();
  }

  int LiveNodes() { return live_; }

  void Clear(std::uint64_t id) {
    if (id < nodes_.size() && nodes_[static_cast<std::size_t>(id)] != nullptr) {
      nodes_[static_cast<std::size_t>(id)].reset();
      --live_;
    }
  }

 private:
  std::uint64_t root_;
  std::vector<std::unique_ptr<minpatricia::NodePage>> nodes_;
  int live_ = 0;
};

using TestIndex = minpatricia::Index<MemKeys>;

struct HeapBuild {
  std::unique_ptr<minpatricia::HeapNodeStore> nodes;
  minpatricia::Index<minpatricia::HeapRecordStore<int>, minpatricia::HeapNodeStore> index;
};

struct TestBenchData {
  std::vector<std::string> keys;
  minpatricia::HeapRecordStore<int> records;
  std::vector<minpatricia::Position> positions;
};

template <class IndexT>
void AssertIndexMatchesMap(IndexT& index,
                           const std::map<std::string, minpatricia::Position>& expected) {
  EXPECT_EQ(index.Len(), static_cast<int>(expected.size()));

  std::vector<std::pair<std::string, minpatricia::Position>> asc;
  EXPECT_TRUE(index.Ascend([&](minpatricia::ByteView key, minpatricia::Position pos) {
    asc.emplace_back(minpatricia::ToString(key), pos);
    return true;
  }).ok());
  EXPECT_EQ(asc.size(), expected.size());

  std::size_t i = 0;
  for (const auto& [key, pos] : expected) {
    EXPECT_EQ(asc[i].first, key);
    EXPECT_EQ(asc[i].second, pos);
    auto got = index.Get(minpatricia::AsBytes(key));
    EXPECT_TRUE(got.ok());
    EXPECT_TRUE(got.value().found);
    EXPECT_EQ(got.value().pos, pos);
    auto probed = index.Probe(minpatricia::AsBytes(key));
    EXPECT_TRUE(probed.ok());
    EXPECT_TRUE(probed.value().found);
    EXPECT_EQ(probed.value().pos, pos);
    ++i;
  }

  std::vector<std::string> desc;
  EXPECT_TRUE(index.Descend([&](minpatricia::ByteView key, minpatricia::Position) {
    desc.push_back(minpatricia::ToString(key));
    return true;
  }).ok());
  std::vector<std::string> want_desc;
  for (auto it = expected.rbegin(); it != expected.rend(); ++it) {
    want_desc.push_back(it->first);
  }
  EXPECT_EQ(desc, want_desc);
}

template <class Records, class Nodes>
minpatricia::Position MinPosForTest(Records& records, Nodes& nodes, minpatricia::Rep rep);

template <class Records, class Nodes>
minpatricia::Position MaxPosForTest(Records& records, Nodes& nodes, minpatricia::Rep rep);

template <class Records, class Nodes>
std::uint16_t DiffBetweenRepsForTest(Records& records, Nodes& nodes, minpatricia::Rep left,
                                     minpatricia::Rep right) {
  const auto left_pos = MaxPosForTest(records, nodes, left);
  const auto right_pos = MinPosForTest(records, nodes, right);
  auto left_key = records.Key(left_pos);
  auto right_key = records.Key(right_pos);
  EXPECT_TRUE(left_key.ok());
  EXPECT_TRUE(right_key.ok());
  auto diff = minpatricia::CompareAndDiffBit(left_key.value(), right_key.value());
  EXPECT_TRUE(diff.ok());
  EXPECT_TRUE(diff.value().compare < 0);
  return diff.value().diff;
}

template <class Records, class Nodes>
minpatricia::Position MinPosForTest(Records& records, Nodes& nodes, minpatricia::Rep rep) {
  if (!rep.is_child()) {
    auto key = records.Key(rep.position());
    EXPECT_TRUE(key.ok());
    return rep.position();
  }
  auto child = nodes.Get(rep.child_id());
  EXPECT_TRUE(child.ok());
  EXPECT_TRUE(child.value()->size > 0);
  return child.value()->first_pos;
}

template <class Records, class Nodes>
minpatricia::Position MaxPosForTest(Records& records, Nodes& nodes, minpatricia::Rep rep) {
  if (!rep.is_child()) {
    auto key = records.Key(rep.position());
    EXPECT_TRUE(key.ok());
    return rep.position();
  }
  auto child = nodes.Get(rep.child_id());
  EXPECT_TRUE(child.ok());
  EXPECT_TRUE(child.value()->size > 0);
  return child.value()->last_pos;
}

template <class Records, class Nodes>
void AssertNodeRoutesValid(Records& records, Nodes& nodes, minpatricia::NodePage* node) {
  const int size = static_cast<int>(node->size);
  if (size > 1) {
    std::array<std::uint16_t, minpatricia::kMaxNodeReps - 1> got_buf{};
    auto got =
        minpatricia::Span<std::uint16_t>(got_buf.data(), static_cast<std::size_t>(size - 1));
    EXPECT_TRUE(node->RouteDiffs(got).ok());
    for (int i = 0; i < size - 1; ++i) {
      const auto want =
          DiffBetweenRepsForTest(records, nodes, node->reps[static_cast<std::size_t>(i)],
                                 node->reps[static_cast<std::size_t>(i + 1)]);
      EXPECT_EQ(got[static_cast<std::size_t>(i)], want);
    }
  }

  for (int i = 0; i < size; ++i) {
    const auto rep = node->reps[static_cast<std::size_t>(i)];
    if (!rep.is_child()) {
      continue;
    }
    auto child = nodes.Get(rep.child_id());
    EXPECT_TRUE(child.ok());
    AssertNodeRoutesValid(records, nodes, child.value());
  }
}

template <class Records, class Nodes>
void AssertAllRoutesValid(Records& records, Nodes& nodes) {
  auto root = nodes.Get(nodes.Root());
  EXPECT_TRUE(root.ok());
  AssertNodeRoutesValid(records, nodes, root.value());
}

TestBenchData NewTestBenchData(int n) {
  TestBenchData data;
  data.keys.reserve(static_cast<std::size_t>(n));
  data.positions.reserve(static_cast<std::size_t>(n));
  std::unordered_set<std::string> seen;
  seen.reserve(static_cast<std::size_t>(n * 2));
  std::mt19937 rng(static_cast<std::uint32_t>(n));

  while (static_cast<int>(data.keys.size()) < n) {
    const std::string key = [&] {
      std::ostringstream out;
      out << "key-" << std::hex << std::setw(8) << std::setfill('0') << rng() << "-"
          << std::setw(8) << rng();
      return out.str();
    }();
    if (!seen.insert(key).second) {
      continue;
    }
    data.positions.push_back(data.records.Add(minpatricia::AsBytes(key),
                                              static_cast<int>(data.keys.size())));
    data.keys.push_back(key);
  }
  return data;
}

HeapBuild BuildHeapPatricia(TestBenchData& data) {
  HeapBuild built;
  built.nodes = std::make_unique<minpatricia::HeapNodeStore>();
  auto index = minpatricia::Index<minpatricia::HeapRecordStore<int>>::NewWithNodes(
      data.records, *built.nodes);
  EXPECT_TRUE(index.ok());
  built.index = index.take_value();
  for (std::size_t i = 0; i < data.keys.size(); ++i) {
    auto put = built.index.Put(minpatricia::AsBytes(data.keys[i]), data.positions[i]);
    EXPECT_TRUE(put.ok());
  }
  return built;
}

void CollectDeleteHeavyPositions(minpatricia::HeapNodeStore& nodes, minpatricia::NodePage* node,
                                 std::vector<minpatricia::Position>* positions) {
  const int size = static_cast<int>(node->size);
  for (int i = 0; i < size; ++i) {
    const auto rep = node->reps[static_cast<std::size_t>(i)];
    if (!rep.is_child()) {
      continue;
    }
    auto child = nodes.Get(rep.child_id());
    EXPECT_TRUE(child.ok());
    const int child_size = static_cast<int>(child.value()->size);
    const int safe_deletes =
        size + child_size - static_cast<int>(minpatricia::kMaxNodeReps) - 2;
    if (safe_deletes > 0) {
      int added = 0;
      for (int j = 1; j + 1 < child_size && added < safe_deletes; ++j) {
        const auto child_rep = child.value()->reps[static_cast<std::size_t>(j)];
        if (child_rep.is_child()) {
          continue;
        }
        positions->push_back(child_rep.position());
        ++added;
      }
    }
    CollectDeleteHeavyPositions(nodes, child.value(), positions);
  }
}

std::vector<minpatricia::Position> DeleteHeavyPositions(TestBenchData& data) {
  auto built = BuildHeapPatricia(data);
  std::vector<minpatricia::Position> positions;
  auto root = built.nodes->Get(built.nodes->Root());
  EXPECT_TRUE(root.ok());
  CollectDeleteHeavyPositions(*built.nodes, root.value(), &positions);
  EXPECT_FALSE(positions.empty());
  return positions;
}

std::vector<std::string> CollectAscendGreaterOrEqual(TestIndex& index, std::string_view pivot) {
  std::vector<std::string> out;
  EXPECT_TRUE(index.AscendGreaterOrEqual(minpatricia::AsBytes(pivot),
                                         [&](minpatricia::ByteView key, minpatricia::Position) {
                                           out.push_back(minpatricia::ToString(key));
                                           return true;
                                         })
                  .ok());
  return out;
}

std::vector<std::string> CollectDescendLessOrEqual(TestIndex& index, std::string_view pivot) {
  std::vector<std::string> out;
  EXPECT_TRUE(index.DescendLessOrEqual(minpatricia::AsBytes(pivot),
                                       [&](minpatricia::ByteView key, minpatricia::Position) {
                                         out.push_back(minpatricia::ToString(key));
                                         return true;
                                       })
                  .ok());
  return out;
}

void TestNodeLayoutAndDiffs() {
  EXPECT_EQ(sizeof(minpatricia::NodePage), minpatricia::kNodeSize);
  EXPECT_TRUE(minpatricia::LayoutBytes(minpatricia::kMaxNodeReps) <=
              static_cast<int>(minpatricia::kNodeSize));
  EXPECT_TRUE(minpatricia::LayoutBytes(minpatricia::kMaxNodeReps + 1) >
              static_cast<int>(minpatricia::kNodeSize));
  EXPECT_EQ(sizeof(minpatricia::Route), static_cast<std::size_t>(4));
  EXPECT_EQ(sizeof(minpatricia::Rep), static_cast<std::size_t>(8));

  auto diff = minpatricia::FindDiffBit(minpatricia::AsBytes("A"), minpatricia::AsBytes("AA"));
  EXPECT_TRUE(diff.ok());
  EXPECT_EQ(diff.value(), static_cast<std::uint16_t>(9));
  EXPECT_EQ(minpatricia::GetDiffBit(minpatricia::AsBytes("A"), diff.value()), 0);
  EXPECT_EQ(minpatricia::GetDiffBit(minpatricia::AsBytes("AA"), diff.value()), 1);

  auto equal = minpatricia::CompareAndDiffBit(minpatricia::AsBytes("x"), minpatricia::AsBytes("x"));
  EXPECT_FALSE(equal.ok());
  EXPECT_STATUS(equal.status(), minpatricia::StatusCode::kEqualKeys);

  minpatricia::Route route = minpatricia::Route::Make(8, 3);
  EXPECT_EQ(route.diff(), static_cast<std::uint16_t>(8));
  EXPECT_EQ(route.left_count(), static_cast<std::uint16_t>(3));
  EXPECT_EQ(route.bit(minpatricia::AsBytes(std::string_view("\001", 1))), 1);
}

void TestHeapStoresAndConstructors() {
  int calls = 0;
  minpatricia::RecordStoreFunc func_records([&](minpatricia::Position pos) {
    ++calls;
    if (pos != 7) {
      return minpatricia::Result<minpatricia::ByteView>(
          minpatricia::Status(minpatricia::StatusCode::kMissingKey));
    }
    return minpatricia::Result<minpatricia::ByteView>(minpatricia::AsBytes("seven"));
  });
  auto func_key = func_records.Key(7);
  EXPECT_TRUE(func_key.ok());
  EXPECT_EQ(minpatricia::ToString(func_key.value()), std::string("seven"));
  EXPECT_EQ(calls, 1);
  EXPECT_FALSE(func_records.Key(8).ok());

  minpatricia::HeapRecordStore<std::string> records;
  EXPECT_FALSE(records.Record(0).ok());
  const auto pos = records.Add(minpatricia::AsBytes("alpha"), "payload");
  EXPECT_EQ(pos, static_cast<minpatricia::Position>(1));
  auto record = records.Record(pos);
  EXPECT_TRUE(record.ok());
  EXPECT_EQ(minpatricia::ToString(minpatricia::ByteView(record.value()->key.data(),
                                                       record.value()->key.size())),
            std::string("alpha"));
  EXPECT_EQ(record.value()->value, std::string("payload"));
  auto key = records.Key(pos);
  EXPECT_TRUE(key.ok());
  EXPECT_EQ(minpatricia::ToString(key.value()), std::string("alpha"));
  auto value = records.Value(pos);
  EXPECT_TRUE(value.ok());
  EXPECT_EQ(*value.value(), std::string("payload"));
  EXPECT_EQ(records.Len(), 1);
  EXPECT_TRUE(records.Free(pos).ok());
  EXPECT_FALSE(records.Key(pos).ok());
  EXPECT_FALSE(records.Record(pos).ok());
  EXPECT_FALSE(records.Value(pos).ok());
  const auto reused = records.Add(minpatricia::AsBytes("bravo"), "second");
  EXPECT_EQ(reused, pos);
  EXPECT_EQ(records.Len(), 1);
  const auto empty_key_pos = records.Add(minpatricia::ByteView{}, "empty");
  auto empty_key = records.Key(empty_key_pos);
  EXPECT_TRUE(empty_key.ok());
  EXPECT_EQ(empty_key.value().size(), static_cast<std::size_t>(0));

  minpatricia::HeapNodeStore nodes;
  auto allocated = nodes.Alloc();
  EXPECT_TRUE(allocated.ok());
  const auto node_id = allocated.value().id;
  allocated.value().node->size = 7;
  EXPECT_TRUE(nodes.Free(node_id).ok());
  EXPECT_FALSE(nodes.Get(node_id).ok());
  auto allocated_again = nodes.Alloc();
  EXPECT_TRUE(allocated_again.ok());
  EXPECT_EQ(allocated_again.value().id, node_id);
  EXPECT_EQ(allocated_again.value().node->size, static_cast<std::uint16_t>(0));
  EXPECT_EQ(nodes.LiveNodes(), 2);

  MemKeys mem;
  auto owned = minpatricia::NewWithRecords(mem);
  EXPECT_TRUE(owned.ok());
  auto owned_index = owned.take_value();
  const auto p = mem.Add("owned");
  EXPECT_TRUE(owned_index.index.Put(minpatricia::AsBytes("owned"), p).ok());
  EXPECT_TRUE(owned_index.index.Get(minpatricia::AsBytes("owned")).value().found);

  auto heap = minpatricia::NewHeap<int>();
  EXPECT_TRUE(heap.ok());
  auto heap_index = heap.take_value();
  const auto hp = heap_index.records->Add(minpatricia::AsBytes("heap"), 42);
  EXPECT_TRUE(heap_index.index.Put(minpatricia::AsBytes("heap"), hp).ok());
  auto hv = heap_index.records->Value(hp);
  EXPECT_TRUE(hv.ok());
  EXPECT_EQ(*hv.value(), 42);
}

void TestRootAndCorruptChildErrors() {
  MemKeys keys;
  NonZeroRootNodeStore nodes(3);
  auto index_result = minpatricia::Index<MemKeys, NonZeroRootNodeStore>::NewWithNodes(keys, nodes);
  EXPECT_TRUE(index_result.ok());
  auto index = index_result.take_value();
  nodes.Clear(nodes.Root());

  EXPECT_STATUS(index.Get(minpatricia::AsBytes("alpha")).status(),
                minpatricia::StatusCode::kCorruptLayout);
  EXPECT_STATUS(index.Probe(minpatricia::AsBytes("alpha")).status(),
                minpatricia::StatusCode::kCorruptLayout);
  EXPECT_STATUS(index.Delete(minpatricia::AsBytes("alpha")).status(),
                minpatricia::StatusCode::kCorruptLayout);
  auto ignore = [](minpatricia::ByteView, minpatricia::Position) { return true; };
  EXPECT_STATUS(index.Ascend(ignore), minpatricia::StatusCode::kCorruptLayout);

  NonZeroRootNodeStore corrupt_open_nodes(1);
  corrupt_open_nodes.Clear(corrupt_open_nodes.Root());
  auto opened =
      minpatricia::Index<MemKeys, NonZeroRootNodeStore>::OpenWithNodes(keys, corrupt_open_nodes);
  EXPECT_STATUS(opened.status(), minpatricia::StatusCode::kCorruptLayout);

  MemKeys child_keys;
  child_keys.keys[1] = Bytes("alpha");
  minpatricia::HeapNodeStore child_nodes;
  auto child_index = TestIndex::NewWithNodes(child_keys, child_nodes).take_value();
  auto child_alloc = child_nodes.Alloc();
  EXPECT_TRUE(child_alloc.ok());
  auto root = child_nodes.Get(child_nodes.Root());
  EXPECT_TRUE(root.ok());
  auto child_rep = minpatricia::Rep::MakeChild(child_alloc.value().id);
  EXPECT_TRUE(child_rep.ok());
  root.value()->size = 1;
  root.value()->reps[0] = child_rep.value();
  root.value()->first_pos = 1;
  root.value()->last_pos = 1;
  EXPECT_STATUS(child_index.Delete(minpatricia::AsBytes("alpha")).status(),
                minpatricia::StatusCode::kCorruptLayout);
}

void TestPutGetDeleteVisit() {
  MemKeys keys;
  minpatricia::HeapNodeStore nodes;
  auto index_result = TestIndex::NewWithNodes(keys, nodes);
  EXPECT_TRUE(index_result.ok());
  auto index = index_result.take_value();

  const std::vector<std::string> inputs = {"delta", "alpha", "charlie", "bravo",
                                           "echo",  "a",     "aa"};
  std::map<std::string, minpatricia::Position> expected;
  for (const auto& key : inputs) {
    const auto pos = keys.Add(key);
    auto put = index.Put(minpatricia::AsBytes(key), pos);
    EXPECT_TRUE(put.ok());
    EXPECT_FALSE(put.value().replaced);
    expected[key] = pos;
  }
  AssertIndexMatchesMap(index, expected);
  AssertAllRoutesValid(keys, nodes);

  const auto replacement = keys.Add("charlie");
  auto replaced = index.Put(minpatricia::AsBytes("charlie"), replacement);
  EXPECT_TRUE(replaced.ok());
  EXPECT_TRUE(replaced.value().replaced);
  EXPECT_EQ(replaced.value().old_pos, expected["charlie"]);
  expected["charlie"] = replacement;
  AssertIndexMatchesMap(index, expected);
  AssertAllRoutesValid(keys, nodes);

  auto deleted = index.Delete(minpatricia::AsBytes("bravo"));
  EXPECT_TRUE(deleted.ok());
  EXPECT_TRUE(deleted.value().deleted);
  EXPECT_EQ(deleted.value().pos, expected["bravo"]);
  expected.erase("bravo");
  AssertIndexMatchesMap(index, expected);
  AssertAllRoutesValid(keys, nodes);

  auto missing = index.Get(minpatricia::AsBytes("missing"));
  EXPECT_TRUE(missing.ok());
  EXPECT_FALSE(missing.value().found);
  auto delete_missing = index.Delete(minpatricia::AsBytes("missing"));
  EXPECT_TRUE(delete_missing.ok());
  EXPECT_FALSE(delete_missing.value().deleted);
}

void TestProbeAndRetarget() {
  MemKeys keys;
  keys.keys[1] = Bytes("alpha");
  keys.keys[2] = Bytes("bravo");
  keys.keys[9] = Bytes("alpha");

  int calls = 0;
  minpatricia::RecordStoreFunc records([&](minpatricia::Position pos) {
    ++calls;
    auto it = keys.keys.find(pos);
    if (it == keys.keys.end()) {
      return minpatricia::Result<minpatricia::ByteView>(
          minpatricia::Status(minpatricia::StatusCode::kMissingKey));
    }
    return minpatricia::Result<minpatricia::ByteView>(
        minpatricia::ByteView(it->second.data(), it->second.size()));
  });
  minpatricia::HeapNodeStore nodes;
  auto index_result = minpatricia::Index<minpatricia::RecordStoreFunc>::NewWithNodes(records, nodes);
  EXPECT_TRUE(index_result.ok());
  auto index = index_result.take_value();

  calls = 0;
  auto first = index.Put(minpatricia::AsBytes("alpha"), 1);
  EXPECT_TRUE(first.ok());
  EXPECT_FALSE(first.value().replaced);
  EXPECT_EQ(calls, 0);
  EXPECT_TRUE(index.Put(minpatricia::AsBytes("bravo"), 2).ok());

  calls = 0;
  auto probe = index.Probe(minpatricia::AsBytes("omega"));
  EXPECT_TRUE(probe.ok());
  EXPECT_TRUE(probe.value().found);
  EXPECT_EQ(calls, 0);
  auto get = index.Get(minpatricia::AsBytes("omega"));
  EXPECT_TRUE(get.ok());
  EXPECT_FALSE(get.value().found);
  EXPECT_EQ(calls, 1);

  calls = 0;
  auto replaced = index.Put(minpatricia::AsBytes("alpha"), 9);
  EXPECT_TRUE(replaced.ok());
  EXPECT_TRUE(replaced.value().replaced);
  EXPECT_EQ(replaced.value().old_pos, static_cast<minpatricia::Position>(1));
  EXPECT_EQ(calls, 1);

  calls = 0;
  EXPECT_TRUE(index.Retarget(minpatricia::AsBytes("alpha"), 9, 1).ok());
  EXPECT_EQ(calls, 0);
  auto got = index.Get(minpatricia::AsBytes("alpha"));
  EXPECT_TRUE(got.ok());
  EXPECT_TRUE(got.value().found);
  EXPECT_EQ(got.value().pos, static_cast<minpatricia::Position>(1));
  EXPECT_STATUS(index.Retarget(minpatricia::AsBytes("alpha"), 9, 10),
                minpatricia::StatusCode::kPositionMismatch);
  EXPECT_STATUS(index.Retarget(minpatricia::AsBytes("alpha"), 1, minpatricia::kChildTag),
                minpatricia::StatusCode::kPositionTag);
}

void TestIteratorAPI() {
  MemKeys keys;
  minpatricia::HeapNodeStore nodes;
  auto index = TestIndex::NewWithNodes(keys, nodes).take_value();
  std::map<std::string, minpatricia::Position> expected;
  for (const auto& key : {"delta", "alpha", "charlie", "bravo", "echo"}) {
    expected[key] = keys.Add(key);
    EXPECT_TRUE(index.Put(minpatricia::AsBytes(key), expected[key]).ok());
  }

  auto collect_asc_range = [&](std::string_view lo, std::string_view hi) {
    std::vector<std::string> out;
    EXPECT_TRUE(index.AscendRange(minpatricia::AsBytes(lo), minpatricia::AsBytes(hi),
                                  [&](minpatricia::ByteView key, minpatricia::Position) {
                                    out.push_back(minpatricia::ToString(key));
                                    return true;
                                  })
                    .ok());
    return out;
  };
  auto collect_desc_range = [&](std::string_view hi, std::string_view lo) {
    std::vector<std::string> out;
    EXPECT_TRUE(index.DescendRange(minpatricia::AsBytes(hi), minpatricia::AsBytes(lo),
                                   [&](minpatricia::ByteView key, minpatricia::Position) {
                                     out.push_back(minpatricia::ToString(key));
                                     return true;
                                   })
                    .ok());
    return out;
  };

  EXPECT_EQ(CollectAscendGreaterOrEqual(index, "charlie"),
            (std::vector<std::string>{"charlie", "delta", "echo"}));
  EXPECT_EQ(CollectAscendGreaterOrEqual(index, "caper"),
            (std::vector<std::string>{"charlie", "delta", "echo"}));
  EXPECT_EQ(CollectAscendGreaterOrEqual(index, "zulu"), (std::vector<std::string>{}));
  EXPECT_EQ(CollectDescendLessOrEqual(index, "caper"),
            (std::vector<std::string>{"bravo", "alpha"}));
  EXPECT_EQ(CollectDescendLessOrEqual(index, "aardvark"), (std::vector<std::string>{}));
  EXPECT_EQ(collect_asc_range("bravo", "echo"),
            (std::vector<std::string>{"bravo", "charlie", "delta"}));
  EXPECT_EQ(collect_asc_range("caper", "echo"),
            (std::vector<std::string>{"charlie", "delta"}));
  std::vector<std::string> ascend_less_than;
  EXPECT_TRUE(index.AscendLessThan(minpatricia::AsBytes("delta"),
                                   [&](minpatricia::ByteView key, minpatricia::Position) {
                                     ascend_less_than.push_back(minpatricia::ToString(key));
                                     return true;
                                   })
                  .ok());
  EXPECT_EQ(ascend_less_than, (std::vector<std::string>{"alpha", "bravo", "charlie"}));
  std::vector<std::string> descend_all;
  EXPECT_TRUE(index.Descend([&](minpatricia::ByteView key, minpatricia::Position) {
    descend_all.push_back(minpatricia::ToString(key));
    return true;
  }).ok());
  EXPECT_EQ(descend_all,
            (std::vector<std::string>{"echo", "delta", "charlie", "bravo", "alpha"}));
  EXPECT_EQ(collect_desc_range("delta", "bravo"),
            (std::vector<std::string>{"delta", "charlie"}));
  EXPECT_EQ(collect_desc_range("dazzle", "bravo"), (std::vector<std::string>{"charlie"}));
  EXPECT_EQ(CollectDescendLessOrEqual(index, "delta"),
            (std::vector<std::string>{"delta", "charlie", "bravo", "alpha"}));
  EXPECT_EQ(CollectDescendLessOrEqual(index, "zulu"),
            (std::vector<std::string>{"echo", "delta", "charlie", "bravo", "alpha"}));
  std::vector<std::string> descend_gt;
  EXPECT_TRUE(index.DescendGreaterThan(minpatricia::AsBytes("bravo"),
                                       [&](minpatricia::ByteView key, minpatricia::Position) {
                                         descend_gt.push_back(minpatricia::ToString(key));
                                         return true;
                                       })
                  .ok());
  EXPECT_EQ(descend_gt, (std::vector<std::string>{"echo", "delta", "charlie"}));

  std::vector<std::string> stopped;
  EXPECT_TRUE(index.Ascend([&](minpatricia::ByteView key, minpatricia::Position) {
    stopped.push_back(minpatricia::ToString(key));
    return stopped.size() < 2;
  }).ok());
  EXPECT_EQ(stopped, (std::vector<std::string>{"alpha", "bravo"}));
}

void TestOpenWithNodesAndNonZeroRoot() {
  MemKeys keys;
  minpatricia::HeapNodeStore nodes;
  auto index = TestIndex::NewWithNodes(keys, nodes).take_value();
  std::map<std::string, minpatricia::Position> expected;
  for (const auto& key : {"alpha", "bravo", "charlie"}) {
    expected[key] = keys.Add(key);
    EXPECT_TRUE(index.Put(minpatricia::AsBytes(key), expected[key]).ok());
  }
  auto reopened = TestIndex::OpenWithNodes(keys, nodes);
  EXPECT_TRUE(reopened.ok());
  auto reopened_index = reopened.take_value();
  AssertIndexMatchesMap(reopened_index, expected);

  MemKeys root_keys;
  NonZeroRootNodeStore root_nodes(3);
  auto root_index_result =
      minpatricia::Index<MemKeys, NonZeroRootNodeStore>::NewWithNodes(root_keys, root_nodes);
  EXPECT_TRUE(root_index_result.ok());
  auto root_index = root_index_result.take_value();
  const auto alpha = root_keys.Add("alpha");
  const auto bravo = root_keys.Add("bravo");
  const auto charlie = root_keys.Add("charlie");
  EXPECT_TRUE(root_index.Put(minpatricia::AsBytes("alpha"), alpha).ok());
  EXPECT_TRUE(root_index.Put(minpatricia::AsBytes("bravo"), bravo).ok());
  EXPECT_TRUE(root_index.Put(minpatricia::AsBytes("charlie"), charlie).ok());
  auto deleted = root_index.Delete(minpatricia::AsBytes("bravo"));
  EXPECT_TRUE(deleted.ok());
  EXPECT_TRUE(deleted.value().deleted);
  EXPECT_EQ(deleted.value().pos, bravo);
  EXPECT_EQ(root_index.Len(), 2);
}

void TestMultiNodeAgainstMap() {
  MemKeys keys;
  minpatricia::HeapNodeStore nodes;
  auto index = TestIndex::NewWithNodes(keys, nodes).take_value();
  std::map<std::string, minpatricia::Position> expected;
  std::vector<std::string> sorted_keys;
  std::mt19937_64 rng(3);

  while (expected.size() < 2500) {
    const std::string key = KeyFor(rng(), rng());
    if (expected.find(key) != expected.end()) {
      continue;
    }
    const auto pos = keys.Add(key);
    auto put = index.Put(minpatricia::AsBytes(key), pos);
    EXPECT_TRUE(put.ok());
    EXPECT_FALSE(put.value().replaced);
    expected[key] = pos;
    sorted_keys.push_back(key);
  }
  std::sort(sorted_keys.begin(), sorted_keys.end());
  EXPECT_TRUE(index.LiveNodes() >= 2);
  AssertIndexMatchesMap(index, expected);
  AssertAllRoutesValid(keys, nodes);

  for (std::size_t i = 0; i < sorted_keys.size(); i += 7) {
    const std::string& key = sorted_keys[i];
    const auto pos = keys.Add(key);
    auto put = index.Put(minpatricia::AsBytes(key), pos);
    EXPECT_TRUE(put.ok());
    EXPECT_TRUE(put.value().replaced);
    EXPECT_EQ(put.value().old_pos, expected[key]);
    expected[key] = pos;
  }
  AssertIndexMatchesMap(index, expected);
  AssertAllRoutesValid(keys, nodes);

  for (std::size_t i = 0; i < sorted_keys.size(); i += 3) {
    const std::string& key = sorted_keys[i];
    auto it = expected.find(key);
    if (it == expected.end()) {
      continue;
    }
    const auto want = it->second;
    auto deleted = index.Delete(minpatricia::AsBytes(key));
    EXPECT_TRUE(deleted.ok());
    EXPECT_TRUE(deleted.value().deleted);
    EXPECT_EQ(deleted.value().pos, want);
    expected.erase(it);
  }
  AssertIndexMatchesMap(index, expected);
  AssertAllRoutesValid(keys, nodes);
}

void TestIteratorRangeMultiNode() {
  MemKeys keys;
  minpatricia::HeapNodeStore nodes;
  auto index = TestIndex::NewWithNodes(keys, nodes).take_value();
  std::map<std::string, minpatricia::Position> expected;
  std::mt19937_64 rng(7);

  while (expected.size() < 2500) {
    const std::string key = KeyFor(rng(), rng(), "range");
    if (expected.find(key) != expected.end()) {
      continue;
    }
    const auto pos = keys.Add(key);
    EXPECT_TRUE(index.Put(minpatricia::AsBytes(key), pos).ok());
    expected[key] = pos;
  }
  std::vector<std::string> sorted_keys;
  for (const auto& [key, _] : expected) {
    sorted_keys.push_back(key);
  }

  const std::string lo = sorted_keys[321];
  const std::string hi = sorted_keys[1987];
  std::vector<std::string> asc_range;
  EXPECT_TRUE(index.AscendRange(minpatricia::AsBytes(lo), minpatricia::AsBytes(hi),
                                [&](minpatricia::ByteView key, minpatricia::Position) {
                                  asc_range.push_back(minpatricia::ToString(key));
                                  return true;
                                })
                  .ok());
  EXPECT_EQ(asc_range,
            std::vector<std::string>(sorted_keys.begin() + 321, sorted_keys.begin() + 1987));

  std::vector<std::string> probes = {
      "range-0000000000000000-0000000000000000",
      sorted_keys[0],
      sorted_keys[100] + "x",
      sorted_keys.back(),
      "range-ffffffffffffffff-ffffffffffffffff",
  };
  for (int i = 0; i < 100; ++i) {
    probes.push_back(KeyFor(rng(), rng(), "range"));
  }
  for (const auto& probe : probes) {
    const auto want = expected.lower_bound(probe);
    std::string got;
    EXPECT_TRUE(index.AscendGreaterOrEqual(
                         minpatricia::AsBytes(probe),
                         [&](minpatricia::ByteView key, minpatricia::Position) {
                           got = minpatricia::ToString(key);
                           return false;
                         })
                    .ok());
    EXPECT_EQ(got, want == expected.end() ? std::string() : want->first);
  }
  for (const auto& probe : probes) {
    auto it = expected.upper_bound(probe);
    std::string want;
    if (it != expected.begin()) {
      --it;
      want = it->first;
    }
    std::string got;
    EXPECT_TRUE(index.DescendLessOrEqual(
                         minpatricia::AsBytes(probe),
                         [&](minpatricia::ByteView key, minpatricia::Position) {
                           got = minpatricia::ToString(key);
                           return false;
                         })
                    .ok());
    EXPECT_EQ(got, want);
  }

  std::vector<std::string> want_desc;
  for (int i = 1986; i > 321; --i) {
    want_desc.push_back(sorted_keys[static_cast<std::size_t>(i)]);
  }
  std::vector<std::string> got_desc;
  EXPECT_TRUE(index.DescendRange(minpatricia::AsBytes(sorted_keys[1986]),
                                 minpatricia::AsBytes(sorted_keys[321]),
                                 [&](minpatricia::ByteView key, minpatricia::Position) {
                                   got_desc.push_back(minpatricia::ToString(key));
                                   return true;
                                 })
                  .ok());
  EXPECT_EQ(got_desc, want_desc);
  AssertAllRoutesValid(keys, nodes);
}

void TestCartesianRouteAgainstSortedMap() {
  MemKeys keys;
  minpatricia::HeapNodeStore nodes;
  auto index = TestIndex::NewWithNodes(keys, nodes).take_value();
  std::map<std::string, minpatricia::Position> expected;
  std::mt19937 rng(2);

  while (expected.size() < minpatricia::kMaxNodeReps) {
    std::ostringstream out;
    out << "key-" << std::hex << std::setw(8) << std::setfill('0') << rng();
    const std::string key = out.str();
    if (expected.find(key) != expected.end()) {
      continue;
    }
    const auto pos = keys.Add(key);
    EXPECT_TRUE(index.Put(minpatricia::AsBytes(key), pos).ok());
    expected[key] = pos;
  }
  AssertIndexMatchesMap(index, expected);
  AssertAllRoutesValid(keys, nodes);

  const auto overflow = keys.Add("overflow");
  auto put = index.Put(minpatricia::AsBytes("overflow"), overflow);
  EXPECT_TRUE(put.ok());
  EXPECT_FALSE(put.value().replaced);
  expected["overflow"] = overflow;
  EXPECT_EQ(index.Len(), static_cast<int>(minpatricia::kMaxNodeReps + 1));
  EXPECT_TRUE(index.LiveNodes() >= 2);
  AssertIndexMatchesMap(index, expected);
  AssertAllRoutesValid(keys, nodes);
}

void TestDeleteAllMaintainsRoutes() {
  MemKeys keys;
  minpatricia::HeapNodeStore nodes;
  auto index = TestIndex::NewWithNodes(keys, nodes).take_value();
  std::map<std::string, minpatricia::Position> expected;
  std::vector<std::string> delete_order;
  std::mt19937 rng(11);

  while (expected.size() < 700) {
    std::ostringstream out;
    out << "delete-" << std::hex << std::setw(8) << std::setfill('0') << rng() << "-"
        << std::setw(8) << rng();
    const std::string key = out.str();
    if (expected.find(key) != expected.end()) {
      continue;
    }
    const auto pos = keys.Add(key);
    auto put = index.Put(minpatricia::AsBytes(key), pos);
    EXPECT_TRUE(put.ok());
    EXPECT_FALSE(put.value().replaced);
    expected[key] = pos;
    delete_order.push_back(key);
  }
  std::shuffle(delete_order.begin(), delete_order.end(), rng);

  for (std::size_t i = 0; i < delete_order.size(); ++i) {
    const auto& key = delete_order[i];
    const auto want = expected[key];
    auto deleted = index.Delete(minpatricia::AsBytes(key));
    EXPECT_TRUE(deleted.ok());
    EXPECT_TRUE(deleted.value().deleted);
    EXPECT_EQ(deleted.value().pos, want);
    expected.erase(key);
    if (i % 17 == 0 || expected.empty()) {
      AssertIndexMatchesMap(index, expected);
      AssertAllRoutesValid(keys, nodes);
    }
  }
}

void TestDeleteHeavyKeepsLookupConsistent() {
  auto data = NewTestBenchData(100000);
  const auto positions = DeleteHeavyPositions(data);
  auto built = BuildHeapPatricia(data);
  std::map<std::string, minpatricia::Position> expected;
  for (std::size_t i = 0; i < data.keys.size(); ++i) {
    expected[data.keys[i]] = data.positions[i];
  }

  for (const auto pos : positions) {
    auto key_result = data.records.Key(pos);
    EXPECT_TRUE(key_result.ok());
    const std::string key = minpatricia::ToString(key_result.value());
    auto deleted = built.index.Delete(minpatricia::AsBytes(key));
    EXPECT_TRUE(deleted.ok());
    EXPECT_TRUE(deleted.value().deleted);
    auto got = built.index.Get(minpatricia::AsBytes(key));
    EXPECT_TRUE(got.ok());
    EXPECT_FALSE(got.value().found);
    expected.erase(key);
  }

  AssertIndexMatchesMap(built.index, expected);
  AssertAllRoutesValid(data.records, *built.nodes);
}

void TestBoundaryReplaceAndRetarget() {
  auto heap = minpatricia::NewHeap<int>();
  EXPECT_TRUE(heap.ok());
  auto owned = heap.take_value();
  auto& index = owned.index;
  auto& records = *owned.records;
  auto& nodes = *owned.nodes;
  std::map<std::string, minpatricia::Position> expected;
  std::mt19937_64 rng(13);

  while (expected.size() < 2500) {
    const std::string key = KeyFor(rng(), rng(), "boundary");
    if (expected.find(key) != expected.end()) {
      continue;
    }
    const auto pos = records.Add(minpatricia::AsBytes(key), static_cast<int>(expected.size()));
    auto put = index.Put(minpatricia::AsBytes(key), pos);
    EXPECT_TRUE(put.ok());
    expected[key] = pos;
  }
  AssertIndexMatchesMap(index, expected);

  auto root = nodes.Get(nodes.Root());
  EXPECT_TRUE(root.ok());
  int replace_slot = -1;
  for (int i = 0; i + 1 < static_cast<int>(root.value()->size); ++i) {
    if (root.value()->reps[static_cast<std::size_t>(i)].is_child()) {
      replace_slot = i;
      break;
    }
  }
  if (replace_slot == -1) {
    for (int i = 0; i < static_cast<int>(root.value()->size); ++i) {
      if (root.value()->reps[static_cast<std::size_t>(i)].is_child()) {
        replace_slot = i;
        break;
      }
    }
  }
  EXPECT_TRUE(replace_slot >= 0);

  auto child = nodes.Get(root.value()->reps[static_cast<std::size_t>(replace_slot)].child_id());
  EXPECT_TRUE(child.ok());
  const auto old_child_last = child.value()->last_pos;
  auto old_child_last_key = records.Key(old_child_last);
  EXPECT_TRUE(old_child_last_key.ok());
  const std::string replace_key = minpatricia::ToString(old_child_last_key.value());
  const auto new_child_last = records.Add(minpatricia::AsBytes(replace_key), 777);
  auto replaced = index.Put(minpatricia::AsBytes(replace_key), new_child_last);
  EXPECT_TRUE(replaced.ok());
  EXPECT_TRUE(replaced.value().replaced);
  EXPECT_EQ(replaced.value().old_pos, old_child_last);
  EXPECT_TRUE(records.Free(old_child_last).ok());
  expected[replace_key] = new_child_last;
  AssertIndexMatchesMap(index, expected);

  root = nodes.Get(nodes.Root());
  EXPECT_TRUE(root.ok());
  int edge_slot = -1;
  bool use_first = true;
  if (root.value()->reps[0].is_child()) {
    edge_slot = 0;
  } else {
    const int last = static_cast<int>(root.value()->size) - 1;
    if (root.value()->reps[static_cast<std::size_t>(last)].is_child()) {
      edge_slot = last;
      use_first = false;
    }
  }
  EXPECT_TRUE(edge_slot >= 0);
  auto edge_child = nodes.Get(root.value()->reps[static_cast<std::size_t>(edge_slot)].child_id());
  EXPECT_TRUE(edge_child.ok());
  const auto old_boundary = use_first ? edge_child.value()->first_pos : edge_child.value()->last_pos;
  auto old_boundary_key = records.Key(old_boundary);
  EXPECT_TRUE(old_boundary_key.ok());
  const std::string retarget_key = minpatricia::ToString(old_boundary_key.value());
  const auto new_boundary = records.Add(minpatricia::AsBytes(retarget_key), 888);
  EXPECT_TRUE(index.Retarget(minpatricia::AsBytes(retarget_key), old_boundary, new_boundary).ok());
  EXPECT_TRUE(records.Free(old_boundary).ok());
  expected[retarget_key] = new_boundary;
  AssertIndexMatchesMap(index, expected);
}

void TestDeterministicModelOps() {
  MemKeys keys;
  minpatricia::HeapNodeStore nodes;
  auto index = TestIndex::NewWithNodes(keys, nodes).take_value();
  std::map<std::string, minpatricia::Position> expected;
  std::mt19937_64 rng(19);

  auto random_key = [&]() {
    const int len = static_cast<int>(rng() % 33);
    std::string key;
    key.reserve(static_cast<std::size_t>(len));
    for (int i = 0; i < len; ++i) {
      key.push_back(static_cast<char>('a' + (rng() % 26)));
    }
    return key;
  };

  for (int step = 0; step < 3000; ++step) {
    const int op = static_cast<int>(rng() % 8);
    const std::string key = random_key();
    if (op <= 1) {
      const auto pos = keys.Add(key);
      auto put = index.Put(minpatricia::AsBytes(key), pos);
      EXPECT_TRUE(put.ok());
      const auto old = expected.find(key);
      EXPECT_EQ(put.value().replaced, old != expected.end());
      if (old != expected.end()) {
        EXPECT_EQ(put.value().old_pos, old->second);
      }
      expected[key] = pos;
    } else if (op == 2) {
      const auto old = expected.find(key);
      auto del = index.Delete(minpatricia::AsBytes(key));
      EXPECT_TRUE(del.ok());
      EXPECT_EQ(del.value().deleted, old != expected.end());
      if (old != expected.end()) {
        EXPECT_EQ(del.value().pos, old->second);
        expected.erase(old);
      }
    } else if (op == 3) {
      const auto old = expected.find(key);
      auto got = index.Get(minpatricia::AsBytes(key));
      EXPECT_TRUE(got.ok());
      EXPECT_EQ(got.value().found, old != expected.end());
      if (old != expected.end()) {
        EXPECT_EQ(got.value().pos, old->second);
      }
    } else if (op == 4) {
      auto got = CollectAscendGreaterOrEqual(index, key);
      std::vector<std::string> want;
      for (auto it = expected.lower_bound(key); it != expected.end(); ++it) {
        want.push_back(it->first);
      }
      EXPECT_EQ(got, want);
    } else if (op == 5) {
      auto got = CollectDescendLessOrEqual(index, key);
      std::vector<std::string> want;
      auto it = expected.upper_bound(key);
      while (it != expected.begin()) {
        --it;
        want.push_back(it->first);
      }
      EXPECT_EQ(got, want);
    } else if (op == 6) {
      const std::string hi = random_key();
      std::vector<std::string> got;
      EXPECT_TRUE(index.AscendRange(minpatricia::AsBytes(key), minpatricia::AsBytes(hi),
                                    [&](minpatricia::ByteView k, minpatricia::Position) {
                                      got.push_back(minpatricia::ToString(k));
                                      return true;
                                    })
                      .ok());
      std::vector<std::string> want;
      for (auto it = expected.lower_bound(key); it != expected.end() && it->first < hi; ++it) {
        want.push_back(it->first);
      }
      EXPECT_EQ(got, want);
    } else {
      const std::string lo = random_key();
      std::vector<std::string> got;
      EXPECT_TRUE(index.DescendRange(minpatricia::AsBytes(key), minpatricia::AsBytes(lo),
                                     [&](minpatricia::ByteView k, minpatricia::Position) {
                                       got.push_back(minpatricia::ToString(k));
                                       return true;
                                     })
                      .ok());
      std::vector<std::string> want;
      auto it = expected.upper_bound(key);
      while (it != expected.begin()) {
        --it;
        if (it->first <= lo) {
          break;
        }
        want.push_back(it->first);
      }
      EXPECT_EQ(got, want);
    }

    if (step % 31 == 0) {
      AssertIndexMatchesMap(index, expected);
    }
  }
  AssertIndexMatchesMap(index, expected);
}

struct FuzzStream {
  std::vector<unsigned char> data;
  std::size_t off = 0;

  bool More() const { return off < data.size(); }

  unsigned char NextByte() {
    EXPECT_TRUE(More());
    return data[off++];
  }

  std::string NextKey() {
    if (!More()) {
      return {};
    }
    std::size_t n = NextByte() % 33;
    if (n > data.size() - off) {
      n = data.size() - off;
    }
    std::string key;
    key.resize(n);
    for (std::size_t i = 0; i < n; ++i) {
      key[i] = static_cast<char>(data[off + i]);
    }
    off += n;
    return key;
  }
};

std::vector<unsigned char> FuzzSplitSeed(int count) {
  std::vector<unsigned char> seed;
  seed.reserve(static_cast<std::size_t>(count * 4));
  for (int i = 0; i < count; ++i) {
    seed.push_back(0);
    seed.push_back(2);
    seed.push_back(static_cast<unsigned char>(i >> 8));
    seed.push_back(static_cast<unsigned char>(i));
  }
  return seed;
}

void RunFuzzSeed(const std::vector<unsigned char>& seed) {
  MemKeys keys;
  minpatricia::HeapNodeStore nodes;
  auto index = TestIndex::NewWithNodes(keys, nodes).take_value();
  std::map<std::string, minpatricia::Position> expected;
  FuzzStream stream{seed, 0};

  for (int step = 0; step < 512 && stream.More(); ++step) {
    const unsigned char op = stream.NextByte();
    const std::string key = stream.NextKey();
    switch (op % 8) {
      case 0:
      case 1: {
        const auto pos = keys.Add(key);
        auto put = index.Put(minpatricia::AsBytes(key), pos);
        EXPECT_TRUE(put.ok());
        const auto old = expected.find(key);
        EXPECT_EQ(put.value().replaced, old != expected.end());
        if (old != expected.end()) {
          EXPECT_EQ(put.value().old_pos, old->second);
        }
        expected[key] = pos;
        break;
      }
      case 2: {
        const auto old = expected.find(key);
        auto deleted = index.Delete(minpatricia::AsBytes(key));
        EXPECT_TRUE(deleted.ok());
        EXPECT_EQ(deleted.value().deleted, old != expected.end());
        if (old != expected.end()) {
          EXPECT_EQ(deleted.value().pos, old->second);
          expected.erase(old);
          auto got = index.Get(minpatricia::AsBytes(key));
          EXPECT_TRUE(got.ok());
          EXPECT_FALSE(got.value().found);
        }
        break;
      }
      case 3: {
        const auto old = expected.find(key);
        auto got = index.Get(minpatricia::AsBytes(key));
        EXPECT_TRUE(got.ok());
        EXPECT_EQ(got.value().found, old != expected.end());
        if (old != expected.end()) {
          EXPECT_EQ(got.value().pos, old->second);
        }
        break;
      }
      case 4: {
        const auto want = expected.lower_bound(key);
        std::string got_key;
        minpatricia::Position got_pos = 0;
        bool got_ok = false;
        EXPECT_TRUE(index.AscendGreaterOrEqual(
                             minpatricia::AsBytes(key),
                             [&](minpatricia::ByteView found_key, minpatricia::Position pos) {
                               got_key = minpatricia::ToString(found_key);
                               got_pos = pos;
                               got_ok = true;
                               return false;
                             })
                        .ok());
        EXPECT_EQ(got_ok, want != expected.end());
        if (want != expected.end()) {
          EXPECT_EQ(got_key, want->first);
          EXPECT_EQ(got_pos, want->second);
        }
        break;
      }
      case 5: {
        auto want_it = expected.upper_bound(key);
        bool want_ok = false;
        std::string want_key;
        minpatricia::Position want_pos = 0;
        if (want_it != expected.begin()) {
          --want_it;
          want_ok = true;
          want_key = want_it->first;
          want_pos = want_it->second;
        }
        std::string got_key;
        minpatricia::Position got_pos = 0;
        bool got_ok = false;
        EXPECT_TRUE(index.DescendLessOrEqual(
                             minpatricia::AsBytes(key),
                             [&](minpatricia::ByteView found_key, minpatricia::Position pos) {
                               got_key = minpatricia::ToString(found_key);
                               got_pos = pos;
                               got_ok = true;
                               return false;
                             })
                        .ok());
        EXPECT_EQ(got_ok, want_ok);
        if (want_ok) {
          EXPECT_EQ(got_key, want_key);
          EXPECT_EQ(got_pos, want_pos);
        }
        break;
      }
      case 6: {
        const std::string hi = stream.NextKey();
        std::vector<std::string> got;
        EXPECT_TRUE(index.AscendRange(
                             minpatricia::AsBytes(key), minpatricia::AsBytes(hi),
                             [&](minpatricia::ByteView found_key, minpatricia::Position) {
                               got.push_back(minpatricia::ToString(found_key));
                               return true;
                             })
                        .ok());
        std::vector<std::string> want;
        for (auto it = expected.lower_bound(key); it != expected.end() && it->first < hi; ++it) {
          want.push_back(it->first);
        }
        EXPECT_EQ(got, want);
        break;
      }
      case 7: {
        const std::string lo = stream.NextKey();
        std::vector<std::string> got;
        EXPECT_TRUE(index.DescendRange(
                             minpatricia::AsBytes(key), minpatricia::AsBytes(lo),
                             [&](minpatricia::ByteView found_key, minpatricia::Position) {
                               got.push_back(minpatricia::ToString(found_key));
                               return true;
                             })
                        .ok());
        std::vector<std::string> want;
        auto it = expected.upper_bound(key);
        while (it != expected.begin()) {
          --it;
          if (it->first <= lo) {
            break;
          }
          want.push_back(it->first);
        }
        EXPECT_EQ(got, want);
        break;
      }
    }

    if (step % 31 == 0) {
      AssertIndexMatchesMap(index, expected);
      AssertAllRoutesValid(keys, nodes);
    }
  }
  AssertIndexMatchesMap(index, expected);
  AssertAllRoutesValid(keys, nodes);
}

void TestGoFuzzSeedsAgainstMap() {
  RunFuzzSeed({
      0, 5, 'a', 'l', 'p', 'h', 'a',
      0, 5, 'b', 'r', 'a', 'v', 'o',
      0, 5, 'a', 'l', 'p', 'h', 'a',
      2, 5, 'b', 'r', 'a', 'v', 'o',
      3, 5, 'a', 'l', 'p', 'h', 'a',
      4, 1, 'a',
      5, 1, 'z',
      6, 1, 'a', 1, 'z',
      7, 1, 'z', 1, 'a',
  });
  RunFuzzSeed({
      0, 0,
      0, 1, 0,
      0, 2, 0, 0,
      0, 3, 0, 0, 0,
      6, 0, 4, 0, 0, 0, 1,
      7, 4, 0, 0, 0, 1, 0,
  });
  RunFuzzSeed(FuzzSplitSeed(static_cast<int>(minpatricia::kMaxNodeReps) + 1));
}

void TestErrorPaths() {
  MemKeys keys;
  minpatricia::HeapNodeStore nodes;
  auto index = TestIndex::NewWithNodes(keys, nodes).take_value();

  std::vector<std::byte> too_large(minpatricia::kMaxKeySize + 1);
  EXPECT_STATUS(index.Get(too_large).status(), minpatricia::StatusCode::kKeyTooLarge);
  EXPECT_STATUS(index.Probe(too_large).status(), minpatricia::StatusCode::kKeyTooLarge);
  EXPECT_STATUS(index.Put(too_large, 1).status(), minpatricia::StatusCode::kKeyTooLarge);
  EXPECT_STATUS(index.Delete(too_large).status(), minpatricia::StatusCode::kKeyTooLarge);
  EXPECT_STATUS(index.Retarget(too_large, 1, 2), minpatricia::StatusCode::kKeyTooLarge);
  auto ignore = [](minpatricia::ByteView, minpatricia::Position) { return true; };
  EXPECT_STATUS(index.AscendRange(too_large, minpatricia::AsBytes(""), ignore),
                minpatricia::StatusCode::kKeyTooLarge);
  EXPECT_STATUS(index.AscendRange(minpatricia::AsBytes(""), too_large, ignore),
                minpatricia::StatusCode::kKeyTooLarge);
  EXPECT_STATUS(index.DescendRange(too_large, minpatricia::AsBytes(""), ignore),
                minpatricia::StatusCode::kKeyTooLarge);
  EXPECT_STATUS(index.DescendRange(minpatricia::AsBytes(""), too_large, ignore),
                minpatricia::StatusCode::kKeyTooLarge);
  EXPECT_STATUS(index.AscendLessThan(too_large, ignore), minpatricia::StatusCode::kKeyTooLarge);
  EXPECT_STATUS(index.AscendGreaterOrEqual(too_large, ignore),
                minpatricia::StatusCode::kKeyTooLarge);
  EXPECT_STATUS(index.DescendLessOrEqual(too_large, ignore),
                minpatricia::StatusCode::kKeyTooLarge);
  EXPECT_STATUS(index.DescendGreaterThan(too_large, ignore),
                minpatricia::StatusCode::kKeyTooLarge);
  EXPECT_STATUS(index.Put(minpatricia::AsBytes("tagged"), minpatricia::kChildTag).status(),
                minpatricia::StatusCode::kPositionTag);

  minpatricia::NodePage empty;
  auto lookup = empty.LookupRouteOnly(minpatricia::AsBytes("x"));
  EXPECT_EQ(lookup.first, 0);
  EXPECT_FALSE(lookup.second);
  auto slot = empty.InsertSlotAbovePath(minpatricia::AsBytes("x"), 1, 0);
  EXPECT_TRUE(slot.ok());
  EXPECT_FALSE(slot.value().second);
  EXPECT_STATUS(empty.InsertRoute(0, minpatricia::AsBytes("x"), 1),
                minpatricia::StatusCode::kCorruptLayout);
  EXPECT_STATUS(empty.DeleteRoute(0), minpatricia::StatusCode::kCorruptLayout);

  minpatricia::NodePage corrupt;
  corrupt.size = 2;
  std::array<std::uint16_t, 1> diffs{};
  EXPECT_STATUS(corrupt.RouteDiffs(diffs), minpatricia::StatusCode::kCorruptLayout);
  EXPECT_FALSE(corrupt.IsDirectRoutePair(-1));
  EXPECT_FALSE(corrupt.IsDirectRoutePair(1));
}

void TestGoldenTraceFiles() {
  for (const auto& line : ReadDataLines("golden_diff_cases.tsv")) {
    std::istringstream in(line);
    std::string a_hex;
    std::string b_hex;
    std::string status;
    int cmp = 0;
    int diff = 0;
    in >> a_hex >> b_hex >> status >> cmp >> diff;
    EXPECT_TRUE(in.good() || in.eof());
    const auto a = HexBytes(a_hex);
    const auto b = HexBytes(b_hex);
    auto got = minpatricia::CompareAndDiffBit(a, b);
    if (status == "equal") {
      EXPECT_FALSE(got.ok());
      EXPECT_STATUS(got.status(), minpatricia::StatusCode::kEqualKeys);
      continue;
    }
    EXPECT_TRUE(got.ok());
    EXPECT_EQ(got.value().compare, cmp);
    EXPECT_EQ(got.value().diff, static_cast<std::uint16_t>(diff));
  }

  for (const auto& line : ReadDataLines("golden_route_cases.tsv")) {
    std::istringstream in(line);
    int diff = 0;
    int left_count = 0;
    std::uint32_t bits = 0;
    in >> diff >> left_count >> bits;
    EXPECT_TRUE(in.good() || in.eof());
    const auto route =
        minpatricia::Route::Make(static_cast<std::uint16_t>(diff),
                                 static_cast<std::uint16_t>(left_count));
    EXPECT_EQ(route.bits, bits);
    EXPECT_EQ(route.diff(), static_cast<std::uint16_t>(diff));
    EXPECT_EQ(route.left_count(), static_cast<std::uint16_t>(left_count));
  }

  MemKeys seek_keys;
  minpatricia::HeapNodeStore seek_nodes;
  auto seek_index = TestIndex::NewWithNodes(seek_keys, seek_nodes).take_value();
  for (const auto& key : {"alpha", "bravo", "charlie", "delta", "echo"}) {
    const auto pos = seek_keys.Add(key);
    EXPECT_TRUE(seek_index.Put(minpatricia::AsBytes(key), pos).ok());
  }
  for (const auto& line : ReadDataLines("golden_seek_cases.tsv")) {
    std::istringstream in(line);
    std::string pivot;
    std::string first_ge;
    std::string last_le;
    in >> pivot >> first_ge >> last_le;
    EXPECT_TRUE(in.good() || in.eof());

    std::string got_ge;
    EXPECT_TRUE(seek_index.AscendGreaterOrEqual(
                               minpatricia::AsBytes(pivot),
                               [&](minpatricia::ByteView key, minpatricia::Position) {
                                 got_ge = minpatricia::ToString(key);
                                 return false;
                               })
                    .ok());
    EXPECT_EQ(got_ge, first_ge == "-" ? std::string() : first_ge);

    std::string got_le;
    EXPECT_TRUE(seek_index.DescendLessOrEqual(
                               minpatricia::AsBytes(pivot),
                               [&](minpatricia::ByteView key, minpatricia::Position) {
                                 got_le = minpatricia::ToString(key);
                                 return false;
                               })
                    .ok());
    EXPECT_EQ(got_le, last_le == "-" ? std::string() : last_le);
  }

  MemKeys op_keys;
  minpatricia::HeapNodeStore op_nodes;
  auto op_index = TestIndex::NewWithNodes(op_keys, op_nodes).take_value();
  std::map<std::string, minpatricia::Position> expected;
  for (const auto& line : ReadDataLines("golden_ops_small.tsv")) {
    std::istringstream in(line);
    std::string op;
    std::string key;
    minpatricia::Position pos = 0;
    in >> op >> key >> pos;
    EXPECT_TRUE(in.good() || in.eof());
    if (op == "put") {
      op_keys.keys[pos] = Bytes(key);
      auto put = op_index.Put(minpatricia::AsBytes(key), pos);
      EXPECT_TRUE(put.ok());
    } else if (op == "delete") {
      auto deleted = op_index.Delete(minpatricia::AsBytes(key));
      EXPECT_TRUE(deleted.ok());
      EXPECT_TRUE(deleted.value().deleted);
      EXPECT_EQ(deleted.value().pos, pos);
    } else if (op == "final") {
      expected[key] = pos;
    } else {
      EXPECT_TRUE(false);
    }
  }
  AssertIndexMatchesMap(op_index, expected);
}

}  // namespace

int main() {
  TestNodeLayoutAndDiffs();
  TestHeapStoresAndConstructors();
  TestRootAndCorruptChildErrors();
  TestPutGetDeleteVisit();
  TestProbeAndRetarget();
  TestIteratorAPI();
  TestOpenWithNodesAndNonZeroRoot();
  TestMultiNodeAgainstMap();
  TestIteratorRangeMultiNode();
  TestCartesianRouteAgainstSortedMap();
  TestDeleteAllMaintainsRoutes();
  TestDeleteHeavyKeepsLookupConsistent();
  TestBoundaryReplaceAndRetarget();
  TestDeterministicModelOps();
  TestGoFuzzSeedsAgainstMap();
  TestErrorPaths();
  TestGoldenTraceFiles();
  return 0;
}
