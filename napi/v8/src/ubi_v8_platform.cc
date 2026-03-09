#include "ubi_v8_platform.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

#include <libplatform/libplatform.h>

namespace {

struct ForegroundTaskRecord {
  void* recycler = nullptr;
  void (*recycle)(void* recycler, ForegroundTaskRecord* record) = nullptr;
  std::unique_ptr<v8::Task> task;
};

void RunForegroundTaskRecord(napi_env /*env*/, void* data) {
  auto* record = static_cast<ForegroundTaskRecord*>(data);
  if (record != nullptr && record->task) {
    record->task->Run();
  }
}

void CleanupForegroundTaskRecord(napi_env /*env*/, void* data) {
  auto* record = static_cast<ForegroundTaskRecord*>(data);
  if (record == nullptr || record->recycle == nullptr) {
    delete record;
    return;
  }
  record->task.reset();
  record->recycle(record->recycler, record);
}

}  // namespace

class UbiV8Platform::ForegroundTaskRunner final : public v8::TaskRunner {
 public:
  ForegroundTaskRunner(v8::Isolate* isolate, v8::Platform* fallback)
      : isolate_(isolate), fallback_(fallback) {}

  bool IdleTasksEnabled() override { return false; }
  bool NonNestableTasksEnabled() const override { return true; }
  bool NonNestableDelayedTasksEnabled() const override { return true; }

 protected:
  void PostTaskImpl(std::unique_ptr<v8::Task> task,
                    const v8::SourceLocation& location) override {
    PostTaskCommon(std::move(task), 0, location);
  }

  void PostNonNestableTaskImpl(std::unique_ptr<v8::Task> task,
                               const v8::SourceLocation& location) override {
    PostTaskCommon(std::move(task), 0, location);
  }

  void PostDelayedTaskImpl(std::unique_ptr<v8::Task> task,
                           double delay_in_seconds,
                           const v8::SourceLocation& location) override {
    uint64_t delay_ms = 0;
    if (delay_in_seconds > 0) {
      delay_ms = static_cast<uint64_t>(std::llround(delay_in_seconds * 1000.0));
    }
    PostTaskCommon(std::move(task), delay_ms, location);
  }

  void PostNonNestableDelayedTaskImpl(
      std::unique_ptr<v8::Task> task,
      double delay_in_seconds,
      const v8::SourceLocation& location) override {
    PostDelayedTaskImpl(std::move(task), delay_in_seconds, location);
  }

  void PostIdleTaskImpl(std::unique_ptr<v8::IdleTask> /*task*/,
                        const v8::SourceLocation& /*location*/) override {}

 private:
  ForegroundTaskRecord* AcquireRecord() {
    std::lock_guard<std::mutex> lock(record_mutex_);
    if (!record_pool_.empty()) {
      ForegroundTaskRecord* record = record_pool_.back();
      record_pool_.pop_back();
      return record;
    }
    return new (std::nothrow) ForegroundTaskRecord();
  }

  void PostTaskCommon(std::unique_ptr<v8::Task> task,
                      uint64_t delay_ms,
                      const v8::SourceLocation& location) {
    if (!task) return;

    unofficial_napi_enqueue_foreground_task_callback enqueue =
        target_enqueue_.load(std::memory_order_acquire);
    void* target = target_data_.load(std::memory_order_acquire);
    if (enqueue != nullptr && target != nullptr) {
      ForegroundTaskRecord* record = AcquireRecord();
      if (record != nullptr) {
        record->recycler = this;
        record->recycle = [](void* recycler, ForegroundTaskRecord* record) {
          static_cast<ForegroundTaskRunner*>(recycler)->RecycleRecord(record);
        };
        record->task = std::move(task);
        if (enqueue(target,
                    RunForegroundTaskRecord,
                    record,
                    CleanupForegroundTaskRecord,
                    delay_ms) == napi_ok) {
          return;
        }
        task = std::move(record->task);
        RecycleRecord(record);
      }
    }

    auto runner = fallback_ != nullptr ? fallback_->GetForegroundTaskRunner(isolate_) : nullptr;
    if (runner) {
      if (delay_ms == 0) {
        runner->PostTask(std::move(task), location);
      } else {
        runner->PostDelayedTask(std::move(task), delay_ms / 1000.0, location);
      }
    }
  }

  v8::Isolate* isolate_ = nullptr;
  v8::Platform* fallback_ = nullptr;
  std::atomic<napi_env> target_env_ {nullptr};
  std::atomic<unofficial_napi_enqueue_foreground_task_callback> target_enqueue_ {nullptr};
  std::atomic<void*> target_data_ {nullptr};
  std::mutex record_mutex_;
  std::vector<ForegroundTaskRecord*> record_pool_;

 public:
  ~ForegroundTaskRunner() override {
    for (ForegroundTaskRecord* record : record_pool_) {
      delete record;
    }
  }

  void RecycleRecord(ForegroundTaskRecord* record) {
    if (record == nullptr) return;
    std::lock_guard<std::mutex> lock(record_mutex_);
    record_pool_.push_back(record);
  }

  void BindTarget(napi_env env,
                  unofficial_napi_enqueue_foreground_task_callback callback,
                  void* target) {
    target_data_.store(target, std::memory_order_release);
    target_enqueue_.store(callback, std::memory_order_release);
    target_env_.store(env, std::memory_order_release);
  }

  void ClearTarget(napi_env env) {
    if (target_env_.load(std::memory_order_acquire) != env) return;
    target_env_.store(nullptr, std::memory_order_release);
    target_enqueue_.store(nullptr, std::memory_order_release);
    target_data_.store(nullptr, std::memory_order_release);
  }
};

