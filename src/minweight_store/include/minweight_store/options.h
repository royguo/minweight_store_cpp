#pragma once

#include <cstdint>
#include <memory>

#include "minweight_store/runtime.h"

namespace minweight_store {

enum class WALReplayPolicy : std::uint8_t {
  // Replays the CRC-valid prefix and truncates the WAL at the first corrupt record.
  kPointInTime = 0,
  // Fails Open when any record in the advertised WAL range is corrupt.
  kStrict = 1,
};

struct Options {
  std::uint64_t wal_size = 128ULL * 1024ULL * 1024ULL;
  WALReplayPolicy wal_replay_policy = WALReplayPolicy::kPointInTime;
  bool verify_index_on_read = false;
  bool sync_on_close = true;
  std::shared_ptr<Runtime> runtime;
};

}  // namespace minweight_store
