#include "minweight_store/store.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <iomanip>
#include <limits>
#include <memory>
#include <sstream>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <unordered_map>
#include <utility>
#include <vector>

#include "minpatricia/minpatricia.h"

namespace minweight_store {
namespace {

constexpr std::uint64_t kChildTag = 1ULL << 63;
constexpr std::uint64_t kRecordOffsetBits = 30;
constexpr std::uint64_t kRecordOffsetLimit = 1ULL << kRecordOffsetBits;
constexpr std::uint64_t kRecordOffsetMask = kRecordOffsetLimit - 1;
constexpr std::uint64_t kRecordFileNoLimit = 1ULL << (63 - kRecordOffsetBits);

constexpr std::uint64_t kWalHeaderSize = 4096;
constexpr std::uint32_t kWalVersion = 1;
constexpr std::uint64_t kFirstWalSegmentNo = 1;
constexpr const char* kWalDirName = "wal";
constexpr const char* kWalSegmentSuffix = ".wal";

constexpr std::size_t kWalHeaderVersionOffset = 8;
constexpr std::size_t kWalHeaderUsedOffset = 16;

constexpr std::size_t kWalRecordHeaderSize = 13;
constexpr std::size_t kWalRecordOpOffset = 0;
constexpr std::size_t kWalRecordKeyOffset = 1;
constexpr std::size_t kWalRecordValueOffset = 5;
constexpr std::size_t kWalRecordCRCOffset = 9;

constexpr std::uint8_t kWalOpPut = 1;
constexpr std::uint8_t kWalOpDelete = 2;

constexpr std::array<char, 8> kWalHeaderMagic{{'M', 'W', 'W', 'A', 'L', '0', '1', 0}};

Status IoError(const std::string& op, const std::string& path) {
  return Status(StatusCode::kIoError,
                op + " " + path + ": " + std::strerror(errno));
}

Status CorruptWal(const std::string& message) {
  return Status(StatusCode::kCorruptWal, message);
}

Status FromMinpatriciaStatus(minpatricia::Status status) {
  if (status.ok()) {
    return OkStatus();
  }
  switch (status.code()) {
    case minpatricia::StatusCode::kKeyTooLarge:
      return Status(StatusCode::kInvalidArgument, "key too large");
    case minpatricia::StatusCode::kPositionTag:
      return Status(StatusCode::kCorruptWal, "invalid position tag");
    case minpatricia::StatusCode::kCorruptLayout:
    case minpatricia::StatusCode::kMissingKey:
    case minpatricia::StatusCode::kPositionMismatch:
    case minpatricia::StatusCode::kDuplicateKey:
    case minpatricia::StatusCode::kEqualKeys:
    case minpatricia::StatusCode::kUnsortedKeys:
    case minpatricia::StatusCode::kNilRecordStore:
    case minpatricia::StatusCode::kNilNodeStore:
    case minpatricia::StatusCode::kOk:
      return Status(StatusCode::kCorruptIndex, "minpatricia index error");
  }
  return Status(StatusCode::kCorruptIndex, "unknown minpatricia index error");
}

std::uint32_t Load32(const std::byte* data) {
  const auto* p = reinterpret_cast<const unsigned char*>(data);
  return static_cast<std::uint32_t>(p[0]) |
         (static_cast<std::uint32_t>(p[1]) << 8) |
         (static_cast<std::uint32_t>(p[2]) << 16) |
         (static_cast<std::uint32_t>(p[3]) << 24);
}

std::uint64_t Load64(const std::byte* data) {
  const auto* p = reinterpret_cast<const unsigned char*>(data);
  std::uint64_t value = 0;
  for (int i = 7; i >= 0; --i) {
    value = (value << 8) | p[i];
  }
  return value;
}

void Store32(std::byte* data, std::uint32_t value) {
  auto* p = reinterpret_cast<unsigned char*>(data);
  p[0] = static_cast<unsigned char>(value);
  p[1] = static_cast<unsigned char>(value >> 8);
  p[2] = static_cast<unsigned char>(value >> 16);
  p[3] = static_cast<unsigned char>(value >> 24);
}

void Store64(std::byte* data, std::uint64_t value) {
  auto* p = reinterpret_cast<unsigned char*>(data);
  for (int i = 0; i < 8; ++i) {
    p[i] = static_cast<unsigned char>(value >> (i * 8));
  }
}

bool IsZeroBytes(const std::byte* data, std::size_t size) {
  for (std::size_t i = 0; i < size; ++i) {
    if (data[i] != std::byte{0}) {
      return false;
    }
  }
  return true;
}

std::array<std::uint32_t, 256> MakeCRC32Table() {
  std::array<std::uint32_t, 256> table{};
  for (std::uint32_t i = 0; i < table.size(); ++i) {
    std::uint32_t crc = i;
    for (int j = 0; j < 8; ++j) {
      if ((crc & 1U) != 0U) {
        crc = (crc >> 1U) ^ 0xedb88320U;
      } else {
        crc >>= 1U;
      }
    }
    table[i] = crc;
  }
  return table;
}

std::uint32_t CRC32Update(std::uint32_t crc, const std::byte* data, std::size_t size) {
  static const auto table = MakeCRC32Table();
  crc = ~crc;
  const auto* p = reinterpret_cast<const unsigned char*>(data);
  for (std::size_t i = 0; i < size; ++i) {
    crc = table[(crc ^ p[i]) & 0xffU] ^ (crc >> 8U);
  }
  return ~crc;
}

std::uint32_t WalRecordCRC(minpatricia::ByteView record) {
  std::uint32_t crc = 0;
  crc = CRC32Update(crc, record.data() + kWalRecordOpOffset,
                    kWalRecordCRCOffset - kWalRecordOpOffset);
  crc = CRC32Update(crc, record.data() + kWalRecordHeaderSize,
                    record.size() - kWalRecordHeaderSize);
  return crc;
}

Result<minpatricia::Position> MakeRecordPosition(std::uint64_t file_no,
                                                  std::uint64_t offset) {
  if (file_no == 0 || file_no >= kRecordFileNoLimit || offset >= kRecordOffsetLimit) {
    return Status(StatusCode::kCorruptWal, "record position out of range");
  }
  const std::uint64_t pos = (file_no << kRecordOffsetBits) | offset;
  if ((pos & kChildTag) != 0) {
    return Status(StatusCode::kCorruptWal, "record position overlaps child tag");
  }
  return static_cast<minpatricia::Position>(pos);
}

std::uint64_t RecordPositionFileNo(minpatricia::Position pos) {
  return static_cast<std::uint64_t>(pos) >> kRecordOffsetBits;
}

std::uint64_t RecordPositionOffset(minpatricia::Position pos) {
  return static_cast<std::uint64_t>(pos) & kRecordOffsetMask;
}

std::string JoinPath(const std::string& lhs, const std::string& rhs) {
  if (lhs.empty()) {
    return rhs;
  }
  if (lhs.back() == '/') {
    return lhs + rhs;
  }
  return lhs + "/" + rhs;
}

Status CreateDirIfMissing(const std::string& path) {
  if (::mkdir(path.c_str(), 0755) == 0 || errno == EEXIST) {
    return OkStatus();
  }
  return IoError("mkdir", path);
}

Status CreateDirs(const std::string& path) {
  if (path.empty()) {
    return OkStatus();
  }

  std::string current;
  std::size_t start = 0;
  if (path[0] == '/') {
    current = "/";
    start = 1;
  }

  while (start <= path.size()) {
    const std::size_t slash = path.find('/', start);
    const std::string part = path.substr(start, slash == std::string::npos
                                                    ? std::string::npos
                                                    : slash - start);
    if (!part.empty()) {
      if (!current.empty() && current.back() != '/') {
        current.push_back('/');
      }
      current += part;
      Status status = CreateDirIfMissing(current);
      if (!status.ok()) {
        return status;
      }
    }
    if (slash == std::string::npos) {
      break;
    }
    start = slash + 1;
  }
  return OkStatus();
}

std::string WalSegmentName(std::uint64_t file_no) {
  std::ostringstream out;
  out << std::setw(20) << std::setfill('0') << file_no << kWalSegmentSuffix;
  return out.str();
}

Result<std::uint64_t> ParseWalSegmentName(const std::string& name) {
  const std::string suffix(kWalSegmentSuffix);
  if (name.size() != 20 + suffix.size() ||
      name.compare(name.size() - suffix.size(), suffix.size(), suffix) != 0) {
    return Status(StatusCode::kInvalidArgument, "not a wal segment");
  }
  std::uint64_t value = 0;
  for (std::size_t i = 0; i < 20; ++i) {
    if (name[i] < '0' || name[i] > '9') {
      return Status(StatusCode::kInvalidArgument, "not a wal segment");
    }
    value = value * 10 + static_cast<std::uint64_t>(name[i] - '0');
  }
  if (value == 0) {
    return Status(StatusCode::kInvalidArgument, "invalid wal id");
  }
  return value;
}

Result<std::vector<std::uint64_t>> ListWalSegmentIDs(const std::string& wal_dir) {
  DIR* dir = ::opendir(wal_dir.c_str());
  if (dir == nullptr) {
    return IoError("opendir", wal_dir);
  }

  std::vector<std::uint64_t> ids;
  for (;;) {
    errno = 0;
    dirent* entry = ::readdir(dir);
    if (entry == nullptr) {
      if (errno != 0) {
        Status status = IoError("readdir", wal_dir);
        ::closedir(dir);
        return status;
      }
      break;
    }
    const std::string name(entry->d_name);
    auto id = ParseWalSegmentName(name);
    if (id.ok()) {
      ids.push_back(id.value());
    }
  }

  if (::closedir(dir) != 0) {
    return IoError("closedir", wal_dir);
  }
  std::sort(ids.begin(), ids.end());
  return ids;
}

struct WalRecord {
  std::uint8_t op = 0;
  minpatricia::ByteView key;
  minpatricia::ByteView value;
  std::uint64_t end = 0;
};

class WalSegment {
 public:
  WalSegment() = default;
  ~WalSegment() { (void)CloseAfterSync(); }

