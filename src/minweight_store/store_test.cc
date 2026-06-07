#include "minweight_store/store.h"

#include <atomic>
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

#define CHECK(expr)                                                                  \
  do {                                                                               \
    if (!(expr)) {                                                                   \
      std::cerr << "check failed: " #expr << " at " << __FILE__ << ":" << __LINE__ \
                << "\n";                                                            \
      std::abort();                                                                  \
    }                                                                                \
  } while (false)

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
  CHECK(std::system(cmd.c_str()) == 0);
  return dir;
}

bool Exists(const std::string& path) {
  struct stat st {};
  return ::stat(path.c_str(), &st) == 0;
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
    CHECK(got.found);
    CHECK(got.value == "three");
    auto deleted = ExpectResult(store->Delete(minweight_store::AsBytes("bravo")));
    CHECK(deleted);
    CHECK(ExpectResult(store->Len()) == 1);
    ExpectStatus(store->Close());
  }

  {
    auto store = ExpectResult(minweight_store::Store::Open(dir, options));
    auto alpha = ExpectResult(store->Get(minweight_store::AsBytes("alpha")));
    CHECK(alpha.found);
    CHECK(alpha.value == "three");
    auto bravo = ExpectResult(store->Get(minweight_store::AsBytes("bravo")));
    CHECK(!bravo.found);
    CHECK(ExpectResult(store->Len()) == 1);
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
  CHECK((keys == std::vector<std::string>{"alpha", "bravo", "charlie", "delta"}));

  keys.clear();
  ExpectStatus(store->ScanRange(minweight_store::AsBytes("bravo"),
                                minweight_store::AsBytes("delta"),
                                [&](const minweight_store::Item& item) {
                                  keys.push_back(item.key);
                                  return true;
                                }));
  CHECK((keys == std::vector<std::string>{"bravo", "charlie"}));

  auto ge = ExpectResult(store->SeekGE(minweight_store::AsBytes("bzz")));
  CHECK(ge.found);
  CHECK(ge.item.key == "charlie");

  auto le = ExpectResult(store->SeekLE(minweight_store::AsBytes("bzz")));
  CHECK(le.found);
  CHECK(le.item.key == "bravo");

  keys.clear();
  ExpectStatus(store->ReverseScan([&](const minweight_store::Item& item) {
    keys.push_back(item.key);
    return true;
  }));
  CHECK((keys == std::vector<std::string>{"delta", "charlie", "bravo", "alpha"}));
}

void CorruptSecondRecordCRC(const std::string& dir) {
  const std::string path =
      dir + "/wal/00000000000000000001/00000000000000000001.wal";
  const int fd = ::open(path.c_str(), O_RDWR);
  CHECK(fd >= 0);

  constexpr off_t wal_header_size = 4096;
  constexpr off_t wal_record_header_size = 13;
  constexpr off_t crc_offset = 9;

  unsigned char header[wal_record_header_size];
  ssize_t n = ::pread(fd, header, sizeof(header), wal_header_size);
  CHECK(n == static_cast<ssize_t>(sizeof(header)));
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
  CHECK(n == 1);
  byte ^= 0xff;
  n = ::pwrite(fd, &byte, 1, second + crc_offset);
  CHECK(n == 1);
  CHECK(::close(fd) == 0);
}

void TestPointInTimeRecoveryKeepsPrefix() {
  const std::string dir = TestDir("point_in_time_recovery");
  minweight_store::Options options;
  options.sync_on_close = false;

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
    CHECK(a.found && a.value == "1");
    CHECK(!b.found);
    CHECK(!c.found);
    CHECK(ExpectResult(store->Len()) == 1);
  }
}

