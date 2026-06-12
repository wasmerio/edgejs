# `proc_spawn3` / `proc_exec4` for Real argv/envp

Why this is a problem:

Older WASIX process syscalls encode `argv` and `envp` as newline-delimited
strings. That cannot represent an argument that itself contains `\n` or `\r`.
Node frequently spawns itself as:

```text
edge -e "<multi-line script>"
```

Splitting that by line feed turns one argument into many.

Minimal manifestation:

```js
const { spawnSync } = require('node:child_process');

const script = [
  "console.log('line 1')",
  "console.log('line 2')",
].join('\n');

const r = spawnSync(process.execPath, ['-e', script], { encoding: 'utf8' });
console.log(r.stdout);
```

Boundary:

```text
Node child_process.spawn()
  -> libuv uv_spawn()
  -> wasix-libc posix_spawn()
  -> wasix_32v1.proc_spawn3(argv_ptrs, argv_lens, env_ptrs, env_lens)
  -> Wasmer process runner
```

Proposed solution:

Add Wasmer imports that receive argument vectors as pointer/count arrays rather
than delimiter-encoded strings. The runtime should reconstruct exact byte
strings per argument.

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

Proposed solution can be found in:

**Reference commits:**

```text
wasmer d80c3d8c980 Add proc_spawn3 and proc_exec4...
wasmer b5880b5268b apply review comments
wasmer e9d1478b914 fix tests
wasmer bda2ead2504 Merge branch 'feat/wasix-proc-spawn3' into tmp-work-4
```

**Cross-project pair:**

```text
wasix-libc proc_spawn3/proc_exec4 lowering
libuv-wasix ba06698b libuv-wasix: use WASIX posix_spawn/proc_join...
```
