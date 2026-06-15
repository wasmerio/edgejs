# Loopback Reverse Lookup

Why this is a problem:

In the baseline resolver path, `getnameinfo()` can fail to return `localhost`
for IPv4 or IPv6 loopback addresses. That pushes guests toward hard-coded
application-level loopback answers, which is the wrong layer. A POSIX-like libc
should provide ordinary loopback reverse lookup behavior itself, using
`/etc/hosts` where available and a small loopback fallback where appropriate.

The fix will make `getnameinfo()` recognize loopback addresses by family and
return `localhost` when no hosts-file result has already filled the name.

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
     -> /etc/hosts may be unavailable or may not fill result
  -> no loopback fallback by family
     HERE IS THE PROBLEM: 127.0.0.1/::1 can remain unnamed
  -> getnameinfo returns failure or numeric fallback
     HERE IS THE PROBLEM: guest does not get normal localhost reverse lookup
```

Proposed solution:

In libc resolver code, switch on address family and return `localhost` for IPv4
and IPv6 loopback. Also support a mounted `/etc/hosts`; do not create synthetic
hosts files in Wasmer.

Relevant wasix-libc code paths:

```text
libc-top-half/musl/src/network/getnameinfo.c
  getnameinfo(...)
    after reverse_hosts(...), if host buffer is empty, call reverse_loopback(...)

  reverse_loopback(...)
    switch on AF_INET / AF_INET6
    map 127.0.0.1 and ::1 to localhost
```

Proposed callgraph:

```text
C getnameinfo(127.0.0.1, NI_NAMEREQD)
  -> wasix-libc getnameinfo(...)
  -> reverse_hosts(...)
  -> if result is still empty, reverse_loopback(...)
  -> family == AF_INET and addr == 127.0.0.1
  -> host = "localhost"
  -> caller receives localhost
```

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

### Commits without PR

- Sadhbh: wasix-libc [1a028f0e9](https://github.com/Anodized-Titanium/wasix-libc/commit/1a028f0e9aeaffa580fc6bc5962069531fda322d) Reverse loopback