  WalSegment(const WalSegment&) = delete;
  WalSegment& operator=(const WalSegment&) = delete;

  static Result<std::unique_ptr<WalSegment>> Open(const std::string& path,
                                                  std::uint64_t size,
                                                  std::uint64_t file_no) {
    if (size < kWalHeaderSize + kWalRecordHeaderSize || size > kRecordOffsetLimit) {
      return Status(StatusCode::kWalFull, "invalid WAL segment size");
    }
    if (!MakeRecordPosition(file_no, kWalHeaderSize).ok()) {
      return Status(StatusCode::kCorruptWal, "invalid WAL file number");
    }

    const int fd = ::open(path.c_str(), O_RDWR | O_CREAT, 0600);
    if (fd < 0) {
      return IoError("open", path);
    }

    struct stat st {};
    if (::fstat(fd, &st) != 0) {
      Status status = IoError("fstat", path);
      ::close(fd);
      return status;
    }

    bool metadata_dirty = false;
    if (st.st_size == 0) {
      if (::ftruncate(fd, static_cast<off_t>(size)) != 0) {
        Status status = IoError("ftruncate", path);
        ::close(fd);
        return status;
      }
      metadata_dirty = true;
    } else if (st.st_size != static_cast<off_t>(size)) {
      ::close(fd);
      return Status(StatusCode::kCorruptWal, "WAL segment size mismatch");
    }

    void* data = ::mmap(nullptr, static_cast<std::size_t>(size), PROT_READ | PROT_WRITE,
                        MAP_SHARED, fd, 0);
    if (data == MAP_FAILED) {
      Status status = IoError("mmap", path);
      ::close(fd);
      return status;
    }

    auto segment = std::unique_ptr<WalSegment>(new WalSegment());
    segment->path_ = path;
    segment->fd_ = fd;
    segment->data_ = static_cast<std::byte*>(data);
    segment->size_ = size;
    segment->file_no_ = file_no;
    segment->metadata_dirty_ = metadata_dirty;

    if (IsZeroBytes(segment->data_, kWalHeaderSize)) {
      segment->InitHeader();
    } else {
      Status status = segment->LoadHeader();
      if (!status.ok()) {
        (void)::munmap(segment->data_, static_cast<std::size_t>(segment->size_));
        (void)::close(segment->fd_);
        segment->data_ = nullptr;
        segment->fd_ = -1;
        return status;
      }
    }
    return segment;
  }

