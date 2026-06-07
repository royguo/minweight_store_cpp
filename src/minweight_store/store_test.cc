#include "minweight_store/store.h"

#include <atomic>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <memory>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace {

class CountingRuntime : public minweight_store::StdRuntime {
 public:
  minweight_store::Status BlockingIO(std::string_view name, Task task) override {
    (void)name;
    ++blocking_io_calls;
    return minweight_store::StdRuntime::BlockingIO(name, std::move(task));
  }

  std::atomic<int> blocking_io_calls{0};
};

std::string TestDir(const std::string& name) {
  std::string dir = ".runtime/minweight_store_tests/" + name;
  std::string cmd = "rm -rf " + dir;
  assert(std::system(cmd.c_str()) == 0);
  return dir;
}

void ExpectStatus(const minweight_store::Status& status) {
  if (!status.ok()) {
    std::cerr << "status failed: code=" << static_cast<int>(status.code())
              << " message=" << status.message() << "\n";
    std::abort();
  }
}

template <class T>
T ExpectResult(minweight_store::Result<T> result) {
  if (!result.ok()) {
    std::cerr << "result failed: code=" << static_cast<int>(result.status().code())
              << " message=" << result.status().message() << "\n";
    std::abort();
  }
  return result.take_value();
}

void TestPutGetDeleteReopen() {
  const std::string dir = TestDir("put_get_delete_reopen");
  minweight_store::Options options;
  options.sync_on_close = true;

  {
    auto store = ExpectResult(minweight_store::Store::Open(dir, options));
    ExpectStatus(store->Put(minweight_store::AsBytes("alpha"),
                            minweight_store::AsBytes("one")));
    ExpectStatus(store->Put(minweight_store::AsBytes("bravo"),
                            minweight_store::AsBytes("two")));
    ExpectStatus(store->Put(minweight_store::AsBytes("alpha"),
                            minweight_store::AsBytes("three")));
    auto got = ExpectResult(store->Get(minweight_store::AsBytes("alpha")));
    assert(got.found);
    assert(got.value == "three");
    auto deleted = ExpectResult(store->Delete(minweight_store::AsBytes("bravo")));
    assert(deleted);
    assert(ExpectResult(store->Len()) == 1);
    ExpectStatus(store->Close());
  }

  {
    auto store = ExpectResult(minweight_store::Store::Open(dir, options));
    auto alpha = ExpectResult(store->Get(minweight_store::AsBytes("alpha")));
    assert(alpha.found);
    assert(alpha.value == "three");
    auto bravo = ExpectResult(store->Get(minweight_store::AsBytes("bravo")));
    assert(!bravo.found);
    assert(ExpectResult(store->Len()) == 1);
  }
}

void TestScanAndSeek() {
  const std::string dir = TestDir("scan_and_seek");
  auto store = ExpectResult(minweight_store::Store::Open(dir));
  for (const auto& key : {"alpha", "bravo", "charlie", "delta"}) {
    ExpectStatus(store->Put(minweight_store::AsBytes(key), minweight_store::AsBytes(key)));
  }

  std::vector<std::string> keys;
  ExpectStatus(store->Scan([&](const minweight_store::Item& item) {
    keys.push_back(item.key);
    return true;
  }));
  assert((keys == std::vector<std::string>{"alpha", "bravo", "charlie", "delta"}));

  keys.clear();
  ExpectStatus(store->ScanRange(minweight_store::AsBytes("bravo"),
                                minweight_store::AsBytes("delta"),
                                [&](const minweight_store::Item& item) {
                                  keys.push_back(item.key);
                                  return true;
                                }));
  assert((keys == std::vector<std::string>{"bravo", "charlie"}));

  auto ge = ExpectResult(store->SeekGE(minweight_store::AsBytes("bzz")));
  assert(ge.found);
  assert(ge.item.key == "charlie");

  auto le = ExpectResult(store->SeekLE(minweight_store::AsBytes("bzz")));
  assert(le.found);
  assert(le.item.key == "bravo");

  keys.clear();
  ExpectStatus(store->ReverseScan([&](const minweight_store::Item& item) {
    keys.push_back(item.key);
    return true;
  }));
  assert((keys == std::vector<std::string>{"delta", "charlie", "bravo", "alpha"}));
}