std::unique_ptr<UbiV8Platform> UbiV8Platform::Create() {
  std::unique_ptr<v8::Platform> fallback = v8::platform::NewDefaultPlatform();
  if (!fallback) return nullptr;
  return std::unique_ptr<UbiV8Platform>(new UbiV8Platform(std::move(fallback)));
}

UbiV8Platform::UbiV8Platform(std::unique_ptr<v8::Platform> fallback)
    : fallback_(std::move(fallback)) {}

UbiV8Platform::~UbiV8Platform() = default;

std::shared_ptr<UbiV8Platform::ForegroundTaskRunner> UbiV8Platform::EnsureRunner(v8::Isolate* isolate) {
  if (isolate == nullptr) return nullptr;
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = runners_.find(isolate);
  if (it != runners_.end()) return it->second;
  auto runner = std::make_shared<ForegroundTaskRunner>(isolate, fallback_.get());
  runners_.emplace(isolate, runner);
  return runner;
}

bool UbiV8Platform::RegisterIsolate(v8::Isolate* isolate) { return EnsureRunner(isolate) != nullptr; }

void UbiV8Platform::UnregisterIsolate(v8::Isolate* isolate) {
  if (isolate == nullptr) return;
  std::lock_guard<std::mutex> lock(mutex_);
  runners_.erase(isolate);
}

bool UbiV8Platform::BindForegroundTaskTarget(
    v8::Isolate* isolate,
    napi_env env,
    unofficial_napi_enqueue_foreground_task_callback callback,
    void* target) {
  std::shared_ptr<ForegroundTaskRunner> runner = EnsureRunner(isolate);
  if (!runner) return false;
  runner->BindTarget(env, callback, target);
  return true;
}

void UbiV8Platform::ClearForegroundTaskTarget(v8::Isolate* isolate, napi_env env) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = runners_.find(isolate);
  if (it == runners_.end() || !it->second) return;
  it->second->ClearTarget(env);
}

int UbiV8Platform::NumberOfWorkerThreads() {
  return fallback_ != nullptr ? fallback_->NumberOfWorkerThreads() : 0;
}

std::shared_ptr<v8::TaskRunner> UbiV8Platform::GetForegroundTaskRunner(
    v8::Isolate* isolate,
    v8::TaskPriority /*priority*/) {
  return EnsureRunner(isolate);
}

bool UbiV8Platform::IdleTasksEnabled(v8::Isolate* isolate) {
  (void)isolate;
  return false;
}

double UbiV8Platform::MonotonicallyIncreasingTime() {
  if (fallback_ != nullptr) return fallback_->MonotonicallyIncreasingTime();
  using clock = std::chrono::steady_clock;
  const auto now = clock::now().time_since_epoch();
  return std::chrono::duration<double>(now).count();
}

double UbiV8Platform::CurrentClockTimeMillis() {
  return fallback_ != nullptr ? fallback_->CurrentClockTimeMillis()
                              : v8::Platform::SystemClockTimeMillis();
}

v8::TracingController* UbiV8Platform::GetTracingController() {
  return fallback_ != nullptr ? fallback_->GetTracingController() : nullptr;
}

v8::PageAllocator* UbiV8Platform::GetPageAllocator() {
  return fallback_ != nullptr ? fallback_->GetPageAllocator() : nullptr;
}

v8::ThreadIsolatedAllocator* UbiV8Platform::GetThreadIsolatedAllocator() {
  return fallback_ != nullptr ? fallback_->GetThreadIsolatedAllocator() : nullptr;
}

void UbiV8Platform::OnCriticalMemoryPressure() {
  if (fallback_ != nullptr) fallback_->OnCriticalMemoryPressure();
}

void UbiV8Platform::DumpWithoutCrashing() {
  if (fallback_ != nullptr) fallback_->DumpWithoutCrashing();
}

v8::HighAllocationThroughputObserver* UbiV8Platform::GetHighAllocationThroughputObserver() {
  if (fallback_ != nullptr) return fallback_->GetHighAllocationThroughputObserver();
  static v8::HighAllocationThroughputObserver observer;
  return &observer;
}

v8::Platform::StackTracePrinter UbiV8Platform::GetStackTracePrinter() {
  return fallback_ != nullptr ? fallback_->GetStackTracePrinter() : nullptr;
}

std::unique_ptr<v8::ScopedBlockingCall> UbiV8Platform::CreateBlockingScope(
    v8::BlockingType blocking_type) {
  return fallback_ != nullptr ? fallback_->CreateBlockingScope(blocking_type) : nullptr;
}

std::unique_ptr<v8::JobHandle> UbiV8Platform::CreateJobImpl(
    v8::TaskPriority priority,
    std::unique_ptr<v8::JobTask> job_task,
    const v8::SourceLocation& location) {
  (void)location;
  return v8::platform::NewDefaultJobHandle(this, priority, std::move(job_task),
                                           static_cast<size_t>(std::max(1, NumberOfWorkerThreads())));
}

void UbiV8Platform::PostTaskOnWorkerThreadImpl(v8::TaskPriority priority,
                                               std::unique_ptr<v8::Task> task,
                                               const v8::SourceLocation& location) {
  if (fallback_ != nullptr) {
    fallback_->PostTaskOnWorkerThread(priority, std::move(task), location);
  }
}

void UbiV8Platform::PostDelayedTaskOnWorkerThreadImpl(
    v8::TaskPriority priority,
    std::unique_ptr<v8::Task> task,
    double delay_in_seconds,
    const v8::SourceLocation& location) {
  if (fallback_ != nullptr) {
    fallback_->PostDelayedTaskOnWorkerThread(priority, std::move(task), delay_in_seconds, location);
  }
}