  [[nodiscard]] std::uint64_t file_no() const { return file_no_; }
  [[nodiscard]] std::uint64_t used() const { return used_; }
  [[nodiscard]] bool empty() const { return used_ == kWalHeaderSize; }
  void set_sealed(bool sealed) { sealed_ = sealed; }

  Result<minpatricia::Position> Append(std::uint8_t op, minpatricia::ByteView key,
                                       minpatricia::ByteView value) {
    if (sealed_) {
      return Status(StatusCode::kWalFull, "WAL segment is sealed");
    }
    if (key.size() > std::numeric_limits<std::uint32_t>::max() ||
        value.size() > std::numeric_limits<std::uint32_t>::max()) {
      return Status(StatusCode::kWalFull, "WAL record is too large");
    }
    const std::uint64_t total = kWalRecordHeaderSize + key.size() + value.size();
    if (total > size_ - used_ || used_ + total > kRecordOffsetLimit) {
      return Status(StatusCode::kWalFull, "WAL segment is full");
    }

    const std::uint64_t start = used_;
    std::byte* record = data_ + start;
    record[kWalRecordOpOffset] = static_cast<std::byte>(op);
    Store32(record + kWalRecordKeyOffset, static_cast<std::uint32_t>(key.size()));
    Store32(record + kWalRecordValueOffset, static_cast<std::uint32_t>(value.size()));
    std::memcpy(record + kWalRecordHeaderSize, key.data(), key.size());
    std::memcpy(record + kWalRecordHeaderSize + key.size(), value.data(), value.size());
    Store32(record + kWalRecordCRCOffset,
            WalRecordCRC(minpatricia::ByteView(record, static_cast<std::size_t>(total))));

    used_ += total;
    WriteUsed();
    return MakeRecordPosition(file_no_, start);
  }

  Result<WalRecord> RecordAt(minpatricia::Position pos, bool verify_crc) {
    if (RecordPositionFileNo(pos) != file_no_) {
      return CorruptWal("record position points at a different WAL segment");
    }
    return RecordAtOffset(RecordPositionOffset(pos), verify_crc);
  }

  Result<WalRecord> RecordAtOffset(std::uint64_t offset, bool verify_crc) {
    if (offset < kWalHeaderSize || offset + kWalRecordHeaderSize > used_) {
      return CorruptWal("WAL record offset out of range");
    }

    std::byte* header = data_ + offset;
    const auto op = static_cast<std::uint8_t>(header[kWalRecordOpOffset]);
    if (op != kWalOpPut && op != kWalOpDelete) {
      return CorruptWal("invalid WAL op");
    }
    const std::uint64_t key_len = Load32(header + kWalRecordKeyOffset);
    const std::uint64_t value_len = Load32(header + kWalRecordValueOffset);
    if (op == kWalOpDelete && value_len != 0) {
      return CorruptWal("delete WAL record has a value");
    }
    if (key_len > minpatricia::kMaxKeySize) {
      return CorruptWal("WAL key is too large");
    }
    const std::uint64_t end = offset + kWalRecordHeaderSize + key_len + value_len;
    if (end < offset || end > used_) {
      return CorruptWal("WAL record extends past used offset");
    }
    auto record = minpatricia::ByteView(data_ + offset,
                                        static_cast<std::size_t>(end - offset));
    if (verify_crc) {
      const std::uint32_t want = Load32(header + kWalRecordCRCOffset);
      const std::uint32_t got = WalRecordCRC(record);
      if (got != want) {
        return CorruptWal("WAL CRC mismatch");
      }
    }
    return WalRecord{op,
                     minpatricia::ByteView(data_ + offset + kWalRecordHeaderSize,
                                           static_cast<std::size_t>(key_len)),
                     minpatricia::ByteView(data_ + offset + kWalRecordHeaderSize + key_len,
                                           static_cast<std::size_t>(value_len)),
                     end};
  }

