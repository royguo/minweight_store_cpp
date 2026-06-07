#include "minweight_store/store.h"

#include <chrono>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {

std::string BenchDir(const std::string& name) {
  std::string dir = ".runtime/minweight_store_bench/" + name;
  std::string cmd = "rm -rf " + dir;
  if (std::system(cmd.c_str()) != 0) {
    std::abort();
  }
  return dir;
}

std::string Key(int i) {
  std::ostringstream out;
  out << "key-" << std::setw(12) << std::setfill('0') << i;
  return out.str();
}

std::string Value(int i) {
  std::string value(64, 'a');
  value[0] = static_cast<char>('a' + (i % 26));
  return value;
}

template <class Fn>
long long Measure(Fn&& fn) {
  const auto start = std::chrono::steady_clock::now();
  fn();
  const auto end = std::chrono::steady_clock::now();
  return std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
}

template <class T>
T Expect(minweight_store::Result<T> result) {
  if (!result.ok()) {
    std::cerr << "error: " << result.status().message() << "\n";
    std::abort();
  }
  return result.take_value();
}

void Expect(minweight_store::Status status) {
  if (!status.ok()) {
    std::cerr << "error: " << status.message() << "\n";
    std::abort();
  }
}

void Run(int n) {
  std::vector<std::string> keys;
  std::vector<std::string> values;
  keys.reserve(static_cast<std::size_t>(n));
  values.reserve(static_cast<std::size_t>(n));
  for (int i = 0; i < n; ++i) {
    keys.push_back(Key(i));
    values.push_back(Value(i));
  }

  minweight_store::Options options;
  options.sync_on_close = false;
  auto store = Expect(minweight_store::Store::Open(BenchDir(std::to_string(n)), options));

  const long long put_ns = Measure([&] {
    for (int i = 0; i < n; ++i) {
      Expect(store->Put(minweight_store::AsBytes(keys[static_cast<std::size_t>(i)]),
                        minweight_store::AsBytes(values[static_cast<std::size_t>(i)])));
    }
  });
  std::cout << "Put/" << n << " " << (put_ns / n) << " ns/op\n";

  const long long get_ns = Measure([&] {
    for (int r = 0; r < 10; ++r) {
      for (int i = 0; i < n; ++i) {
        auto got = Expect(store->Get(minweight_store::AsBytes(keys[static_cast<std::size_t>(i)])));
        if (!got.found) {
          std::abort();
        }
      }
    }
  });
  std::cout << "Get/" << n << " " << (get_ns / (n * 10)) << " ns/op\n";

  std::size_t scanned = 0;
  const long long scan_ns = Measure([&] {
    Expect(store->Scan([&](const minweight_store::Item&) {
      ++scanned;
      return true;
    }));
  });
  std::cout << "Scan/" << n << " " << (scan_ns / n) << " ns/item scanned=" << scanned << "\n";

  const long long seek_ns = Measure([&] {
    for (int r = 0; r < 10; ++r) {
      for (int i = 0; i < n; ++i) {
        auto got = Expect(store->SeekGE(minweight_store::AsBytes(keys[static_cast<std::size_t>(i)])));
        if (!got.found) {
          std::abort();
        }
      }
    }
  });
  std::cout << "SeekGE/" << n << " " << (seek_ns / (n * 10)) << " ns/op\n";
}

}  // namespace

int main() {
  Run(1000);
  Run(10000);
  if (std::getenv("MINWEIGHT_STORE_BENCH_LARGE") != nullptr) {
    Run(100000);
  }
  return 0;
}
