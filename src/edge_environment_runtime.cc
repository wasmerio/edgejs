#include "edge_environment_runtime.h"

#include "edge_cares_wrap.h"
#include "edge_runtime_platform.h"
#include "edge_timers_host.h"
#include "edge_worker_control.h"

namespace {

constexpr int kCleanupStopWorkers = 10;
constexpr int kCleanupTimers = 20;
constexpr int kCleanupRuntimePlatform = 30;
constexpr int kCleanupCares = 35;
void StopWorkersCleanup(napi_env env, void* /*arg*/) {
  EdgeWorkerStopAllForEnv(env);
}

void TimersCleanup(napi_env env, void* /*arg*/) {
  EdgeRunTimersHostEnvCleanup(env);
}

void RuntimePlatformCleanup(napi_env env, void* /*arg*/) {
  EdgeRunRuntimePlatformEnvCleanup(env);
}

void CaresCleanup(napi_env env, void* /*arg*/) {
  EdgeRunCaresWrapEnvCleanup(env);
}

}  // namespace

bool EdgeAttachEnvironmentForRuntime(napi_env env, const EdgeEnvironmentConfig* config) {
  if (!EdgeEnvironmentAttach(env, config)) return false;

  EdgeEnvironmentRegisterCleanupStage(env, StopWorkersCleanup, nullptr, kCleanupStopWorkers);
  EdgeEnvironmentRegisterCleanupStage(env, TimersCleanup, nullptr, kCleanupTimers);
  EdgeEnvironmentRegisterCleanupStage(
      env, RuntimePlatformCleanup, nullptr, kCleanupRuntimePlatform);
  EdgeEnvironmentRegisterCleanupStage(env, CaresCleanup, nullptr, kCleanupCares);
  return true;
}
