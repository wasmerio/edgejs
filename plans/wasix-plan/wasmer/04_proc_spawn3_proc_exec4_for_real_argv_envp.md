# `proc_spawn3` / `proc_exec4` for Real argv/envp

Why this is a problem:

In the baseline process ABI, argv and envp are represented as newline-delimited
strings. That encoding cannot distinguish between a newline that separates two
arguments and a newline that is part of one argument. Any C program using
`posix_spawn()` or `execv()` with an argument containing `\n` can have that one
argument split into several arguments before the child starts.

The fix will add and use process syscalls that carry argument vectors as real
pointer/count arrays. Wasmer should reconstruct each argv/envp string from its
own pointer and length, preserving embedded newlines and other bytes.

Minimal Example:

```c
#include <spawn.h>
#include <stdio.h>
#include <sys/wait.h>

extern char **environ;

int main(void) {
  pid_t pid;
  char *argv[] = {
    "argv-dump",
    "first line\nsecond line",
    NULL,
  };

  int err = posix_spawnp(&pid, "argv-dump", NULL, NULL, argv, environ);
  if (err != 0) {
    printf("posix_spawnp failed: %d\n", err);
    return 1;
  }

  int status = 0;
  waitpid(pid, &status, 0);
  return status;
}
```

`argv-dump` can be any tiny C helper that prints `argv[1]`. The important
property is that `argv[1]` contains an embedded line feed and must arrive as one
argument.

Callgraph and boundary:

Current problematic path:

```text
C posix_spawnp("argv-dump", ["argv-dump", "first line\nsecond line"])
  -> wasix-libc posix_spawn()
  -> old WASIX proc_spawn/proc_spawn2-style ABI
     -> combines argv into one newline-delimited buffer
        HERE IS THE PROBLEM: embedded newline is indistinguishable from separator
  -> Wasmer process runner parses line-delimited arguments
  -> child receives argv = ["argv-dump", "first line", "second line"]
     HERE IS THE PROBLEM: child argv shape is corrupted before program starts

C execv(path, argv_with_newline)
  -> wasix-libc execv()/execvpe()
  -> old proc_exec/proc_exec3-style ABI
     -> same delimiter corruption for replacement process
```

Proposed solution:

Add Wasmer imports that receive argument vectors as pointer/count arrays rather
than delimiter-encoded strings. The runtime should reconstruct exact byte
strings per argument.

Relevant Wasmer code paths:

```text
lib/wasix/src/lib.rs
  export wasix_32v1.proc_spawn3 / wasix_64v1.proc_spawn3
  export wasix_32v1.proc_exec4 / wasix_64v1.proc_exec4

lib/wasix/src/syscalls/wasix/proc_spawn3.rs
  proc_spawn3(...)
  proc_spawn3_impl(...)
    read argv/envp arrays from guest memory as pointer/length pairs

lib/wasix/src/syscalls/wasix/proc_exec4.rs
  proc_exec4(...)
  proc_exec4_impl(...)
    read replacement argv/envp arrays the same way

lib/wasix/src/syscalls/wasix/proc_spawn2.rs
lib/wasix/src/syscalls/wasix/proc_exec3.rs
  keep legacy newline-delimited wrappers only as compatibility shims
```

Proposed callgraph:

```text
C posix_spawnp("argv-dump", argv)
  -> wasix-libc posix_spawn()
  -> __wasi_proc_spawn3(name, argv_ptrs, argv_lens, env_ptrs, env_lens, ...)
  -> Wasmer proc_spawn3_impl(...)
  -> read each argv[i] from its own pointer and length
  -> child receives argv[1] = "first line\nsecond line"

C execv(path, argv)
  -> wasix-libc execv()/execvpe()
  -> __wasi_proc_exec4(name, argv_ptrs, argv_lens, env_ptrs, env_lens, ...)
  -> Wasmer proc_exec4_impl(...)
  -> replacement process receives exact argv strings
```

Sketch:

```witx
(@interface func (export "proc_spawn3")
  (param $path string)
  (param $argv_ptrs (@witx pointer u32))
  (param $argv_lens (@witx pointer size))
  (param $argv_len size)
  ...
  (result $errno errno))
```

Runtime sketch:

```rust
let argv = read_string_array(memory, argv_ptrs, argv_lens, argv_len)?;
let envp = read_string_array(memory, env_ptrs, env_lens, env_len)?;
spawn_process(path, argv, envp, file_actions)
```

## Proposed Solution References

### Commits without PR

- wasix-libc proc_spawn3/proc_exec4 lowering

## Related Commits

- Arshia: wasmer [d80c3d8c980](https://github.com/Anodized-Titanium/wasmer/commit/d80c3d8c98065562c845c2ec8acfe0668798a166) Add proc_spawn3 and proc_exec4...
- Arshia: wasmer [b5880b5268b](https://github.com/Anodized-Titanium/wasmer/commit/b5880b5268b74aefc5b4295767286976d83b0deb) apply review comments
- Arshia: wasmer [e9d1478b914](https://github.com/Anodized-Titanium/wasmer/commit/e9d1478b914c224051541d35281ff0a0638fab21) fix tests
- Artem: libuv-wasix [ba06698b](https://github.com/Anodized-Titanium/libuv-wasix/commit/ba06698ba3f3b508a7a4eeacb23e0c911ebf4576) libuv-wasix: use WASIX posix_spawn/proc_join...
