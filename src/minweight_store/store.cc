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
constexpr std::uint64_t kFirstWalGeneration = 1;
constexpr const char* kWalDirName = "wal";
constexpr const char* kWalSegmentSuffix = ".wal";
constexpr const char* kManifestName = "MANIFEST";
constexpr const char* kSnapshotName = "SNAPSHOT";
constexpr const char* kTmpSuffix = ".tmp";
constexpr std::size_t kCheckpointWalSegmentThreshold = 4;

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
constexpr std::array<char, 8> kManifestMagic{{'M', 'W', 'M', 'A', 'N', '0', '1', 0}};
constexpr std::array<char, 8> kSnapshotMagic{{'M', 'W', 'S', 'N', 'A', 'P', '1', 0}};
constexpr std::uint32_t kManifestVersion = 1;
constexpr std::uint32_t kSnapshotVersion = 1;

Status IoError(const std::string& op, const std::string& path) {
  return Status(StatusCode::kIoError,
                op + " " + path + ": " + std::strerror(errno));
}

Status CorruptWal(const std::string& message) {
  return Status(StatusCode::kCorruptWal, message);
}

Status CorruptManifest(const std::string& message) {
  return Status(StatusCode::kCorruptManifest, message);
}

Status CorruptSnapshot(const std::string& message) {
  return Status(StatusCode::kCorruptSnapshot, message);
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

std::uint32_t BytesCRC(minpatricia::ByteView value) {
  return CRC32Update(0, value.data(), value.size());
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

Status WriteAll(int fd, const std::byte* data, std::size_t size,
                const std::string& path) {
  std::size_t written = 0;
  while (written < size) {
    const ssize_t n = ::write(fd, data + written, size - written);
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      return IoError("write", path);
    }
    if (n == 0) {
      return Status(StatusCode::kIoError, "short write " + path);
    }
    written += static_cast<std::size_t>(n);
  }
  return OkStatus();
}

Status ReadAll(int fd, std::byte* data, std::size_t size, const std::string& path) {
  std::size_t read_bytes = 0;
  while (read_bytes < size) {
    const ssize_t n = ::read(fd, data + read_bytes, size - read_bytes);
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      return IoError("read", path);
    }
    if (n == 0) {
      return Status(StatusCode::kIoError, "short read " + path);
    }
    read_bytes += static_cast<std::size_t>(n);
  }
  return OkStatus();
}

