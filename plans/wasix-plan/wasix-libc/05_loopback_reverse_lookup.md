# Loopback Reverse Lookup

Why this is a problem:

POSIX applications expect loopback reverse lookup and `getnameinfo()` to return
`localhost` in ordinary environments. The guest should not need EdgeJS-specific
hard-coded answers for `127.0.0.1` or `::1`.

Minimal manifestation:

```js
const dns = require('node:dns');
dns.reverse('127.0.0.1', (err, names) => console.log(err, names));
```

Boundary:

```text
Node dns.reverse()
  -> c-ares / getnameinfo-shaped resolver behavior
  -> wasix-libc getnameinfo()
  -> /etc/hosts or loopback fallback
```

Proposed solution:

In libc resolver code, switch on address family and return `localhost` for IPv4
and IPv6 loopback. Also support a mounted `/etc/hosts`; do not create synthetic
hosts files in Wasmer.

Sketch:

```c
switch (family) {
case AF_INET:
  if (!memcmp(addr, "\x7f\x00\x00\x01", 4))
    strcpy(buf, "localhost");
  break;
case AF_INET6:
  if (is_ipv6_loopback(addr, scopeid))
    strcpy(buf, "localhost");
  break;
}
```

## Proposed Solution References

Proposed solution can be found in:

**Reference commits:**

```text
wasix-libc f157cd9 Reverse loopback
```
