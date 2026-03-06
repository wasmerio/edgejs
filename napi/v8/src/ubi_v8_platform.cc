#include "ubi_v8_platform.h"

#include <cmath>
#include <cstdint>
#include <memory>
#include <utility>

#include <libplatform/libplatform.h>
#include <uv.h>

#include "internal/unofficial_napi_bridge.h"

namespace {

struct ForegroundTaskRecord {
  std::unique_ptr<v8::Task> task;
};

void RunForegroundTaskRecord(napi_env /*env*/, void* data) {
  auto* record = static_cast<ForegroundTaskRecord*>(data);
  if (record != nullptr && record->task) {
    record->task->Run();
  }
}

void CleanupForegroundTaskRecord(napi_env /*env*/, void* data) {
  delete static_cast<ForegroundTaskRecord*>(data);
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
  void PostTaskCommon(std::unique_ptr<v8::Task> task,
                      uint64_t delay_ms,
                      const v8::SourceLocation& location) {
    if (!task) return;

    napi_env env = nullptr;
    unofficial_napi_enqueue_foreground_task_callback enqueue = nullptr;
    if (NapiV8LookupForegroundTaskTarget(isolate_, &env, &enqueue) &&
        enqueue != nullptr &&
        env != nullptr) {
      auto* record = new (std::nothrow) ForegroundTaskRecord();
      if (record != nullptr) {
        record->task = std::move(task);
        if (enqueue(env,
                    RunForegroundTaskRecord,
                    record,
                    CleanupForegroundTaskRecord,
                    delay_ms) == napi_ok) {
          return;
        }
        delete record;
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
  return fallback_ != nullptr ? fallback_->MonotonicallyIncreasingTime() : (uv_hrtime() / 1e9);
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