  template <class Fn>
  Status Replay(WALReplayPolicy policy, Fn&& fn, bool* truncated) {
    if (truncated != nullptr) {
      *truncated = false;
    }

    std::uint64_t offset = kWalHeaderSize;
    std::uint64_t last_good = offset;
    while (offset < used_) {
      auto record = RecordAtOffset(offset, true);
      if (!record.ok()) {
        if (policy == WALReplayPolicy::kStrict) {
          return record.status();
        }
        Truncate(last_good);
        if (truncated != nullptr) {
          *truncated = true;
        }
        return OkStatus();
      }
      auto pos = MakeRecordPosition(file_no_, offset);
      if (!pos.ok()) {
        return pos.status();
      }
      Status status = fn(record.value(), pos.value());
      if (!status.ok()) {
        return status;
      }
      offset = record.value().end;
      last_good = offset;
    }
    if (offset != used_) {
      if (policy == WALReplayPolicy::kStrict) {
        return CorruptWal("WAL replay ended off the used offset");
      }
      Truncate(last_good);
      if (truncated != nullptr) {
        *truncated = true;
      }
    }
    return OkStatus();
  }

  Status Sync() {
    if (data_dirty_) {
      if (::msync(data_, static_cast<std::size_t>(size_), MS_SYNC) != 0) {
        return IoError("msync", path_);
      }
      data_dirty_ = false;
    }
    if (metadata_dirty_) {
      if (::fsync(fd_) != 0) {
        return IoError("fsync", path_);
      }
      metadata_dirty_ = false;
    }
    return OkStatus();
  }

  Status Close(bool sync) {
    Status first;
    if (sync) {
      first = Sync();
    }
    Status close = CloseAfterSync();
    if (!first.ok()) {
      return first;
    }
    return close;
  }

 private:
  Status LoadHeader() {
    if (std::memcmp(data_, kWalHeaderMagic.data(), kWalHeaderMagic.size()) != 0) {
      return CorruptWal("invalid WAL magic");
    }
    if (Load32(data_ + kWalHeaderVersionOffset) != kWalVersion) {
      return CorruptWal("unsupported WAL version");
    }
    const std::uint64_t used = Load64(data_ + kWalHeaderUsedOffset);
    if (used < kWalHeaderSize || used > size_) {
      return CorruptWal("invalid WAL used offset");
    }
    used_ = used;
    return OkStatus();
  }

  void InitHeader() {
    std::memcpy(data_, kWalHeaderMagic.data(), kWalHeaderMagic.size());
    Store32(data_ + kWalHeaderVersionOffset, kWalVersion);
    used_ = kWalHeaderSize;
    WriteUsed();
  }

  void WriteUsed() {
    Store64(data_ + kWalHeaderUsedOffset, used_);
    data_dirty_ = true;
  }

  void Truncate(std::uint64_t used) {
    used_ = used;
    WriteUsed();
  }

  Status CloseAfterSync() {
    Status first;
    if (data_ != nullptr) {
      if (::munmap(data_, static_cast<std::size_t>(size_)) != 0) {
        first = IoError("munmap", path_);
      }
      data_ = nullptr;
    }
    if (fd_ >= 0) {
      if (::close(fd_) != 0 && first.ok()) {
        first = IoError("close", path_);
      }
      fd_ = -1;
    }
    return first;
  }

  std::string path_;
  int fd_ = -1;
  std::byte* data_ = nullptr;
  std::uint64_t size_ = 0;
  std::uint64_t used_ = 0;
  std::uint64_t file_no_ = 0;
  bool sealed_ = false;
  bool data_dirty_ = false;
  bool metadata_dirty_ = false;
};

class SegmentedRecordStore {
 public:
  static Result<std::unique_ptr<SegmentedRecordStore>> Open(const std::string& dir,
                                                            std::uint64_t wal_size) {
    if (wal_size < kWalHeaderSize + kWalRecordHeaderSize ||
        wal_size > kRecordOffsetLimit) {
      return Status(StatusCode::kInvalidArgument, "invalid WAL segment size");
    }
    Status status = CreateDirs(JoinPath(dir, kWalDirName));
    if (!status.ok()) {
      return status;
    }

    auto ids = ListWalSegmentIDs(JoinPath(dir, kWalDirName));
    if (!ids.ok()) {
      return ids.status();
    }
    if (ids.value().empty()) {
      ids.value().push_back(kFirstWalSegmentNo);
    }

    auto store = std::unique_ptr<SegmentedRecordStore>(new SegmentedRecordStore());
    store->dir_ = dir;
    store->wal_size_ = wal_size;
    store->active_file_no_ = ids.value().back();
    store->next_file_no_ = store->active_file_no_ + 1;

    for (const std::uint64_t id : ids.value()) {
      auto segment = WalSegment::Open(store->WalPath(id), wal_size, id);
      if (!segment.ok()) {
        return segment.status();
      }
      segment.value()->set_sealed(id != store->active_file_no_);
      store->segments_.emplace(id, segment.take_value());
    }
    return store;
  }