void CorruptSecondRecordCRC(const std::string& dir) {
  const std::string path = dir + "/wal/00000000000000000001.wal";
  const int fd = ::open(path.c_str(), O_RDWR);
  assert(fd >= 0);

  constexpr off_t wal_header_size = 4096;
  constexpr off_t wal_record_header_size = 13;
  constexpr off_t crc_offset = 9;

  unsigned char header[wal_record_header_size];
  ssize_t n = ::pread(fd, header, sizeof(header), wal_header_size);
  assert(n == static_cast<ssize_t>(sizeof(header)));
  const std::uint32_t key_len = static_cast<std::uint32_t>(header[1]) |
                                (static_cast<std::uint32_t>(header[2]) << 8) |
                                (static_cast<std::uint32_t>(header[3]) << 16) |
                                (static_cast<std::uint32_t>(header[4]) << 24);
  const std::uint32_t value_len = static_cast<std::uint32_t>(header[5]) |
                                  (static_cast<std::uint32_t>(header[6]) << 8) |
                                  (static_cast<std::uint32_t>(header[7]) << 16) |
                                  (static_cast<std::uint32_t>(header[8]) << 24);
  const off_t second = wal_header_size + wal_record_header_size + key_len + value_len;
  unsigned char byte = 0;
  n = ::pread(fd, &byte, 1, second + crc_offset);
  assert(n == 1);
  byte ^= 0xff;
  n = ::pwrite(fd, &byte, 1, second + crc_offset);
  assert(n == 1);
  assert(::close(fd) == 0);
}

void TestPointInTimeRecoveryKeepsPrefix() {
  const std::string dir = TestDir("point_in_time_recovery");
  minweight_store::Options options;
  options.sync_on_close = true;

  {
    auto store = ExpectResult(minweight_store::Store::Open(dir, options));
    ExpectStatus(store->Put(minweight_store::AsBytes("a"), minweight_store::AsBytes("1")));
    ExpectStatus(store->Put(minweight_store::AsBytes("b"), minweight_store::AsBytes("2")));
    ExpectStatus(store->Put(minweight_store::AsBytes("c"), minweight_store::AsBytes("3")));
    ExpectStatus(store->Close());
  }

  CorruptSecondRecordCRC(dir);

  {
    auto store = ExpectResult(minweight_store::Store::Open(dir, options));
    auto a = ExpectResult(store->Get(minweight_store::AsBytes("a")));
    auto b = ExpectResult(store->Get(minweight_store::AsBytes("b")));
    auto c = ExpectResult(store->Get(minweight_store::AsBytes("c")));
    assert(a.found && a.value == "1");
    assert(!b.found);
    assert(!c.found);
    assert(ExpectResult(store->Len()) == 1);
  }
}

void TestStrictRecoveryFails() {
  const std::string dir = TestDir("strict_recovery");
  minweight_store::Options options;
  options.sync_on_close = true;

  {
    auto store = ExpectResult(minweight_store::Store::Open(dir, options));
    ExpectStatus(store->Put(minweight_store::AsBytes("a"), minweight_store::AsBytes("1")));
    ExpectStatus(store->Put(minweight_store::AsBytes("b"), minweight_store::AsBytes("2")));
    ExpectStatus(store->Close());
  }

  CorruptSecondRecordCRC(dir);
  options.wal_replay_policy = minweight_store::WALReplayPolicy::kStrict;
  auto store = minweight_store::Store::Open(dir, options);
  assert(!store.ok());
  assert(store.status() == minweight_store::StatusCode::kCorruptWal);
}

void TestRuntimeInjection() {
  const std::string dir = TestDir("runtime_injection");
  auto runtime = std::make_shared<CountingRuntime>();
  minweight_store::Options options;
  options.runtime = runtime;

  auto store = ExpectResult(minweight_store::Store::Open(dir, options));
  ExpectStatus(store->Put(minweight_store::AsBytes("alpha"),
                          minweight_store::AsBytes("one")));
  ExpectStatus(store->Close());
  assert(runtime->blocking_io_calls.load() >= 2);
}

}  // namespace

int main() {
  TestPutGetDeleteReopen();
  TestScanAndSeek();
  TestPointInTimeRecoveryKeepsPrefix();
  TestStrictRecoveryFails();
  TestRuntimeInjection();
  std::cout << "minweight_store_tests passed\n";
  return 0;
}
