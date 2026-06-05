#include "minpatricia/minpatricia.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

class BenchRecordStore {
 public:
  minpatricia::Position Add(minpatricia::ByteView key) {
    const auto pos = static_cast<minpatricia::Position>(keys_.size());
    keys_.push_back(key);
    return pos;
  }

  minpatricia::Result<minpatricia::ByteView> Key(minpatricia::Position pos) {
    if (pos == 0 || pos >= keys_.size()) {
      return minpatricia::Status(minpatricia::StatusCode::kMissingKey);
    }
    return keys_[static_cast<std::size_t>(pos)];
  }

 private:
  std::vector<minpatricia::ByteView> keys_{minpatricia::ByteView{}};
};

struct BenchData {
  std::vector<std::string> keys;
  BenchRecordStore records;
  std::vector<minpatricia::Position> positions;
};

struct BenchIndex {
  std::unique_ptr<minpatricia::HeapNodeStore> nodes;
  minpatricia::Index<BenchRecordStore, minpatricia::HeapNodeStore> index;
};

std::string TestDataPath(std::string_view name) {
#ifdef MINPATRICIA_TESTDATA_DIR
  return std::string(MINPATRICIA_TESTDATA_DIR) + "/" + std::string(name);
#else
  return "src/minpatricia/testdata/" + std::string(name);
#endif
}

std::vector<std::string> LoadBenchKeys(std::string_view name, int n) {
  const std::string path = TestDataPath("bench_keys_" + std::string(name) + ".tsv");
  std::ifstream in(path);
  if (!in.good()) {
    std::cerr << "missing benchmark fixture: " << path << "\n";
    std::exit(1);
  }

  std::vector<std::string> keys;
  keys.reserve(static_cast<std::size_t>(n));
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty() || line[0] == '#') {
      continue;
    }
    keys.push_back(line);
  }
  if (static_cast<int>(keys.size()) != n) {
    std::cerr << "fixture " << path << " has " << keys.size() << " keys, want " << n
              << "\n";
    std::exit(1);
  }
  return keys;
}

BenchData NewBenchData(std::string_view name, int n) {
  BenchData data;
  data.keys = LoadBenchKeys(name, n);
  data.keys.reserve(static_cast<std::size_t>(n));
  data.positions.reserve(static_cast<std::size_t>(n));

  for (std::size_t i = 0; i < data.keys.size(); ++i) {
    data.positions.push_back(data.records.Add(minpatricia::AsBytes(data.keys[i])));
  }
  return data;
}

BenchIndex BuildPatricia(BenchData& data) {
  BenchIndex built;
  built.nodes = std::make_unique<minpatricia::HeapNodeStore>();
  auto index_result =
      minpatricia::Index<BenchRecordStore>::NewWithNodes(data.records, *built.nodes);
  if (!index_result.ok()) {
    std::cerr << "create index failed\n";
    std::exit(1);
  }
  built.index = index_result.take_value();
  for (std::size_t i = 0; i < data.keys.size(); ++i) {
    auto result = built.index.Put(minpatricia::AsBytes(data.keys[i]), data.positions[i]);
    if (!result.ok()) {
      std::cerr << "build put failed\n";
      std::exit(1);
    }
  }
  return built;
}

std::vector<minpatricia::Position> NewReplacementPositions(BenchData& data) {
  std::vector<minpatricia::Position> replacements;
  replacements.reserve(data.keys.size());
  for (std::size_t i = 0; i < data.keys.size(); ++i) {
    replacements.push_back(data.records.Add(minpatricia::AsBytes(data.keys[i])));
  }
  return replacements;
}

void CollectDeleteHeavyPositions(minpatricia::HeapNodeStore& nodes, minpatricia::NodePage* node,
                                 std::vector<minpatricia::Position>* positions) {
  const int size = static_cast<int>(node->size);
  for (int i = 0; i < size; ++i) {
    const minpatricia::Rep rep = node->reps[static_cast<std::size_t>(i)];
    if (!rep.is_child()) {
      continue;
    }
    auto child_result = nodes.Get(rep.child_id());
    if (!child_result.ok()) {
      std::cerr << "delete-heavy child lookup failed\n";
      std::exit(1);
    }
    minpatricia::NodePage* child = child_result.value();
    const int child_size = static_cast<int>(child->size);
    const int safe_deletes = size + child_size - static_cast<int>(minpatricia::kMaxNodeReps) - 2;
    if (safe_deletes > 0) {
      int added = 0;
      for (int j = 1; j + 1 < child_size && added < safe_deletes; ++j) {
        const minpatricia::Rep child_rep = child->reps[static_cast<std::size_t>(j)];
        if (child_rep.is_child()) {
          continue;
        }
        positions->push_back(child_rep.position());
        ++added;
      }
    }
    CollectDeleteHeavyPositions(nodes, child, positions);
  }
}

