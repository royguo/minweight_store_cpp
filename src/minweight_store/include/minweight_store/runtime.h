#pragma once

#include <functional>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string_view>

#include "minweight_store/status.h"

namespace minweight_store {

class Mutex {
 public:
  virtual ~Mutex() = default;
  virtual void Lock() = 0;
  virtual void Unlock() = 0;
};

class RWMutex {
 public:
  virtual ~RWMutex() = default;
  virtual void Lock() = 0;
  virtual void Unlock() = 0;
  virtual void RLock() = 0;
  virtual void RUnlock() = 0;
};

class Runtime {
 public:
  using Task = std::function<Status()>;

  virtual ~Runtime() = default;
  virtual std::unique_ptr<Mutex> NewMutex() = 0;
  virtual std::unique_ptr<RWMutex> NewRWMutex() = 0;
  virtual Status BlockingIO(std::string_view name, Task task) = 0;
};

class MutexLock {
 public:
  explicit MutexLock(Mutex& mu) : mu_(mu) { mu_.Lock(); }
  ~MutexLock() { mu_.Unlock(); }

  MutexLock(const MutexLock&) = delete;
  MutexLock& operator=(const MutexLock&) = delete;

 private:
  Mutex& mu_;
};

class WriteLock {
 public:
  explicit WriteLock(RWMutex& mu) : mu_(mu) { mu_.Lock(); }
  ~WriteLock() { mu_.Unlock(); }

  WriteLock(const WriteLock&) = delete;
  WriteLock& operator=(const WriteLock&) = delete;

 private:
  RWMutex& mu_;
};

class ReadLock {
 public:
  explicit ReadLock(RWMutex& mu) : mu_(mu) { mu_.RLock(); }
  ~ReadLock() { mu_.RUnlock(); }

  ReadLock(const ReadLock&) = delete;
  ReadLock& operator=(const ReadLock&) = delete;

 private:
  RWMutex& mu_;
};

class StdRuntime : public Runtime {
 public:
  static std::shared_ptr<StdRuntime> Shared();

  std::unique_ptr<Mutex> NewMutex() override;
  std::unique_ptr<RWMutex> NewRWMutex() override;
  Status BlockingIO(std::string_view name, Task task) override;
};

}  // namespace minweight_store