  Result<minpatricia::Position> Append(minpatricia::ByteView key,
                                       minpatricia::ByteView value) {
    auto result = Active()->Append(kWalOpPut, key, value);
    if (result.ok() || result.status() != StatusCode::kWalFull) {
      return result;
    }
    Status status = Rollover();
    if (!status.ok()) {
      return status;
    }
    return Active()->Append(kWalOpPut, key, value);
  }

  Result<minpatricia::Position> DeleteRecord(minpatricia::ByteView key) {
    auto result = Active()->Append(kWalOpDelete, key, minpatricia::ByteView{});
    if (result.ok() || result.status() != StatusCode::kWalFull) {
      return result;
    }
    Status status = Rollover();
    if (!status.ok()) {
      return status;
    }
    return Active()->Append(kWalOpDelete, key, minpatricia::ByteView{});
  }

  minpatricia::Result<minpatricia::ByteView> Key(minpatricia::Position pos) {
    WalSegment* segment = Segment(RecordPositionFileNo(pos));
    if (segment == nullptr) {
      return minpatricia::Status(minpatricia::StatusCode::kMissingKey);
    }
    auto record = segment->RecordAt(pos, false);
    if (!record.ok() || record.value().op != kWalOpPut) {
      return minpatricia::Status(minpatricia::StatusCode::kMissingKey);
    }
    return record.value().key;
  }

  Result<minpatricia::ByteView> ValueView(minpatricia::Position pos) {
    WalSegment* segment = Segment(RecordPositionFileNo(pos));
    if (segment == nullptr) {
      return Status(StatusCode::kCorruptIndex, "record segment not found");
    }
    auto record = segment->RecordAt(pos, false);
    if (!record.ok()) {
      return record.status();
    }
    if (record.value().op != kWalOpPut) {
      return Status(StatusCode::kCorruptIndex, "index points to a non-put record");
    }
    return record.value().value;
  }

  Result<std::string> OwnedValue(minpatricia::Position pos) {
    auto value = ValueView(pos);
    if (!value.ok()) {
      return value.status();
    }
    return minpatricia::ToString(value.value());
  }

  Status ReplayAll(WALReplayPolicy policy,
                   minpatricia::Index<SegmentedRecordStore, minpatricia::HeapNodeStore>* index) {
    std::vector<std::uint64_t> ids;
    ids.reserve(segments_.size());
    for (const auto& entry : segments_) {
      ids.push_back(entry.first);
    }
    std::sort(ids.begin(), ids.end());

    bool drop_remaining = false;
    for (std::uint64_t id : ids) {
      if (drop_remaining) {
        remove(WalPath(id).c_str());
        segments_.erase(id);
        continue;
      }

      bool truncated = false;
      Status status = segments_.at(id)->Replay(policy,
          [&](const WalRecord& record, minpatricia::Position pos) -> Status {
            if (record.op == kWalOpPut) {
              auto put = index->Put(record.key, pos);
              if (!put.ok()) {
                return FromMinpatriciaStatus(put.status());
              }
              return OkStatus();
            }
            if (record.op == kWalOpDelete) {
              auto deleted = index->Delete(record.key);
              if (!deleted.ok()) {
                return FromMinpatriciaStatus(deleted.status());
              }
              return OkStatus();
            }
            return CorruptWal("unsupported WAL op");
          },
          &truncated);
      if (!status.ok()) {
        return status;
      }
      if (truncated) {
        drop_remaining = true;
      }
    }
    return OkStatus();
  }

  Status Sync() {
    Status first;
    for (auto& entry : segments_) {
      Status status = entry.second->Sync();
      if (!status.ok() && first.ok()) {
        first = status;
      }
    }
    return first;
  }

  Status Close(bool sync) {
    Status first;
    for (auto& entry : segments_) {
      Status status = entry.second->Close(sync);
      if (!status.ok() && first.ok()) {
        first = status;
      }
    }
    segments_.clear();
    return first;
  }

 private:
  SegmentedRecordStore() = default;

  WalSegment* Segment(std::uint64_t file_no) {
    auto it = segments_.find(file_no);
    if (it == segments_.end()) {
      return nullptr;
    }
    return it->second.get();
  }

  WalSegment* Active() { return Segment(active_file_no_); }

  Status Rollover() {
    WalSegment* old = Active();
    if (old == nullptr) {
      return Status(StatusCode::kClosed, "WAL is closed");
    }
    old->set_sealed(true);
    const std::uint64_t id = next_file_no_++;
    auto segment = WalSegment::Open(WalPath(id), wal_size_, id);
    if (!segment.ok()) {
      old->set_sealed(false);
      --next_file_no_;
      return segment.status();
    }
    segment.value()->set_sealed(false);
    active_file_no_ = id;
    segments_.emplace(id, segment.take_value());
    return OkStatus();
  }

  std::string WalPath(std::uint64_t id) const {
    return JoinPath(JoinPath(dir_, kWalDirName), WalSegmentName(id));
  }