Status SyncDir(const std::string& path) {
  const int fd = ::open(path.c_str(), O_RDONLY | O_DIRECTORY);
  if (fd < 0) {
    return IoError("open_dir", path);
  }
  Status status;
  if (::fsync(fd) != 0) {
    status = IoError("fsync_dir", path);
  }
  if (::close(fd) != 0 && status.ok()) {
    status = IoError("close_dir", path);
  }
  return status;
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

std::string FixedWidthID(std::uint64_t id) {
  std::ostringstream out;
  out << std::setw(20) << std::setfill('0') << id;
  return out.str();
}

std::string WalGenerationName(std::uint64_t generation) {
  return FixedWidthID(generation);
}

std::string SnapshotFileName(std::uint64_t generation) {
  return std::string(kSnapshotName) + "." + FixedWidthID(generation);
}

std::string SnapshotPath(const std::string& dir, std::uint64_t generation) {
  return JoinPath(dir, SnapshotFileName(generation));
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

Result<std::vector<std::uint64_t>> ListWalGenerations(const std::string& wal_root) {
  DIR* dir = ::opendir(wal_root.c_str());
  if (dir == nullptr) {
    if (errno == ENOENT) {
      return std::vector<std::uint64_t>{};
    }
    return IoError("opendir", wal_root);
  }

  std::vector<std::uint64_t> generations;
  for (;;) {
    errno = 0;
    dirent* entry = ::readdir(dir);
    if (entry == nullptr) {
      if (errno != 0) {
        Status status = IoError("readdir", wal_root);
        ::closedir(dir);
        return status;
      }
      break;
    }
    const std::string name(entry->d_name);
    if (name == "." || name == ".." || name.size() != 20) {
      continue;
    }
    bool digits = true;
    std::uint64_t value = 0;
    for (char c : name) {
      if (c < '0' || c > '9') {
        digits = false;
        break;
      }
      value = value * 10 + static_cast<std::uint64_t>(c - '0');
    }
    if (digits && value > 0) {
      generations.push_back(value);
    }
  }
  if (::closedir(dir) != 0) {
    return IoError("closedir", wal_root);
  }
  std::sort(generations.begin(), generations.end());
  return generations;
}

Result<std::uint64_t> ParseSnapshotFileName(const std::string& name) {
  const std::string prefix = std::string(kSnapshotName) + ".";
  if (name.size() != prefix.size() + 20 ||
      name.compare(0, prefix.size(), prefix) != 0) {
    return Status(StatusCode::kInvalidArgument, "not a snapshot file");
  }
  std::uint64_t value = 0;
  for (std::size_t i = prefix.size(); i < name.size(); ++i) {
    if (name[i] < '0' || name[i] > '9') {
      return Status(StatusCode::kInvalidArgument, "not a snapshot file");
    }
    value = value * 10 + static_cast<std::uint64_t>(name[i] - '0');
  }
  if (value == 0) {
    return Status(StatusCode::kInvalidArgument, "invalid snapshot generation");
  }
  return value;
}

Result<std::vector<std::uint64_t>> ListSnapshotGenerations(const std::string& dir_path) {
  DIR* dir = ::opendir(dir_path.c_str());
  if (dir == nullptr) {
    if (errno == ENOENT) {
      return std::vector<std::uint64_t>{};
    }
    return IoError("opendir", dir_path);
  }

  std::vector<std::uint64_t> generations;
  for (;;) {
    errno = 0;
    dirent* entry = ::readdir(dir);
    if (entry == nullptr) {
      if (errno != 0) {
        Status status = IoError("readdir", dir_path);
        ::closedir(dir);
        return status;
      }
      break;
    }
    auto generation = ParseSnapshotFileName(entry->d_name);
    if (generation.ok()) {
      generations.push_back(generation.value());
    }
  }
  if (::closedir(dir) != 0) {
    return IoError("closedir", dir_path);
  }
  std::sort(generations.begin(), generations.end());
  return generations;
}

Status RemoveTree(const std::string& path) {
  DIR* dir = ::opendir(path.c_str());
  if (dir == nullptr) {
    if (errno == ENOENT) {
      return OkStatus();
    }
    if (::unlink(path.c_str()) == 0 || errno == ENOENT) {
      return OkStatus();
    }
    return IoError("remove", path);
  }

  Status first;
  for (;;) {
    errno = 0;
    dirent* entry = ::readdir(dir);
    if (entry == nullptr) {
      if (errno != 0 && first.ok()) {
        first = IoError("readdir", path);
      }
      break;
    }
    const std::string name(entry->d_name);
    if (name == "." || name == "..") {
      continue;
    }
    Status status = RemoveTree(JoinPath(path, name));
    if (!status.ok() && first.ok()) {
      first = status;
    }
  }
  if (::closedir(dir) != 0 && first.ok()) {
    first = IoError("closedir", path);
  }
  if (::rmdir(path.c_str()) != 0 && errno != ENOENT && first.ok()) {
    first = IoError("rmdir", path);
  }
  return first;
}

Status RemoveOldWalGenerations(const std::string& dir, std::uint64_t keep_generation) {
  const std::string wal_root = JoinPath(dir, kWalDirName);
  auto generations = ListWalGenerations(wal_root);
  if (!generations.ok()) {
    return generations.status();
  }
  Status first;
  for (std::uint64_t generation : generations.value()) {
    if (generation == keep_generation) {
      continue;
    }
    Status status = RemoveTree(JoinPath(wal_root, WalGenerationName(generation)));
    if (!status.ok() && first.ok()) {
      first = status;
    }
  }
  return first;
}

Status RemoveOldSnapshots(const std::string& dir, std::uint64_t keep_generation) {
  auto generations = ListSnapshotGenerations(dir);
  if (!generations.ok()) {
    return generations.status();
  }
  Status first;
  for (std::uint64_t generation : generations.value()) {
    if (generation == keep_generation) {
      continue;
    }
    const std::string path = SnapshotPath(dir, generation);
    if (::unlink(path.c_str()) != 0 && errno != ENOENT && first.ok()) {
      first = IoError("unlink", path);
    }
  }
  return first;
}

struct WalRecord {
  std::uint8_t op = 0;
  minpatricia::ByteView key;
  minpatricia::ByteView value;
  std::uint64_t end = 0;
};

struct ManifestState {
  bool valid = false;
  std::uint64_t wal_generation = kFirstWalGeneration;
};

std::string ManifestPath(const std::string& dir) {
  return JoinPath(dir, kManifestName);
}

Result<ManifestState> ReadManifest(const std::string& dir) {
  const std::string path = ManifestPath(dir);
  const int fd = ::open(path.c_str(), O_RDONLY);
  if (fd < 0) {
    if (errno == ENOENT) {
      return ManifestState{};
    }
    return IoError("open", path);
  }

  std::array<std::byte, 32> data{};
  Status status = ReadAll(fd, data.data(), data.size(), path);
  if (::close(fd) != 0 && status.ok()) {
    status = IoError("close", path);
  }
  if (!status.ok()) {
    return status;
  }

  if (std::memcmp(data.data(), kManifestMagic.data(), kManifestMagic.size()) != 0) {
    return CorruptManifest("invalid manifest magic");
  }
  if (Load32(data.data() + 8) != kManifestVersion) {
    return CorruptManifest("unsupported manifest version");
  }
  const std::uint64_t wal_generation = Load64(data.data() + 16);
  if (wal_generation == 0) {
    return CorruptManifest("invalid WAL generation");
  }
  const std::uint32_t want = Load32(data.data() + 28);
  Store32(data.data() + 28, 0);
  const std::uint32_t got = BytesCRC(minpatricia::ByteView(data.data(), data.size()));
  if (got != want) {
    return CorruptManifest("manifest CRC mismatch");
  }
  return ManifestState{true, wal_generation};
}

Status WriteManifest(const std::string& dir, std::uint64_t wal_generation) {
  if (wal_generation == 0) {
    return CorruptManifest("invalid WAL generation");
  }

  std::array<std::byte, 32> data{};
  std::memcpy(data.data(), kManifestMagic.data(), kManifestMagic.size());
  Store32(data.data() + 8, kManifestVersion);
  Store64(data.data() + 16, wal_generation);
  Store32(data.data() + 28, 0);
  Store32(data.data() + 28, BytesCRC(minpatricia::ByteView(data.data(), data.size())));

  const std::string path = ManifestPath(dir);
  const std::string tmp = path + kTmpSuffix;
  const int fd = ::open(tmp.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0600);
  if (fd < 0) {
    return IoError("open", tmp);
  }
  Status status = WriteAll(fd, data.data(), data.size(), tmp);
  if (status.ok() && ::fsync(fd) != 0) {
    status = IoError("fsync", tmp);
  }
  if (::close(fd) != 0 && status.ok()) {
    status = IoError("close", tmp);
  }
  if (!status.ok()) {
    (void)::unlink(tmp.c_str());
    return status;
  }
  if (::rename(tmp.c_str(), path.c_str()) != 0) {
    (void)::unlink(tmp.c_str());
    return IoError("rename", path);
  }
  return SyncDir(dir);
}

Status WriteSnapshot(const std::string& dir, std::uint64_t generation,
                     const std::vector<Item>& entries) {
  if (generation == 0) {
    return CorruptSnapshot("invalid snapshot generation");
  }
  const std::string path = SnapshotPath(dir, generation);
  const std::string tmp = path + kTmpSuffix;
  const int fd = ::open(tmp.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0600);
  if (fd < 0) {
    return IoError("open", tmp);
  }

  std::array<std::byte, 32> header{};
  std::memcpy(header.data(), kSnapshotMagic.data(), kSnapshotMagic.size());
  Store32(header.data() + 8, kSnapshotVersion);
  Store64(header.data() + 16, static_cast<std::uint64_t>(entries.size()));
  Store64(header.data() + 24, generation);
  Status status = WriteAll(fd, header.data(), header.size(), tmp);

  for (const Item& item : entries) {
    if (!status.ok()) {
      break;
    }
    if (item.key.size() > std::numeric_limits<std::uint32_t>::max() ||
        item.value.size() > std::numeric_limits<std::uint32_t>::max()) {
      status = CorruptSnapshot("snapshot item is too large");
      break;
    }
    std::array<std::byte, 12> record_header{};
    Store32(record_header.data(), static_cast<std::uint32_t>(item.key.size()));
    Store32(record_header.data() + 4, static_cast<std::uint32_t>(item.value.size()));
    std::uint32_t crc = CRC32Update(0, record_header.data(), 8);
    crc = CRC32Update(crc, reinterpret_cast<const std::byte*>(item.key.data()),
                      item.key.size());
    crc = CRC32Update(crc, reinterpret_cast<const std::byte*>(item.value.data()),
                      item.value.size());
    Store32(record_header.data() + 8, crc);
    status = WriteAll(fd, record_header.data(), record_header.size(), tmp);
    if (status.ok()) {
      status = WriteAll(fd, reinterpret_cast<const std::byte*>(item.key.data()),
                        item.key.size(), tmp);
    }
    if (status.ok()) {
      status = WriteAll(fd, reinterpret_cast<const std::byte*>(item.value.data()),
                        item.value.size(), tmp);
    }
  }

  if (status.ok() && ::fsync(fd) != 0) {
    status = IoError("fsync", tmp);
  }
  if (::close(fd) != 0 && status.ok()) {
    status = IoError("close", tmp);
  }
  if (!status.ok()) {
    (void)::unlink(tmp.c_str());
    return status;
  }
  if (::rename(tmp.c_str(), path.c_str()) != 0) {
    (void)::unlink(tmp.c_str());
    return IoError("rename", path);
  }
  return SyncDir(dir);
}

Result<std::vector<Item>> ReadSnapshot(const std::string& dir, std::uint64_t generation) {
  if (generation == 0) {
    return CorruptSnapshot("invalid snapshot generation");
  }
  const std::string path = SnapshotPath(dir, generation);
  const int fd = ::open(path.c_str(), O_RDONLY);
  if (fd < 0) {
    if (errno == ENOENT) {
      return CorruptSnapshot("snapshot missing for manifest generation");
    }
    return IoError("open", path);
  }

  std::array<std::byte, 32> header{};
  Status status = ReadAll(fd, header.data(), header.size(), path);
  if (!status.ok()) {
    (void)::close(fd);
    return status;
  }
  if (std::memcmp(header.data(), kSnapshotMagic.data(), kSnapshotMagic.size()) != 0) {
    (void)::close(fd);
    return CorruptSnapshot("invalid snapshot magic");
  }
  if (Load32(header.data() + 8) != kSnapshotVersion) {
    (void)::close(fd);
    return CorruptSnapshot("unsupported snapshot version");
  }
  const std::uint64_t count = Load64(header.data() + 16);
  const std::uint64_t snapshot_generation = Load64(header.data() + 24);
  if (snapshot_generation != generation) {
    (void)::close(fd);
    return CorruptSnapshot("snapshot generation mismatch");
  }
  if (count > static_cast<std::uint64_t>(kRecordOffsetLimit - 1)) {
    (void)::close(fd);
    return CorruptSnapshot("snapshot has too many records");
  }

  std::vector<Item> entries;
  entries.reserve(static_cast<std::size_t>(count));
  for (std::uint64_t i = 0; i < count; ++i) {
    std::array<std::byte, 12> record_header{};
    status = ReadAll(fd, record_header.data(), record_header.size(), path);
    if (!status.ok()) {
      (void)::close(fd);
      return status;
    }
    const std::uint32_t key_size = Load32(record_header.data());
    const std::uint32_t value_size = Load32(record_header.data() + 4);
    const std::uint32_t want_crc = Load32(record_header.data() + 8);
    if (key_size > minpatricia::kMaxKeySize) {
      (void)::close(fd);
      return CorruptSnapshot("snapshot key is too large");
    }
    Item item;
    item.key.resize(key_size);
    item.value.resize(value_size);
    if (key_size != 0) {
      status = ReadAll(fd, reinterpret_cast<std::byte*>(&item.key[0]), key_size, path);
    }
    if (status.ok() && value_size != 0) {
      status = ReadAll(fd, reinterpret_cast<std::byte*>(&item.value[0]), value_size, path);
    }
    if (!status.ok()) {
      (void)::close(fd);
      return status;
    }
    Store32(record_header.data() + 8, 0);
    std::uint32_t crc = CRC32Update(0, record_header.data(), 8);
    crc = CRC32Update(crc, reinterpret_cast<const std::byte*>(item.key.data()),
                      item.key.size());
    crc = CRC32Update(crc, reinterpret_cast<const std::byte*>(item.value.data()),
                      item.value.size());
    if (crc != want_crc) {
      (void)::close(fd);
      return CorruptSnapshot("snapshot record CRC mismatch");
    }
    entries.push_back(std::move(item));
  }

  if (::close(fd) != 0) {
    return IoError("close", path);
  }
  return entries;
}

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
                                                            std::uint64_t wal_size,
                                                            std::uint64_t generation) {
    if (wal_size < kWalHeaderSize + kWalRecordHeaderSize ||
        wal_size > kRecordOffsetLimit) {
      return Status(StatusCode::kInvalidArgument, "invalid WAL segment size");
    }
    if (generation == 0) {
      return CorruptManifest("invalid WAL generation");
    }
    Status status = CreateDirs(WalGenerationPath(dir, generation));
    if (!status.ok()) {
      return status;
    }

    auto ids = ListWalSegmentIDs(WalGenerationPath(dir, generation));
    if (!ids.ok()) {
      return ids.status();
    }
    if (ids.value().empty()) {
      ids.value().push_back(kFirstWalSegmentNo);
    }

    auto store = std::unique_ptr<SegmentedRecordStore>(new SegmentedRecordStore());
    store->dir_ = dir;
    store->wal_size_ = wal_size;
    store->generation_ = generation;
    store->active_file_no_ = ids.value().back();
    store->next_file_no_ = store->active_file_no_ + 1;
    store->snapshot_records_.push_back(OwnedRecord{});

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

  Result<minpatricia::Position> AddSnapshotRecord(minpatricia::ByteView key,
                                                  minpatricia::ByteView value) {
    if (snapshot_records_.size() >= kRecordOffsetLimit) {
      return CorruptSnapshot("too many snapshot records");
    }
    const minpatricia::Position pos =
        static_cast<minpatricia::Position>(snapshot_records_.size());
    snapshot_records_.push_back(OwnedRecord{minpatricia::CopyBytes(key),
                                            minpatricia::CopyBytes(value), true});
    return pos;
  }

  minpatricia::Result<minpatricia::ByteView> Key(minpatricia::Position pos) {
    if (IsSnapshotPosition(pos)) {
      const auto* record = SnapshotRecord(pos);
      if (record == nullptr) {
        return minpatricia::Status(minpatricia::StatusCode::kMissingKey);
      }
      return minpatricia::ByteView(record->key);
    }
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
    if (IsSnapshotPosition(pos)) {
      const auto* record = SnapshotRecord(pos);
      if (record == nullptr) {
        return Status(StatusCode::kCorruptIndex, "snapshot record not found");
      }
      return minpatricia::ByteView(record->value);
    }
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
    std::uint64_t last_kept_file_no = active_file_no_;
    for (std::uint64_t id : ids) {
      if (drop_remaining) {
        (void)::unlink(WalPath(id).c_str());
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
      last_kept_file_no = id;
      if (truncated) {
        drop_remaining = true;
      }
    }
    if (drop_remaining) {
      active_file_no_ = last_kept_file_no;
      next_file_no_ = active_file_no_ + 1;
      for (auto& entry : segments_) {
        entry.second->set_sealed(entry.first != active_file_no_);
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

  [[nodiscard]] std::uint64_t generation() const { return generation_; }
  [[nodiscard]] std::size_t SegmentCount() const { return segments_.size(); }

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

  Status ResetToSnapshot(std::uint64_t generation) {
    if (generation == 0) {
      return CorruptManifest("invalid WAL generation");
    }
    Status first = Close(false);
    if (!first.ok()) {
      return first;
    }
    Status status = CreateDirs(WalGenerationPath(dir_, generation));
    if (!status.ok()) {
      return status;
    }
    snapshot_records_.clear();
    snapshot_records_.push_back(OwnedRecord{});
    generation_ = generation;
    active_file_no_ = kFirstWalSegmentNo;
    next_file_no_ = kFirstWalSegmentNo + 1;
    auto segment = WalSegment::Open(WalPath(kFirstWalSegmentNo), wal_size_,
                                    kFirstWalSegmentNo);
    if (!segment.ok()) {
      return segment.status();
    }
    segment.value()->set_sealed(false);
    segments_.emplace(kFirstWalSegmentNo, segment.take_value());
    return OkStatus();
  }

 private:
  struct OwnedRecord {
    std::vector<std::byte> key;
    std::vector<std::byte> value;
    bool live = false;
  };

  SegmentedRecordStore() = default;

  static std::string WalGenerationPath(const std::string& dir, std::uint64_t generation) {
    return JoinPath(JoinPath(dir, kWalDirName), WalGenerationName(generation));
  }

  static bool IsSnapshotPosition(minpatricia::Position pos) {
    return pos > 0 && static_cast<std::uint64_t>(pos) < kRecordOffsetLimit;
  }

  const OwnedRecord* SnapshotRecord(minpatricia::Position pos) const {
    const std::uint64_t index = static_cast<std::uint64_t>(pos);
    if (index == 0 || index >= snapshot_records_.size()) {
      return nullptr;
    }
    const auto& record = snapshot_records_[static_cast<std::size_t>(index)];
    if (!record.live) {
      return nullptr;
    }
    return &record;
  }

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
    return JoinPath(WalGenerationPath(dir_, generation_), WalSegmentName(id));
  }

  std::string dir_;
  std::uint64_t wal_size_ = 0;
  std::uint64_t generation_ = kFirstWalGeneration;
  std::uint64_t active_file_no_ = 0;
  std::uint64_t next_file_no_ = 0;
  std::vector<OwnedRecord> snapshot_records_;
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

    auto manifest = ReadManifest(dir);
    if (!manifest.ok()) {
      return manifest.status();
    }

    auto records = SegmentedRecordStore::Open(dir, options.wal_size,
                                              manifest.value().wal_generation);
    if (!records.ok()) {
      return records.status();
    }
    auto nodes = std::unique_ptr<minpatricia::HeapNodeStore>(new minpatricia::HeapNodeStore());
    auto index = Index::NewWithNodes(*records.value(), *nodes);
    if (!index.ok()) {
      return FromMinpatriciaStatus(index.status());
    }

    if (manifest.value().valid) {
      auto snapshot = ReadSnapshot(dir, manifest.value().wal_generation);
      if (!snapshot.ok()) {
        return snapshot.status();
      }
      status = LoadSnapshotRecords(records.value().get(), &index.value(), snapshot.value());
      if (!status.ok()) {
        return status;
      }
    }

    status = records.value()->ReplayAll(options.wal_replay_policy, &index.value());
    if (!status.ok()) {
      return status;
    }
    if (manifest.value().valid) {
      status = options.runtime->BlockingIO("cleanup_old_checkpoints", [&] {
        Status wal = RemoveOldWalGenerations(dir, manifest.value().wal_generation);
        Status snapshot = RemoveOldSnapshots(dir, manifest.value().wal_generation);
        if (!wal.ok()) {
          return wal;
        }
        return snapshot;
      });
      if (!status.ok()) {
        return status;
      }
    }

    auto impl = std::unique_ptr<Impl>(new Impl());
    impl->runtime_ = options.runtime;
    impl->dir_ = dir;
    impl->wal_size_ = options.wal_size;
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
    const std::string dir = InMemoryTempPath();
    auto records = SegmentedRecordStore::Open(dir, options.wal_size, kFirstWalGeneration);
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
    impl->dir_ = dir;
    impl->wal_size_ = options.wal_size;
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
    Status checkpoint_status;
    {
      WriteLock lock(*primary_mu_);
      if (!open_) {
        return OkStatus();
      }
      if (sync_on_close_) {
        checkpoint_status = CheckpointLocked();
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
      Status close_status = records->Close(sync_on_close);
      if (!checkpoint_status.ok()) {
        return checkpoint_status;
      }
      return close_status;
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
    return CheckpointIfNeededLocked();
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
    auto deleted = index_->Delete(key);
    if (!deleted.ok()) {
      return MarkFatal(FromMinpatriciaStatus(deleted.status()));
    }
    if (!deleted.value().deleted || deleted.value().pos != pos.value().pos) {
      return MarkFatal(Status(StatusCode::kCorruptIndex, "delete position mismatch"));
    }
    auto tombstone = records_->DeleteRecord(key);
    if (!tombstone.ok()) {
      auto restore = index_->Put(key, pos.value().pos);
      if (!restore.ok()) {
        return MarkFatal(FromMinpatriciaStatus(restore.status()));
      }
      return tombstone.status();
    }
    status = CheckpointIfNeededLocked();
    if (!status.ok()) {
      return status;
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

  static Status LoadSnapshotRecords(SegmentedRecordStore* records, Index* index,
                                    const std::vector<Item>& entries) {
    for (const Item& item : entries) {
      auto pos = records->AddSnapshotRecord(minpatricia::AsBytes(item.key),
                                            minpatricia::AsBytes(item.value));
      if (!pos.ok()) {
        return pos.status();
      }
      auto put = index->Put(minpatricia::AsBytes(item.key), pos.value());
      if (!put.ok()) {
        return FromMinpatriciaStatus(put.status());
      }
    }
    return OkStatus();
  }

  Result<std::vector<Item>> CollectLiveItemsLocked() {
    std::vector<Item> entries;
    entries.reserve(static_cast<std::size_t>(index_->Len()));
    Status visit_status;
    auto status = index_->Ascend([&](minpatricia::ByteView key,
                                     minpatricia::Position pos) {
      auto item = MakeItem(key, pos);
      if (!item.ok()) {
        visit_status = item.status();
        return false;
      }
      entries.push_back(item.take_value());
      return true;
    });
    if (!visit_status.ok()) {
      return visit_status;
    }
    if (!status.ok()) {
      return FromMinpatriciaStatus(status);
    }
    return entries;
  }

  Status RebuildIndexFromSnapshotLocked(const std::vector<Item>& entries) {
    auto nodes = std::unique_ptr<minpatricia::HeapNodeStore>(new minpatricia::HeapNodeStore());
    auto index = Index::NewWithNodes(*records_, *nodes);
    if (!index.ok()) {
      return FromMinpatriciaStatus(index.status());
    }
    Status status = LoadSnapshotRecords(records_.get(), &index.value(), entries);
    if (!status.ok()) {
      return status;
    }
    nodes_ = std::move(nodes);
    index_.reset(new Index(index.take_value()));
    return OkStatus();
  }

  Status CheckpointIfNeededLocked() {
    if (records_->SegmentCount() <= kCheckpointWalSegmentThreshold) {
      return OkStatus();
    }
    return CheckpointLocked();
  }

  Status CheckpointLocked() {
    auto entries = CollectLiveItemsLocked();
    if (!entries.ok()) {
      return entries.status();
    }
    const std::uint64_t next_generation = records_->generation() + 1;
    Status status = runtime_->BlockingIO("write_checkpoint", [&] {
      Status snapshot = WriteSnapshot(dir_, next_generation, entries.value());
      if (!snapshot.ok()) {
        return snapshot;
      }
      Status dirs = CreateDirs(JoinPath(JoinPath(dir_, kWalDirName),
                                        WalGenerationName(next_generation)));
      if (!dirs.ok()) {
        return dirs;
      }
      Status manifest = WriteManifest(dir_, next_generation);
      if (!manifest.ok()) {
        return manifest;
      }
      return OkStatus();
    });
    if (!status.ok()) {
      return status;
    }
    status = records_->ResetToSnapshot(next_generation);
    if (!status.ok()) {
      return status;
    }
    status = RebuildIndexFromSnapshotLocked(entries.value());
    if (!status.ok()) {
      return status;
    }
    return runtime_->BlockingIO("cleanup_checkpoint", [&] {
      Status wal = RemoveOldWalGenerations(dir_, next_generation);
      Status snapshot = RemoveOldSnapshots(dir_, next_generation);
      if (!wal.ok()) {
        return wal;
      }
      return snapshot;
    });
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
  std::string dir_;
  std::uint64_t wal_size_ = 0;
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
