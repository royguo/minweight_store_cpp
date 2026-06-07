#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "minpatricia/byte_view.h"
#include "minweight_store/options.h"
#include "minweight_store/status.h"

namespace minweight_store {

using ByteView = minpatricia::ByteView;

inline ByteView AsBytes(std::string_view value) {
  return minpatricia::AsBytes(value);
}

struct Item {
  std::string key;
  std::string value;
};

struct GetResult {
  std::string value;
  bool found = false;
};

struct SeekResult {
  Item item;
  bool found = false;
};

using VisitFunc = std::function<bool(const Item&)>;

class Store {
 public:
  static Result<std::unique_ptr<Store>> Open(const std::string& dir,
                                             Options options = Options{});
  static Result<std::unique_ptr<Store>> New(Options options = Options{});

  Store();
  ~Store();

  Store(const Store&) = delete;
  Store& operator=(const Store&) = delete;

  Status Close();
  Result<std::size_t> Len();
  Status Put(ByteView key, ByteView value);
  Result<GetResult> Get(ByteView key);
  Result<bool> Delete(ByteView key);

  Status Scan(const VisitFunc& fn);
  Status ScanRange(ByteView greater_or_equal, ByteView less_than, const VisitFunc& fn);
  Status ReverseScan(const VisitFunc& fn);
  Status ReverseScanRange(ByteView less_or_equal, ByteView greater_than,
                          const VisitFunc& fn);
  Result<SeekResult> SeekGE(ByteView key);
  Result<SeekResult> SeekLE(ByteView key);

 private:
  class Impl;
  explicit Store(std::unique_ptr<Impl> impl);

  std::unique_ptr<Impl> impl_;
};

}  // namespace minweight_store
