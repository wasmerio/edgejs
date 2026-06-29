# EdgeJS: remove WASIX-only guest workarounds after lower-layer plans land

Why this is a problem:

EdgeJS is the guest. If native EdgeJS works through POSIX APIs, the WASIX build
should work through the same APIs once `wasix-libc` and Wasmer provide correct
semantics. WASIX-only rewrites in EdgeJS hide missing platform behavior and make
EdgeJS diverge from native Node behavior.

This occurs when EdgeJS rewrites multiline `-e` arguments, hard-codes loopback
resolver answers, normalizes stat modes, or changes stream type behavior only
under `__wasi__`.

Why native EdgeJS passes, and why WASIX still needs this:

Native EdgeJS passes because the host libc/kernel already preserve argv strings,
provide resolver files, return conventional stat modes, and implement the socket
semantics that libuv expects. WASIX failures in this bucket came from those lower
layers being incomplete, not from JavaScript wanting different behavior. EdgeJS
workarounds were useful to prove causality, but they are the wrong final shape:
they make this one guest special while every other POSIX-style WASIX program
still sees the broken behavior. The EdgeJS action here is therefore mostly
negative: remove guest-only branches once `wasix-libc`, Wasmer, or libuv-wasix
implements the native contract.

Minimal Example:

```js
const assert = require('node:assert');
const { spawnSync } = require('node:child_process');
const dns = require('node:dns');
const fs = require('node:fs');

const script = "console.log('a')\nconsole.log('b')";
const child = spawnSync(process.execPath, ['-e', script], { encoding: 'utf8' });
assert.strictEqual(child.status, 0);
assert.match(child.stdout, /a\nb/);

dns.lookupService('127.0.0.1', 80, (err, host) => {
  assert.ifError(err);
  assert.strictEqual(host, 'localhost');
});

const mode = fs.statSync(__filename).mode;
assert.notStrictEqual(mode & 0o777, 0);
```

Failing tests that showed this class before lower-layer fixes:

```text
client-proxy/test-http-proxy-fetch
parallel/test-stream-readable-unpipe-resume
parallel/test-fastutf8stream-mode
parallel/test-http-client-default-headers-exist
parallel/test-dns-*
```

Callgraph and boundary:

Current problematic path:

```text
JavaScript child_process.spawn(process.execPath, ['-e', multilineScript])
  -> EdgeJS process wrapper rewrites script text for WASIX
     HERE IS THE PROBLEM: EdgeJS is compensating for old newline-delimited
     WASIX argv instead of using a libc/runtime argv API that preserves strings.

JavaScript dns.lookupService('127.0.0.1')
  -> EdgeJS DNS binding special-cases loopback
     HERE IS THE PROBLEM: resolver behavior is hard-coded in the guest instead
     of coming from wasix-libc resolver semantics and mounted /etc/hosts.

JavaScript fs.statSync(path)
  -> EdgeJS normalizes mode bits
     HERE IS THE PROBLEM: stat mode compatibility belongs in wasix-libc/Wasmer.
```

The boundary is EdgeJS <-> POSIX-facing libc/runtime APIs. Any workaround here
should be treated as temporary investigation scaffolding.

Proposed solution:

Remove EdgeJS-only compatibility shims once the lower layers provide the needed
behavior:

- argv/envp preservation belongs in `wasix-libc` and Wasmer process syscalls;
- resolver behavior belongs in `wasix-libc` plus mounted resolver files;
- file mode behavior belongs in `wasix-libc`/Wasmer stat translation;
- stream/socket behavior belongs in libuv, `wasix-libc`, and Wasmer.

Relevant EdgeJS code paths:

```text
~/src/edgejs/src/edge_process_wrap.cc
~/src/edgejs/src/edge_spawn_sync.cc
~/src/edgejs/src/edge_cares_wrap.cc
~/src/edgejs/scripts/edge-wasix-node-runner.sh
```

Proposed callgraph:

```text
JavaScript child_process.spawn(process.execPath, ['-e', multilineScript])
  -> EdgeJS forwards argv unchanged
  -> libuv-wasix posix_spawnp()
  -> wasix-libc proc_spawn3/proc_exec4 argv pointer array
  -> Wasmer starts child with exact argv strings

JavaScript dns.lookupService(loopback)
  -> EdgeJS calls normal resolver path
  -> wasix-libc reads /etc/hosts / resolver config
  -> normal POSIX-style answer

JavaScript fs.statSync(path)
  -> EdgeJS exposes stat result unchanged
  -> wasix-libc/Wasmer provide compatible mode bits
```

The rule is simple: if a JS test passes natively and only fails on WASIX because
of libc/runtime semantics, prefer fixing libc/runtime and deleting the EdgeJS
branch.

## Proposed Solution References

### [wasmerio/edgejs#91: [WIP] Node tests using Edgejs WASIX QuickJS](https://github.com/wasmerio/edgejs/pull/91)

- Sadhbh: edgejs [f1999f45](https://github.com/wasmerio/edgejs/commit/f1999f45d43453d508db45b3b52e4115983e26a8) Removed hacks: loopback, and spawn
- Sadhbh: edgejs [27118329](https://github.com/wasmerio/edgejs/commit/27118329f47b9876c3f415bf42669f9cc4a6568b) Fixes around edge process wrap, plus w/a for edge -e eval *(reverted by f1999f45 above)*
- Sadhbh: edgejs [61ba9604](https://github.com/wasmerio/edgejs/commit/61ba9604327a7326a555c2d537d7214421c60a9c) various fixes *(reverted by 27118329 above)*