std::vector<minpatricia::Position> DeleteHeavyPositions(BenchData& data) {
  auto built = BuildPatricia(data);
  std::vector<minpatricia::Position> positions;
  auto root = built.nodes->Get(built.nodes->Root());
  if (!root.ok()) {
    std::cerr << "root lookup failed\n";
    std::exit(1);
  }
  CollectDeleteHeavyPositions(*built.nodes, root.value(), &positions);
  if (positions.empty()) {
    std::cerr << "no delete-heavy candidates\n";
    std::exit(1);
  }
  return positions;
}

template <class Fn>
std::int64_t TimeNs(Fn&& fn) {
  const auto start = std::chrono::steady_clock::now();
  fn();
  const auto end = std::chrono::steady_clock::now();
  return std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
}

void RunSize(std::string_view name, int n) {
  BenchData data = NewBenchData(name, n);
  auto built = BuildPatricia(data);
  std::uint64_t sink = 0;

  const std::int64_t get_ns = TimeNs([&] {
    for (int round = 0; round < 100; ++round) {
      for (std::size_t i = 0; i < data.keys.size(); ++i) {
        auto result = built.index.Get(minpatricia::AsBytes(data.keys[i]));
        if (!result.ok() || !result.value().found) {
          std::cerr << "get failed\n";
          std::exit(1);
        }
        sink += result.value().pos;
      }
    }
  });
  std::cout << "Get/" << name << " " << (get_ns / (n * 100)) << " ns/op\n";

  auto replacements = NewReplacementPositions(data);
  const std::int64_t replace_ns = TimeNs([&] {
    for (int round = 0; round < 100; ++round) {
      for (std::size_t i = 0; i < data.keys.size(); ++i) {
        auto result = built.index.Put(minpatricia::AsBytes(data.keys[i]), replacements[i]);
        if (!result.ok() || !result.value().replaced) {
          std::cerr << "replace failed\n";
          std::exit(1);
        }
      }
    }
  });
  std::cout << "PutReplace/" << name << " " << (replace_ns / (n * 100)) << " ns/op\n";

  const int insert_rounds = n >= 10000 ? 20 : 100;
  const std::int64_t insert_ns = TimeNs([&] {
    for (int round = 0; round < insert_rounds; ++round) {
      auto fresh_nodes = std::make_unique<minpatricia::HeapNodeStore>();
      auto index_result =
          minpatricia::Index<BenchRecordStore>::NewWithNodes(data.records, *fresh_nodes);
      if (!index_result.ok()) {
        std::cerr << "fresh index failed\n";
        std::exit(1);
      }
      auto index = index_result.take_value();
      for (std::size_t i = 0; i < data.keys.size(); ++i) {
        auto result = index.Put(minpatricia::AsBytes(data.keys[i]), data.positions[i]);
        if (!result.ok()) {
          std::cerr << "insert failed\n";
          std::exit(1);
        }
      }
      sink += static_cast<std::uint64_t>(index.LiveNodes());
    }
  });
  std::cout << "PutInsert/" << name << " " << (insert_ns / (n * insert_rounds)) << " ns/op\n";

  const std::int64_t seek_ns = TimeNs([&] {
    for (int round = 0; round < 100; ++round) {
      for (const auto& key : data.keys) {
        bool found = false;
        auto status = built.index.AscendGreaterOrEqual(
            minpatricia::AsBytes(key), [&](minpatricia::ByteView, minpatricia::Position pos) {
              sink += pos;
              found = true;
              return false;
            });
        if (!status.ok() || !found) {
          std::cerr << "seek failed\n";
          std::exit(1);
        }
      }
    }
  });
  std::cout << "Seek/" << name << " " << (seek_ns / (n * 100)) << " ns/op\n";

  const std::int64_t reverse_seek_ns = TimeNs([&] {
    for (int round = 0; round < 100; ++round) {
      for (const auto& key : data.keys) {
        bool found = false;
        auto status = built.index.DescendLessOrEqual(
            minpatricia::AsBytes(key), [&](minpatricia::ByteView, minpatricia::Position pos) {
              sink += pos;
              found = true;
              return false;
            });
        if (!status.ok() || !found) {
          std::cerr << "reverse seek failed\n";
          std::exit(1);
        }
      }
    }
  });
  std::cout << "ReverseSeek/" << name << " " << (reverse_seek_ns / (n * 100)) << " ns/op\n";

  const int visit_rounds = n >= 10000 ? 20 : 100;
  const std::int64_t visit_ns = TimeNs([&] {
    for (int round = 0; round < visit_rounds; ++round) {
      auto status = built.index.Visit([&](minpatricia::ByteView, minpatricia::Position pos) {
        sink += pos;
        return true;
      });
      if (!status.ok()) {
        std::cerr << "visit failed\n";
        std::exit(1);
      }
    }
  });
  std::cout << "VisitFullSetOrdered/" << name << " "
            << (visit_ns / (n * visit_rounds)) << " ns/item\n";

  const std::int64_t reverse_visit_ns = TimeNs([&] {
    for (int round = 0; round < visit_rounds; ++round) {
      auto status = built.index.Descend([&](minpatricia::ByteView, minpatricia::Position pos) {
        sink += pos;
        return true;
      });
      if (!status.ok()) {
        std::cerr << "reverse visit failed\n";
        std::exit(1);
      }
    }
  });
  std::cout << "VisitFullSetReverse/" << name << " "
            << (reverse_visit_ns / (n * visit_rounds)) << " ns/item\n";

  std::cout << "Footprint/" << name << " "
            << (static_cast<double>(built.index.LiveNodes() * minpatricia::kNodeSize) /
                static_cast<double>(n))
            << " node_B/key " << built.index.LiveNodes() << " nodes\n";
  std::cerr << "sink=" << sink << "\n";
}

