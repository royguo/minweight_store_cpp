#pragma once

#include <utility>

namespace minpatricia {

enum class StatusCode {
  kOk = 0,
  kEqualKeys,
  kKeyTooLarge,
  kUnsortedKeys,
  kMissingKey,
  kPositionTag,
  kPositionMismatch,
  kDuplicateKey,
  kCorruptLayout,
  kNilRecordStore,
  kNilNodeStore,
};

class Status {
 public:
  constexpr Status() = default;
  constexpr explicit Status(StatusCode code) : code_(code) {}

  [[nodiscard]] constexpr bool ok() const { return code_ == StatusCode::kOk; }
  [[nodiscard]] constexpr StatusCode code() const { return code_; }

  [[nodiscard]] constexpr bool operator==(Status other) const {
    return code_ == other.code_;
  }
  [[nodiscard]] constexpr bool operator!=(Status other) const {
    return !(*this == other);
  }

  [[nodiscard]] constexpr bool operator==(StatusCode code) const {
    return code_ == code;
  }
  [[nodiscard]] constexpr bool operator!=(StatusCode code) const {
    return code_ != code;
  }

 private:
  StatusCode code_ = StatusCode::kOk;
};

inline constexpr Status OkStatus() { return Status{}; }

template <class T>
class Result {
 public:
  constexpr Result() = default;
  constexpr Result(Status status) : status_(status) {}
  constexpr Result(T value) : status_(OkStatus()), value_(std::move(value)) {}
  constexpr Result(Status status, T value)
      : status_(status), value_(std::move(value)) {}

  [[nodiscard]] constexpr bool ok() const { return status_.ok(); }
  [[nodiscard]] constexpr Status status() const { return status_; }

  [[nodiscard]] constexpr const T& value() const { return value_; }
  [[nodiscard]] constexpr T& value() { return value_; }
  [[nodiscard]] constexpr T&& take_value() { return std::move(value_); }

 private:
  Status status_{StatusCode::kCorruptLayout};
  T value_{};
};

}  // namespace minpatricia
