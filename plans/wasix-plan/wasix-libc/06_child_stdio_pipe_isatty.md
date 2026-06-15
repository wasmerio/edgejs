# wasix-libc: child stdio pipe `isatty()` classification

Why this is a problem:

`uv_guess_handle(fd)` checks `isatty(fd)` before it falls back to `fstat()`.
That is correct on native Unix: a pipe-backed child stdio fd returns false from
`isatty()`, then `fstat()` exposes a FIFO/socket-like fd and libuv classifies it
as `UV_NAMED_PIPE`.

On WASIX, `wasix-libc` currently implements `isatty()` from WASI fd metadata:
`CHARACTER_DEVICE` plus no seek/tell rights means TTY. That heuristic can be
wrong for child stdio fds that are pipe endpoints. If the runtime exposes a
spawned child stderr/stdout pipe as a non-seekable character device, `isatty()`
returns true and `uv_guess_handle()` stops early with `UV_TTY`. Node then uses
the TTY path for a fd that should behave like a pipe.

This is not an EdgeJS behavior difference. Native EdgeJS passes because native
Linux/macOS kernels distinguish a terminal from a pipe at the fd level:

```text
pipe-backed fd -> isatty(fd) == 0 -> fstat(fd) says FIFO/socket -> UV_NAMED_PIPE
real terminal  -> isatty(fd) == 1 -> UV_TTY
```

The WASIX failure is that the lower layers do not preserve that distinction for
some child stdio pipe fds.

When it occurs:

- spawned children with stdout/stderr connected to parent-created pipes;
- Node/libuv code that calls `uv_guess_handle(0|1|2)` during bootstrap;
- stream tests such as `sequential/test-stream2-stderr-sync` where stderr must
  be accepted as a pipe-like stream.

Minimal Example:

This C program spawns itself with the child's `stderr` duplicated from the write
end of a pipe. In the child, `isatty(STDERR_FILENO)` must be false. The parent
reads the child result from the pipe.

```c
#include <errno.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

static int child(void) {
  errno = 0;
  int tty = isatty(STDERR_FILENO);
  int saved_errno = errno;

  struct stat st;
  memset(&st, 0, sizeof(st));
  int stat_rc = fstat(STDERR_FILENO, &st);

  dprintf(STDERR_FILENO,
          "isatty=%d errno=%d stat_rc=%d fifo=%d chr=%d\n",
          tty,
          saved_errno,
          stat_rc,
          stat_rc == 0 && S_ISFIFO(st.st_mode),
          stat_rc == 0 && S_ISCHR(st.st_mode));

  return tty == 0 ? 0 : 1;
}

int main(int argc, char **argv) {
  if (argc == 2 && strcmp(argv[1], "--child") == 0)
    return child();

  int pipefd[2];
  if (pipe(pipefd) != 0) {
    perror("pipe");
    return 1;
  }

  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_adddup2(&actions, pipefd[1], STDERR_FILENO);
  posix_spawn_file_actions_addclose(&actions, pipefd[0]);
  posix_spawn_file_actions_addclose(&actions, pipefd[1]);

  char *child_argv[] = { argv[0], "--child", NULL };
  pid_t pid;
  int rc = posix_spawnp(&pid, argv[0], &actions, NULL, child_argv, environ);
  posix_spawn_file_actions_destroy(&actions);
  close(pipefd[1]);

  if (rc != 0) {
    errno = rc;
    perror("posix_spawnp");
    close(pipefd[0]);
    return 1;
  }

  char buf[256];
  ssize_t nread = read(pipefd[0], buf, sizeof(buf) - 1);
  if (nread < 0) {
    perror("read");
    close(pipefd[0]);
    return 1;
  }
  buf[nread] = '\0';
  close(pipefd[0]);

  int status = 0;
  waitpid(pid, &status, 0);

  printf("child said: %s", buf);
  return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}
```

Expected output shape on a POSIX-compatible runtime:

```text
child said: isatty=0 errno=25 stat_rc=0 fifo=1 chr=0
```

The exact `errno` after `isatty()` may vary, but the important part is
`isatty=0` for the pipe-backed child `stderr` fd.

Callgraph and boundary:

Current problematic path:

```text
Node bootstrap / stream setup
  -> EdgeJS guessHandleType(fd)
  -> uv_guess_handle(fd)
  -> isatty(fd)
  -> wasix-libc __isatty(fd)
     -> __wasi_fd_fdstat_get(fd)
     -> CHARACTER_DEVICE + no seek/tell rights => true
        HERE IS THE PROBLEM: a child stdio pipe can be classified as TTY before
        libuv reaches fstat() and pipe/socket detection.
  -> uv_guess_handle() returns UV_TTY
  -> Node opens stderr/stdout through TTY behavior instead of pipe behavior
```

On native Linux, the problematic branch is not taken for a pipe:

```text
uv_guess_handle(fd)
  -> isatty(pipe_fd) == false
  -> fstat(pipe_fd) says FIFO
  -> UV_NAMED_PIPE
```

Proposed solution:

Fix this below EdgeJS. EdgeJS should not need to rewrite `UV_TTY` into
`UV_NAMED_PIPE` for stdio fds.

Preferred runtime/libc behavior:

- Wasmer should preserve fd kind for child stdio pipe endpoints created by
  `posix_spawn` / WASIX process file actions. A pipe endpoint must not be
  exposed to libc as an indistinguishable TTY-like character device.
- `wasix-libc` `isatty(fd)` should become fd-specific rather than global or
  heuristic-only. It should return true only when the runtime confirms that the
  specific fd is a terminal.
- `wasix-libc` should reject pipe-like fds before the old
  `CHARACTER_DEVICE && !SEEK && !TELL` fallback. If `fstat(fd)` can identify a
  pipe/FIFO, `isatty(fd)` must return false.

Possible implementation shape:

```text
wasix-libc isatty(fd)
  -> ask runtime whether this exact fd is a tty
     -> true: return 1
     -> false for pipe/socket/file: errno = ENOTTY; return 0
  -> only use legacy fdstat heuristic if no fd-specific answer exists
```

If the existing `tty_get` ABI remains global and fd-less, add a fd-aware WASIX
query or expose enough fd metadata through `fdstat` / `filestat` for libc to
separate:

```text
real tty fd
child stdio pipe fd
regular file fd
socket fd
```

Proposed callgraph after the fix:

```text
EdgeJS guessHandleType(fd)
  -> uv_guess_handle(fd)
  -> isatty(child_stderr_pipe_fd) == false
  -> fstat(child_stderr_pipe_fd) identifies pipe-like fd
  -> uv_guess_handle() returns UV_NAMED_PIPE
  -> Node stream bootstrap accepts stderr/stdout as pipe-like streams
```

Relevant code paths:

```text
~/src/edgejs/deps/libuv-wasix/src/unix/tty.c
  uv_guess_handle() order: isatty() first, fstat() second

~/src/wasix-libc/libc-bottom-half/sources/isatty.c
  current fdstat heuristic

~/src/wasix-libc/libc-bottom-half/cloudlibc/src/libc/sys/ioctl/ioctl.c
  current tty_get usage is not fd-specific

~/src/wasmer/lib/wasix/src/syscalls/wasix/proc_spawn*.rs
~/src/wasmer/lib/wasix/src/syscalls/wasix/proc_exec*.rs
  process file actions / child stdio fd setup

~/src/wasmer/lib/wasix/src/state/builder.rs
~/src/wasmer/lib/virtual-fs/src/*
  fd table and virtual file kind exposed to guest libc
```