void RunDeleteHeavy(std::string_view name, int n) {
  BenchData data = NewBenchData(name, n);
  auto positions = DeleteHeavyPositions(data);
  std::vector<std::string> keys;
  keys.reserve(positions.size());
  for (const auto pos : positions) {
    auto key = data.records.Key(pos);
    if (!key.ok()) {
      std::cerr << "missing delete key\n";
      std::exit(1);
    }
    keys.push_back(minpatricia::ToString(key.value()));
  }
  std::vector<minpatricia::ByteView> key_views;
  key_views.reserve(keys.size());
  for (const auto& key : keys) {
    key_views.push_back(minpatricia::AsBytes(key));
  }

  const int rounds = n >= 100000 ? 100 : 50;
  std::uint64_t sink = 0;
  int live_nodes = 0;
  int live_keys = 0;
  std::int64_t ns = 0;
  for (int round = 0; round < rounds; ++round) {
    auto built = BuildPatricia(data);
    ns += TimeNs([&] {
      for (const auto key : key_views) {
        auto deleted = built.index.Delete(key);
        if (!deleted.ok() || !deleted.value().deleted) {
          std::cerr << "delete-heavy failed\n";
          std::exit(1);
        }
        sink += deleted.value().pos;
      }
    });
    live_nodes = built.index.LiveNodes();
    live_keys = built.index.Len();
  }
  std::cout << "DeleteHeavy/" << name << " " << (ns / (static_cast<int>(keys.size()) * rounds))
            << " ns/op deleted=" << keys.size() << "\n";
  std::cout << "DeleteHeavyFootprint/" << name << " "
            << (static_cast<double>(live_nodes * minpatricia::kNodeSize) /
                static_cast<double>(live_keys))
            << " node_B/live_key " << live_nodes << " nodes deleted=" << keys.size()
            << "\n";
  std::cerr << "delete_sink=" << sink << "\n";
}

}  // namespace

int main() {
  RunSize("1K", 1000);
  RunSize("10K", 10000);
  RunDeleteHeavy("10K", 10000);
  if (const char* large = std::getenv("MINPATRICIA_BENCH_LARGE");
      large != nullptr && std::string_view(large) == "1") {
    RunSize("100K", 100000);
    RunDeleteHeavy("100K", 100000);
  }
  return 0;
}