void TestStrictRecoveryFails() {
  const std::string dir = TestDir("strict_recovery");
  minweight_store::Options options;
  options.sync_on_close = false;

  {
    auto store = ExpectResult(minweight_store::Store::Open(dir, options));
    ExpectStatus(store->Put(minweight_store::AsBytes("a"), minweight_store::AsBytes("1")));
    ExpectStatus(store->Put(minweight_store::AsBytes("b"), minweight_store::AsBytes("2")));
    ExpectStatus(store->Close());
  }

  CorruptSecondRecordCRC(dir);
  options.wal_replay_policy = minweight_store::WALReplayPolicy::kStrict;
  auto store = minweight_store::Store::Open(dir, options);
  CHECK(!store.ok());
  CHECK(store.status() == minweight_store::StatusCode::kCorruptWal);
}

void TestCloseCheckpointsAndReclaimsWal() {
  const std::string dir = TestDir("close_checkpoint");
  minweight_store::Options options;
  options.sync_on_close = true;
  options.wal_size = 4096 + 80;

  {
    auto store = ExpectResult(minweight_store::Store::Open(dir, options));
    ExpectStatus(store->Put(minweight_store::AsBytes("alpha"),
                            minweight_store::AsBytes("one")));
    ExpectStatus(store->Put(minweight_store::AsBytes("bravo"),
                            minweight_store::AsBytes("two")));
    ExpectStatus(store->Put(minweight_store::AsBytes("charlie"),
                            minweight_store::AsBytes("three")));
    ExpectStatus(store->Close());
  }

  CHECK(Exists(dir + "/MANIFEST"));
  CHECK(Exists(dir + "/SNAPSHOT.00000000000000000002"));
  CHECK(!Exists(dir + "/wal/00000000000000000001"));
  CHECK(Exists(dir + "/wal/00000000000000000002"));

  {
    auto store = ExpectResult(minweight_store::Store::Open(dir, options));
    auto alpha = ExpectResult(store->Get(minweight_store::AsBytes("alpha")));
    auto bravo = ExpectResult(store->Get(minweight_store::AsBytes("bravo")));
    auto charlie = ExpectResult(store->Get(minweight_store::AsBytes("charlie")));
    CHECK(alpha.found && alpha.value == "one");
    CHECK(bravo.found && bravo.value == "two");
    CHECK(charlie.found && charlie.value == "three");
    CHECK(ExpectResult(store->Len()) == 3);
  }
}

void TestCheckpointReplaysWalTail() {
  const std::string dir = TestDir("checkpoint_wal_tail");
  minweight_store::Options options;
  options.sync_on_close = false;
  options.wal_size = 4096 + 32;

  {
    auto store = ExpectResult(minweight_store::Store::Open(dir, options));
    for (int i = 0; i < 7; ++i) {
      const std::string key = "k0" + std::to_string(i);
      const std::string value = "v0" + std::to_string(i);
      ExpectStatus(store->Put(minweight_store::AsBytes(key),
                              minweight_store::AsBytes(value)));
    }
    CHECK(ExpectResult(store->Len()) == 7);
    ExpectStatus(store->Close());
  }

  CHECK(Exists(dir + "/MANIFEST"));
  CHECK(Exists(dir + "/SNAPSHOT.00000000000000000002"));

  {
    auto store = ExpectResult(minweight_store::Store::Open(dir, options));
    for (int i = 0; i < 7; ++i) {
      const std::string key = "k0" + std::to_string(i);
      const std::string value = "v0" + std::to_string(i);
      auto got = ExpectResult(store->Get(minweight_store::AsBytes(key)));
      CHECK(got.found);
      CHECK(got.value == value);
    }
    CHECK(ExpectResult(store->Len()) == 7);
  }
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
  CHECK(runtime->blocking_io_calls.load() >= 2);
}

}  // namespace

int main() {
  TestPutGetDeleteReopen();
  TestScanAndSeek();
  TestPointInTimeRecoveryKeepsPrefix();
  TestStrictRecoveryFails();
  TestCloseCheckpointsAndReclaimsWal();
  TestCheckpointReplaysWalTail();
  TestRuntimeInjection();
  std::cout << "minweight_store_tests passed\n";
  return 0;
}
