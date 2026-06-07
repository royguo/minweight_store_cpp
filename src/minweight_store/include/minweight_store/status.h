#pragma once

#include <string>
#include <utility>

namespace minweight_store {

enum class StatusCode {
  kOk = 0,
  kInvalidArgument,
  kClosed,
  kWalFull,
  kCorruptWal,
  kCorruptIndex,
  kIoError,
  kRuntimeError,
  kFatal,
};

class Status {
 public:
  Status() = default;
  explicit Status(StatusCode code) : code_(code) {}
  Status(StatusCode code, std::string message)
      : code_(code), message_(std::move(message)) {}

  [[nodiscard]] bool ok() const { return code_ == StatusCode::kOk; }
  [[nodiscard]] StatusCode code() const { return code_; }
  [[nodiscard]] const std::string& message() const { return message_; }

  [[nodiscard]] bool operator==(StatusCode code) const { return code_ == code; }
  [[nodiscard]] bool operator!=(StatusCode code) const { return code_ != code; }

 private:
  StatusCode code_ = StatusCode::kOk;
  std::string message_;
};

inline Status OkStatus() { return Status{}; }

template <class T>
class Result {
 public:
  Result() = default;
  Result(Status status) : status_(std::move(status)) {}
  Result(T value) : status_(OkStatus()), value_(std::move(value)) {}
  Result(Status status, T value) : status_(std::move(status)), value_(std::move(value)) {}

  [[nodiscard]] bool ok() const { return status_.ok(); }
  [[nodiscard]] const Status& status() const { return status_; }

  [[nodiscard]] const T& value() const { return value_; }
  [[nodiscard]] T& value() { return value_; }
  [[nodiscard]] T&& take_value() { return std::move(value_); }

 private:
  Status status_{StatusCode::kFatal};
  T value_{};
};

}  // namespace minweight_store