  std::string dir_;
  std::uint64_t wal_size_ = 0;
  std::uint64_t active_file_no_ = 0;
  std::uint64_t next_file_no_ = 0;
  std::unordered_map<std::uint64_t, std::unique_ptr<WalSegment>> segments_;
};

class StdMutex final : public Mutex {
 public:
  void Lock() override { mu_.lock(); }
  void Unlock() override { mu_.unlock(); }

 private:
  std::mutex mu_;
};

class StdRWMutex final : public RWMutex {
 public:
  void Lock() override { mu_.lock(); }
  void Unlock() override { mu_.unlock(); }
  void RLock() override { mu_.lock_shared(); }
  void RUnlock() override { mu_.unlock_shared(); }

 private:
  std::shared_mutex mu_;
};

}  // namespace

std::shared_ptr<StdRuntime> StdRuntime::Shared() {
  static auto runtime = std::make_shared<StdRuntime>();
  return runtime;
}

std::unique_ptr<Mutex> StdRuntime::NewMutex() {
  return std::unique_ptr<Mutex>(new StdMutex());
}

std::unique_ptr<RWMutex> StdRuntime::NewRWMutex() {
  return std::unique_ptr<RWMutex>(new StdRWMutex());
}

Status StdRuntime::BlockingIO(std::string_view, Task task) {
  if (!task) {
    return OkStatus();
  }
  return task();
}

class Store::Impl {
 public:
  using Index = minpatricia::Index<SegmentedRecordStore, minpatricia::HeapNodeStore>;

  static Result<std::unique_ptr<Impl>> Open(const std::string& dir, Options options) {
    if (options.runtime == nullptr) {
      options.runtime = StdRuntime::Shared();
    }
    if (options.wal_size < kWalHeaderSize + kWalRecordHeaderSize ||
        options.wal_size > kRecordOffsetLimit) {
      return Status(StatusCode::kInvalidArgument, "invalid WAL size");
    }

    Status status = options.runtime->BlockingIO("open_store_dirs", [&] {
      return CreateDirs(dir);
    });
    if (!status.ok()) {
      return status;
    }

    auto records = SegmentedRecordStore::Open(dir, options.wal_size);
    if (!records.ok()) {
      return records.status();
    }
    auto nodes = std::unique_ptr<minpatricia::HeapNodeStore>(new minpatricia::HeapNodeStore());
    auto index = Index::NewWithNodes(*records.value(), *nodes);
    if (!index.ok()) {
      return FromMinpatriciaStatus(index.status());
    }

    status = records.value()->ReplayAll(options.wal_replay_policy, &index.value());
    if (!status.ok()) {
      return status;
    }

    auto impl = std::unique_ptr<Impl>(new Impl());
    impl->runtime_ = options.runtime;
    impl->primary_mu_ = impl->runtime_->NewRWMutex();
    impl->records_ = records.take_value();
    impl->nodes_ = std::move(nodes);
    impl->index_ = std::unique_ptr<Index>(new Index(index.take_value()));
    impl->verify_index_on_read_ = options.verify_index_on_read;
    impl->sync_on_close_ = options.sync_on_close;
    impl->open_ = true;
    return impl;
  }

  static Result<std::unique_ptr<Impl>> New(Options options) {
    if (options.runtime == nullptr) {
      options.runtime = StdRuntime::Shared();
    }
    auto records = SegmentedRecordStore::Open(InMemoryTempPath(), options.wal_size);
    if (!records.ok()) {
      return records.status();
    }
    auto nodes = std::unique_ptr<minpatricia::HeapNodeStore>(new minpatricia::HeapNodeStore());
    auto index = Index::NewWithNodes(*records.value(), *nodes);
    if (!index.ok()) {
      return FromMinpatriciaStatus(index.status());
    }
    auto impl = std::unique_ptr<Impl>(new Impl());
    impl->runtime_ = options.runtime;
    impl->primary_mu_ = impl->runtime_->NewRWMutex();
    impl->records_ = records.take_value();
    impl->nodes_ = std::move(nodes);
    impl->index_ = std::unique_ptr<Index>(new Index(index.take_value()));
    impl->verify_index_on_read_ = options.verify_index_on_read;
    impl->sync_on_close_ = options.sync_on_close;
    impl->open_ = true;
    return impl;
  }

  Status Close() {
    std::unique_ptr<SegmentedRecordStore> records;
    bool sync_on_close = false;
    {
      WriteLock lock(*primary_mu_);
      if (!open_) {
        return OkStatus();
      }
      open_ = false;
      sync_on_close = sync_on_close_;
      records = std::move(records_);
      index_.reset();
      nodes_.reset();
    }
    if (records == nullptr) {
      return OkStatus();
    }
    return runtime_->BlockingIO("close_store", [&] {
      return records->Close(sync_on_close);
    });
  }

  Result<std::size_t> Len() {
    ReadLock lock(*primary_mu_);
    Status status = CheckOpen();
    if (!status.ok()) {
      return status;
    }
    return static_cast<std::size_t>(index_->Len());
  }

  Status Put(ByteView key, ByteView value) {
    WriteLock lock(*primary_mu_);
    Status status = CheckOpen();
    if (!status.ok()) {
      return status;
    }
    if (key.size() > minpatricia::kMaxKeySize) {
      return Status(StatusCode::kInvalidArgument, "key too large");
    }
    auto pos = records_->Append(key, value);
    if (!pos.ok()) {
      return pos.status();
    }
    auto put = index_->Put(key, pos.value());
    if (!put.ok()) {
      return MarkFatal(FromMinpatriciaStatus(put.status()));
    }
    return OkStatus();
  }

