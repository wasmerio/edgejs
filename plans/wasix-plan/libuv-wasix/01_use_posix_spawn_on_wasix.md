# libuv-wasix: use POSIX spawn on WASIX

Why this is a problem:

WASIX does not provide `fork()`. The normal Unix libuv child-process path is
fork/exec oriented, so pretending that fork succeeded creates fake child handles,
missing stdout/stderr, and tests that hang while waiting for child output or an
exit event.

This occurs whenever Node uses `child_process.spawn()`, `execFile()`, `exec()`,
`fork()`, or cluster worker creation on WASIX.

Minimal Example:

```c
#include <uv.h>
#include <stdio.h>

static void on_exit(uv_process_t *req, int64_t status, int signal) {
  printf("exit=%lld signal=%d\n", (long long)status, signal);
  uv_close((uv_handle_t *)req, NULL);
}

int main(void) {
  uv_loop_t *loop = uv_default_loop();
  uv_process_t child;
  char *args[] = { "edge", "-e", "console.log('child')", NULL };
  uv_process_options_t opts = {0};
  opts.file = "edge";
  opts.args = args;
  opts.exit_cb = on_exit;
  int rc = uv_spawn(loop, &child, &opts);
  if (rc != 0) return 1;
  return uv_run(loop, UV_RUN_DEFAULT);
}
```

Callgraph and boundary:

Current problematic path:

```text
Node child_process.spawn()
  -> libuv uv_spawn()
  -> libuv Unix fork/exec helper
     HERE IS THE PROBLEM: fork is not available on WASIX; returning success or
     leaving a fake pid makes the parent wait for child events that cannot occur.
  -> wasix-libc/Wasmer never receive a valid process-spawn request
```

The boundary is libuv <-> libc. libuv should call a POSIX-facing process API;
`wasix-libc` and Wasmer should own the WASIX syscall details.

Proposed solution:

Add a WASIX libuv process path that uses `posix_spawnp()` plus spawn file
actions. Do not emulate fork in libuv and do not rewrite EdgeJS JavaScript.

Relevant libuv-wasix code paths:

```text
~/src/edgejs/deps/libuv-wasix/src/unix/process.c
~/src/wasix-libc/libc-top-half/musl/src/process/posix_spawn.c
~/src/wasmer/lib/wasix/src/syscalls/wasix/proc_spawn*.rs
~/src/wasmer/lib/wasix/src/syscalls/wasix/proc_exec*.rs
```

Proposed callgraph:

```text
Node child_process.spawn()
  -> libuv uv_spawn()
  -> libuv WASIX uv__spawn_and_init_child_posix_spawn_wasi()
  -> posix_spawnp(file, file_actions, attrs, argv, envp)
  -> wasix-libc proc_spawn3/proc_exec4 ABI with pointer arrays
  -> Wasmer starts the child process and returns a real pid
  -> libuv reports spawn/exit/close events normally
```

The minimal libuv piece is just translation from libuv's process options and
stdio container into `posix_spawn_file_actions_*` calls plus `posix_spawnp()`.

## Proposed Solution References

### Commits without PR

- libuv-wasix [ba06698b](https://github.com/Anodized-Titanium/libuv-wasix/commit/ba06698ba3f3b508a7a4eeacb23e0c911ebf4576) libuv-wasix: use WASIX posix_spawn/proc_join for child processes
