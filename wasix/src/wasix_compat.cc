#include <errno.h>
#include <cstdlib>
#include <grp.h>
#include <stdint.h>

int getgroups(int size, gid_t* list) {
  if (size < 0) {
    errno = EINVAL;
    return -1;
  }
  (void)list;
  return 0;
}

extern "C" uint64_t uv_get_available_memory(void) {
  return 0;
}

extern "C" uint64_t uv_get_constrained_memory(void) {
  return 0;
}

extern "C" __attribute__((used)) uint32_t ubi_guest_malloc(uint32_t size) {
  void* ptr = std::malloc(static_cast<size_t>(size));
  return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(ptr));
}
