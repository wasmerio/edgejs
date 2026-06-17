# Loopback Reverse Lookup Through /etc/hosts And Fallback

Why this is a problem:

In the baseline resolver path, `getnameinfo()` can fail to return `localhost`
for IPv4 or IPv6 loopback addresses when the WASIX guest has no usable
`/etc/hosts` and no useful PTR DNS answer. That pushes guests toward
application-level loopback answers, which is the wrong layer.

The normal POSIX-shaped configuration is still to provide loopback entries
through `/etc/hosts`. The WASIX package or runner should provide an `/etc/hosts`
file and mount it into the guest as `/etc/hosts`.

The required hosts entries are:

```text
127.0.0.1 localhost
::1 localhost ip6-localhost ip6-loopback
```

However, keeping a libc/runtime fallback for the canonical loopback addresses is
also acceptable. POSIX specifies the API behavior, but leaves the underlying name
service data source as an implementation detail of the operating environment.
That data source may be `/etc/hosts`, DNS, NSS, a platform resolver, or a small
built-in answer for reserved addresses. A fallback for `127.0.0.1` and `::1`
therefore does not have to be treated as an application workaround.

Minimal Example:

```c
#include <arpa/inet.h>
#include <netdb.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>

int main(void) {
  struct sockaddr_in addr = {0};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(80);
  inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

  char host[NI_MAXHOST];
  int err = getnameinfo((struct sockaddr *)&addr, sizeof(addr),
                        host, sizeof(host), NULL, 0, NI_NAMEREQD);
  if (err != 0) {
    printf("getnameinfo: %s\n", gai_strerror(err));
    return 1;
  }

  printf("host: %s\n", host);
}
```

Callgraph and boundary:

Current problematic path:

```text
C getnameinfo(127.0.0.1, NI_NAMEREQD)
  -> wasix-libc libc-top-half/musl/src/network/getnameinfo.c
  -> reverse_hosts(...)
     -> opens /etc/hosts inside the guest
     HERE IS THE PROBLEM: /etc/hosts may be missing or not mounted
  -> DNS PTR path is unavailable or does not answer localhost
  -> no reserved-address fallback exists
     HERE IS THE PROBLEM: the WASIX environment has no remaining name-service
        source for the canonical loopback name
  -> NI_NAMEREQD returns EAI_NONAME, or without NI_NAMEREQD numeric fallback wins
```

Related forward lookup path:

```text
C getaddrinfo("localhost", ...)
  -> wasix-libc libc-top-half/musl/src/network/lookup_name.c
  -> name_from_hosts(...)
     -> opens /etc/hosts inside the guest
     HERE IS THE SAME CONFIGURATION PROBLEM: without /etc/hosts, localhost
        forward lookup is not configured the way a normal POSIX environment is
        configured
```

Proposed solution:

Use both layers, with clear ownership:

1. Provide the expected hosts file in the WASIX guest and mount it as
   `/etc/hosts`. This is the normal configuration path and fixes both forward
   and reverse lookup.
2. Keep a narrow `wasix-libc` reverse-lookup fallback for the reserved loopback
   addresses `127.0.0.1` and `::1` when `/etc/hosts` and PTR lookup do not
   produce a name. This is an implementation-defined resolver data source, not
   an EdgeJS or application-level workaround.

For EdgeJS QuickJS WASIX, the package should include a hosts file such as:

```text
quickjs-wasm/etc/hosts
```

with:

```text
127.0.0.1 localhost
::1 localhost ip6-localhost ip6-loopback
```

and the Wasmer package or runner should mount it as `/etc/hosts`, for example by
mounting the containing directory as `/etc`:

```text
quickjs-wasm/etc:/etc
```

or, if the package format supports file mounts directly:

```text
quickjs-wasm/etc/hosts:/etc/hosts
```

Relevant code paths:

```text
wasix-libc
  libc-top-half/musl/src/network/getnameinfo.c
    reverse_hosts(...)
      reads /etc/hosts for address -> name
    reverse_loopback(...)
      if reverse_hosts/PTR do not provide a name, map canonical loopback
      addresses to localhost

wasix-libc
  libc-top-half/musl/src/network/lookup_name.c
    name_from_hosts(...)
      reads /etc/hosts for name -> address

edgejs package / runner
  quickjs-wasm/etc/hosts
  wasmer.toml or runner volume configuration
    mounts hosts file into guest as /etc/hosts
```

Proposed reverse lookup callgraph:

```text
C getnameinfo(127.0.0.1, NI_NAMEREQD)
  -> wasix-libc getnameinfo(...)
  -> reverse_hosts(...)
  -> open /etc/hosts inside guest
  -> if hosts contains 127.0.0.1 localhost, caller receives localhost
  -> otherwise PTR lookup is attempted where available
  -> if still empty, reverse_loopback(...)
  -> family == AF_INET and addr == 127.0.0.1
  -> caller receives localhost
```

Forward lookup remains configuration-driven:

```text
C getaddrinfo("localhost", ...)
  -> wasix-libc __lookup_name(...)
  -> name_from_hosts(...)
  -> open /etc/hosts inside guest
  -> find 127.0.0.1 / ::1 localhost entries
  -> caller receives loopback addresses
```

This keeps the preferred configuration in `/etc/hosts`, while still letting the
WASIX libc/runtime provide a small, deterministic fallback for reserved loopback
addresses when the guest filesystem does not provide one.

## Proposed Solution References

### Commits without PR

- Sadhbh: wasix-libc [1a028f0e9](https://github.com/Anodized-Titanium/wasix-libc/commit/1a028f0e9aeaffa580fc6bc5962069531fda322d) Reverse loopback
- Sadhbh: edgejs [dc9a0465](https://github.com/wasmerio/edgejs/commit/dc9a04651f74fd820f9bba7ec97ec6f342929df6) Added /etc/hosts, removed stream type normalization