  Result<GetResult> Get(ByteView key) {
    ReadLock lock(*primary_mu_);
    Status status = CheckOpen();
    if (!status.ok()) {
      return status;
    }
    auto pos = index_->Get(key);
    if (!pos.ok()) {
      return FromMinpatriciaStatus(pos.status());
    }
    if (!pos.value().found) {
      return GetResult{};
    }
    status = VerifyReadPosition(key, pos.value().pos);
    if (!status.ok()) {
      return status;
    }
    auto value = records_->OwnedValue(pos.value().pos);
    if (!value.ok()) {
      return value.status();
    }
    return GetResult{value.take_value(), true};
  }

  Result<bool> Delete(ByteView key) {
    WriteLock lock(*primary_mu_);
    Status status = CheckOpen();
    if (!status.ok()) {
      return status;
    }
    auto pos = index_->Get(key);
    if (!pos.ok()) {
      return FromMinpatriciaStatus(pos.status());
    }
    if (!pos.value().found) {
      return false;
    }
    if (key.size() > minpatricia::kMaxKeySize) {
      return Status(StatusCode::kInvalidArgument, "key too large");
    }
    auto tombstone = records_->DeleteRecord(key);
    if (!tombstone.ok()) {
      return tombstone.status();
    }
    auto deleted = index_->Delete(key);
    if (!deleted.ok()) {
      return MarkFatal(FromMinpatriciaStatus(deleted.status()));
    }
    if (!deleted.value().deleted || deleted.value().pos != pos.value().pos) {
      return MarkFatal(Status(StatusCode::kCorruptIndex, "delete position mismatch"));
    }
    return true;
  }

  Status Scan(const VisitFunc& fn) {
    ReadLock lock(*primary_mu_);
    Status status = CheckOpen();
    if (!status.ok()) {
      return status;
    }
    return VisitIndex([&](auto&& visit) { return index_->Ascend(visit); }, fn);
  }

  Status ScanRange(ByteView ge, ByteView lt, const VisitFunc& fn) {
    if (minpatricia::CompareKeys(ge, lt) > 0) {
      return Status(StatusCode::kInvalidArgument, "invalid scan range");
    }
    ReadLock lock(*primary_mu_);
    Status status = CheckOpen();
    if (!status.ok()) {
      return status;
    }
    return VisitIndex([&](auto&& visit) { return index_->AscendRange(ge, lt, visit); }, fn);
  }

  Status ReverseScan(const VisitFunc& fn) {
    ReadLock lock(*primary_mu_);
    Status status = CheckOpen();
    if (!status.ok()) {
      return status;
    }
    return VisitIndex([&](auto&& visit) { return index_->Descend(visit); }, fn);
  }

  Status ReverseScanRange(ByteView le, ByteView gt, const VisitFunc& fn) {
    if (minpatricia::CompareKeys(gt, le) > 0) {
      return Status(StatusCode::kInvalidArgument, "invalid reverse scan range");
    }
    ReadLock lock(*primary_mu_);
    Status status = CheckOpen();
    if (!status.ok()) {
      return status;
    }
    return VisitIndex([&](auto&& visit) { return index_->DescendRange(le, gt, visit); }, fn);
  }

  Result<SeekResult> SeekGE(ByteView key) {
    ReadLock lock(*primary_mu_);
    Status status = CheckOpen();
    if (!status.ok()) {
      return status;
    }

    SeekResult result;
    Status visit_status;
    auto status2 = index_->AscendGreaterOrEqual(
        key, [&](minpatricia::ByteView found_key, minpatricia::Position pos) {
          auto item = MakeItem(found_key, pos);
          if (!item.ok()) {
            visit_status = item.status();
            return false;
          }
          result.item = item.take_value();
          result.found = true;
          return false;
        });
    if (!visit_status.ok()) {
      return visit_status;
    }
    if (!status2.ok()) {
      return FromMinpatriciaStatus(status2);
    }
    return result;
  }

  Result<SeekResult> SeekLE(ByteView key) {
    ReadLock lock(*primary_mu_);
    Status status = CheckOpen();
    if (!status.ok()) {
      return status;
    }

    SeekResult result;
    Status visit_status;
    auto status2 = index_->DescendLessOrEqual(
        key, [&](minpatricia::ByteView found_key, minpatricia::Position pos) {
          auto item = MakeItem(found_key, pos);
          if (!item.ok()) {
            visit_status = item.status();
            return false;
          }
          result.item = item.take_value();
          result.found = true;
          return false;
        });
    if (!visit_status.ok()) {
      return visit_status;
    }
    if (!status2.ok()) {
      return FromMinpatriciaStatus(status2);
    }
    return result;
  }

 private:
  Impl() = default;

  static std::string InMemoryTempPath() {
    static std::atomic<std::uint64_t> counter{0};
    std::ostringstream out;
    out << ".runtime/minweight_store_new_" << ::getpid() << "_"
        << std::chrono::steady_clock::now().time_since_epoch().count() << "_"
        << counter.fetch_add(1, std::memory_order_relaxed);
    return out.str();
  }

  Status CheckOpen() const {
    if (!open_ || records_ == nullptr || index_ == nullptr) {
      return Status(StatusCode::kClosed, "store is closed");
    }
    if (!fatal_.ok()) {
      return fatal_;
    }
    return OkStatus();
  }

