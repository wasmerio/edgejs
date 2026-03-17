#include <cstdint>
#include <limits>

extern "C" {

// Bun's prebuilt WebKit archive expects these host hooks to exist, but the
// standalone JSC package does not ship the Bun runtime that normally provides
// them. Weak definitions let a real Bun host override them when present while
// still making the static SDK usable for edgejs.
__attribute__((weak)) void Bun__errorInstance__finalize(void*) {}

__attribute__((weak)) void Bun__reportUnhandledError(void*, std::uint64_t) {}

// Bun's RunLoopBun.cpp routes through these timer hooks. Returning a null timer
// cleanly disables the Bun-specific event-loop integration for this embedder.
__attribute__((weak)) void* WTFTimer__create(void*) { return nullptr; }

__attribute__((weak)) void WTFTimer__update(void*, double, bool) {}

__attribute__((weak)) void WTFTimer__deinit(void*) {}

__attribute__((weak)) bool WTFTimer__isActive(const void*) { return false; }

__attribute__((weak)) void WTFTimer__cancel(void*) {}

__attribute__((weak)) double WTFTimer__secondsUntilTimer(void*) {
  return std::numeric_limits<double>::infinity();
}

}  // extern "C"
