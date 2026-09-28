// SPDX-License-Identifier: MIT
// Adapted from wasix-libc's musl waitpid implementation. The WASIX 0.4.3
// sysroot returns a child PID for JOIN_STATUS_NOTHING, which makes libuv
// report an exit before the child has run. Remove this compatibility object
// once the corrected wasix-libc waitpid is included in the WASIX sysroot.

#include <errno.h>
#include <stdlib.h>
#include <unistd.h>
#include <wasi/api.h>

pid_t waitpid(pid_t pid, int *status, int options) {
  __wasi_join_flags_t flags = 0;
  if ((options & WNOHANG) != 0) flags |= __WASI_JOIN_FLAGS_NON_BLOCKING;
  if ((options & WUNTRACED) != 0) flags |= __WASI_JOIN_FLAGS_WAKE_STOPPED;

  __wasi_option_pid_t opid;
  if (pid == -1) {
    opid.tag = __WASI_OPTION_NONE;
  } else {
    opid.tag = __WASI_OPTION_SOME;
    opid.u.some = abs(pid);
  }

  __wasi_join_status_t code;
  int result = __wasi_proc_join(&opid, flags, &code);
  if (result != 0) {
    errno = result;
    return -1;
  }

  // A nonblocking poll of a live child has no exit status to reap.
  if (code.tag == __WASI_JOIN_STATUS_TYPE_NOTHING) {
    if ((options & WNOHANG) != 0) return 0;
    errno = ECHILD;
    return -1;
  }

  if (opid.tag != __WASI_OPTION_SOME) {
    errno = ECHILD;
    return -1;
  }
  pid = opid.u.some;

  if (code.tag == __WASI_JOIN_STATUS_TYPE_EXIT_NORMAL) {
    if (status) *status = W_EXITCODE(code.u.exit_normal, 0);
  } else if (code.tag == __WASI_JOIN_STATUS_TYPE_EXIT_SIGNAL) {
    if (status) {
      *status = W_EXITCODE(code.u.exit_signal.exit_code,
                           code.u.exit_signal.signal);
    }
  } else if (code.tag == __WASI_JOIN_STATUS_TYPE_STOPPED) {
    if (status) *status = W_STOPCODE(code.u.stopped);
  } else {
    errno = EUNKNOWN;
    return -1;
  }
  return pid;
}