  Status MarkFatal(Status cause) {
    if (!fatal_.ok()) {
      return fatal_;
    }
    if (cause == StatusCode::kClosed) {
      return cause;
    }
    std::string message = "store is fatal";
    if (!cause.message().empty()) {
      message += ": " + cause.message();
    }
    fatal_ = Status(StatusCode::kFatal, std::move(message));
    return fatal_;
  }

  Status VerifyReadPosition(ByteView key, minpatricia::Position pos) {
    if (!verify_index_on_read_) {
      return OkStatus();
    }
    auto probe = index_->Probe(key);
    if (!probe.ok()) {
      return FromMinpatriciaStatus(probe.status());
    }
    if (!probe.value().found || probe.value().pos != pos) {
      return Status(StatusCode::kCorruptIndex, "index verification failed");
    }
    return OkStatus();
  }

  Result<Item> MakeItem(minpatricia::ByteView key, minpatricia::Position pos) {
    Status status = VerifyReadPosition(key, pos);
    if (!status.ok()) {
      return status;
    }
    auto value = records_->OwnedValue(pos);
    if (!value.ok()) {
      return value.status();
    }
    return Item{minpatricia::ToString(key), value.take_value()};
  }

  template <class IndexVisit>
  Status VisitIndex(IndexVisit&& index_visit, const VisitFunc& fn) {
    Status visit_status;
    auto status = index_visit([&](minpatricia::ByteView key, minpatricia::Position pos) {
      auto item = MakeItem(key, pos);
      if (!item.ok()) {
        visit_status = item.status();
        return false;
      }
      return fn(item.value());
    });
    if (!visit_status.ok()) {
      return visit_status;
    }
    if (!status.ok()) {
      return FromMinpatriciaStatus(status);
    }
    return OkStatus();
  }

  std::shared_ptr<Runtime> runtime_;
  std::unique_ptr<RWMutex> primary_mu_;
  std::unique_ptr<SegmentedRecordStore> records_;
  std::unique_ptr<minpatricia::HeapNodeStore> nodes_;
  std::unique_ptr<Index> index_;
  bool verify_index_on_read_ = false;
  bool sync_on_close_ = true;
  bool open_ = false;
  Status fatal_;
};

Result<std::unique_ptr<Store>> Store::Open(const std::string& dir, Options options) {
  auto impl = Impl::Open(dir, std::move(options));
  if (!impl.ok()) {
    return impl.status();
  }
  return std::unique_ptr<Store>(new Store(impl.take_value()));
}

Result<std::unique_ptr<Store>> Store::New(Options options) {
  auto impl = Impl::New(std::move(options));
  if (!impl.ok()) {
    return impl.status();
  }
  return std::unique_ptr<Store>(new Store(impl.take_value()));
}

Store::Store() = default;

Store::Store(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

Store::~Store() {
  if (impl_ != nullptr) {
    (void)impl_->Close();
  }
}

Status Store::Close() {
  if (impl_ == nullptr) {
    return OkStatus();
  }
  return impl_->Close();
}

Result<std::size_t> Store::Len() {
  if (impl_ == nullptr) {
    return Status(StatusCode::kClosed, "store is closed");
  }
  return impl_->Len();
}

Status Store::Put(ByteView key, ByteView value) {
  if (impl_ == nullptr) {
    return Status(StatusCode::kClosed, "store is closed");
  }
  return impl_->Put(key, value);
}

Result<GetResult> Store::Get(ByteView key) {
  if (impl_ == nullptr) {
    return Status(StatusCode::kClosed, "store is closed");
  }
  return impl_->Get(key);
}

Result<bool> Store::Delete(ByteView key) {
  if (impl_ == nullptr) {
    return Status(StatusCode::kClosed, "store is closed");
  }
  return impl_->Delete(key);
}

Status Store::Scan(const VisitFunc& fn) {
  if (impl_ == nullptr) {
    return Status(StatusCode::kClosed, "store is closed");
  }
  return impl_->Scan(fn);
}

Status Store::ScanRange(ByteView greater_or_equal, ByteView less_than, const VisitFunc& fn) {
  if (impl_ == nullptr) {
    return Status(StatusCode::kClosed, "store is closed");
  }
  return impl_->ScanRange(greater_or_equal, less_than, fn);
}

Status Store::ReverseScan(const VisitFunc& fn) {
  if (impl_ == nullptr) {
    return Status(StatusCode::kClosed, "store is closed");
  }
  return impl_->ReverseScan(fn);
}

Status Store::ReverseScanRange(ByteView less_or_equal, ByteView greater_than,
                               const VisitFunc& fn) {
  if (impl_ == nullptr) {
    return Status(StatusCode::kClosed, "store is closed");
  }
  return impl_->ReverseScanRange(less_or_equal, greater_than, fn);
}

Result<SeekResult> Store::SeekGE(ByteView key) {
  if (impl_ == nullptr) {
    return Status(StatusCode::kClosed, "store is closed");
  }
  return impl_->SeekGE(key);
}

Result<SeekResult> Store::SeekLE(ByteView key) {
  if (impl_ == nullptr) {
    return Status(StatusCode::kClosed, "store is closed");
  }
  return impl_->SeekLE(key);
}

}  // namespace minweight_store
