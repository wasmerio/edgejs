#include <csignal>
#include <cstring>

#if !defined(_WIN32)

#include "edge_node_addon_compat.h"

namespace node {

namespace {

constexpr unsigned kMaxSignal = 32;

}  // namespace

__attribute__((visibility("default"))) void RegisterSignalHandler(
    int signal,
    void (*handler)(int signal, siginfo_t* info, void* ucontext),
    bool reset_handler) {
  if (handler == nullptr) return;

  struct sigaction sa;
  std::memset(&sa, 0, sizeof(sa));
  sa.sa_sigaction = handler;
  sa.sa_flags = reset_handler ? SA_RESETHAND : 0;
  sigfillset(&sa.sa_mask);
  (void)sigaction(signal, &sa, nullptr);
}

__attribute__((visibility("default"))) void ResetSignalHandlers() {
  struct sigaction act;
  std::memset(&act, 0, sizeof(act));

  for (unsigned nr = 1; nr < kMaxSignal; nr += 1) {
    if (nr == SIGKILL || nr == SIGSTOP) continue;

    bool ignore_signal = false;
#if defined(SIGPIPE)
    ignore_signal = ignore_signal || nr == SIGPIPE;
#endif
#if defined(SIGXFSZ)
    ignore_signal = ignore_signal || nr == SIGXFSZ;
#endif
    act.sa_handler = ignore_signal ? SIG_IGN : SIG_DFL;

    if (act.sa_handler == SIG_DFL) {
      struct sigaction old;
      if (sigaction(static_cast<int>(nr), nullptr, &old) != 0) continue;
#if defined(SA_SIGINFO)
      if ((old.sa_flags & SA_SIGINFO) || old.sa_handler != SIG_IGN) continue;
#else
      if (old.sa_handler != SIG_IGN) continue;
#endif
    }

    (void)sigaction(static_cast<int>(nr), &act, nullptr);
  }
}

}  // namespace node

extern "C" __attribute__((visibility("default"))) void node_module_register(void* /*mod*/) {}

#endif  // !defined(_WIN32)
