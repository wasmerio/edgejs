# Use POSIX Spawn on WASIX

Why this is a problem:

WASI/WASIX does not provide `fork()` in the POSIX sense. libuv process spawn
must use a spawn-style API, then wait for process completion through the WASIX
join mechanism. Returning success from an unimplemented fork-like path creates
fake child handles and hangs.

When it occurs:

- `child_process.spawn()`;
- `child_process.execFile()`;
- Node self-spawn with `process.execPath`;
- proxy tests that spawn child clients.

Minimal manifestation:

```js
const { spawn } = require('node:child_process');
const child = spawn(process.execPath, ['-e', "console.log('child')"]);
child.stdout.on('data', b => process.stdout.write(b));
```

Boundary:

```text
Node child_process
  -> libuv uv_spawn()
  -> wasix-libc posix_spawn()
  -> Wasmer proc_spawn3
  -> libuv process exit watcher via proc_join
```

Proposed solution:

Implement the libuv Unix process path with `posix_spawn()` and WASIX process
join primitives. Keep argv unchanged; do not base64-wrap multiline scripts in
EdgeJS.

Sketch:

```c
int r = posix_spawnp(&pid, file, &file_actions, &attr, args, env);
if (r != 0)
  return UV__ERR(r);

process->pid = pid;
uv__wasix_register_process_join(loop, process);
```

## Proposed Solution References

Proposed solution can be found in:

**Reference commits:**

```text
libuv-wasix ba06698b libuv-wasix: use WASIX posix_spawn/proc_join for child processes
```
