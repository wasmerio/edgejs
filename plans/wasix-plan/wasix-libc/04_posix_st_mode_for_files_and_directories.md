# POSIX `st_mode` for Files and Directories

Why this is a problem:

In the baseline libc stat conversion, WASI file metadata can be exposed without
normal POSIX `st_mode` type and permission bits. C code expects `S_ISREG()` and
`S_ISDIR()` to work. If libc leaves those bits empty or incomplete, callers see
ordinary files and directories as mode `0`-like objects.

The fix will synthesize POSIX type bits and conservative permission bits during
WASI-to-`struct stat` conversion. This belongs in libc, not in each guest
program after it calls `stat()`.

When it occurs:

- FastUTF8 stream stat tests;
- module loader file checks;
- any C or JS runtime code calling `stat()`, `fstat()`, or `fstatat()`.

Minimal Example:

```c
#include <stdio.h>
#include <sys/stat.h>

int main(int argc, char **argv) {
  const char *path = argc > 1 ? argv[1] : argv[0];
  struct stat st;

  if (stat(path, &st) != 0) {
    perror("stat");
    return 1;
  }

  printf("mode: %o\n", st.st_mode);
  printf("is regular: %d\n", S_ISREG(st.st_mode));
  printf("is dir: %d\n", S_ISDIR(st.st_mode));
}
```

Callgraph and boundary:

Current problematic path:

```text
C stat(path, &st)
  -> wasix-libc libc-bottom-half/sources/posix.c __wasilibc_stat(...)
  -> __wasi_path_filestat_get(...) or __wasi_fd_filestat_get(...)
  -> wasix-libc stat conversion
     -> maps WASI filetype incompletely into st.st_mode
        HERE IS THE PROBLEM: S_IFREG/S_IFDIR or useful permission bits are absent
  -> C caller checks S_ISREG(st.st_mode)
     -> false for a normal file
        HERE IS THE PROBLEM: POSIX stat contract is broken at libc boundary
```

Proposed solution:

Synthesize POSIX type bits and conservative permission bits in `wasix-libc`.
Do not normalize this in EdgeJS after the fact.

Relevant wasix-libc code paths:

```text
libc-bottom-half/sources/posix.c
  __wasilibc_stat(...)
    calls WASI filestat functions

libc-bottom-half/cloudlibc/src/libc/sys/stat/stat_impl.h
  to_public_stat(...)
    convert __wasi_filestat_t to struct stat
    set S_IFDIR/S_IFREG and default permission bits

libc-bottom-half/cloudlibc/src/libc/sys/stat/fstat.c
libc-bottom-half/cloudlibc/src/libc/sys/stat/fstatat.c
  share the same conversion helper
```

Proposed callgraph:

```text
C stat(path, &st)
  -> wasix-libc __wasilibc_stat(...)
  -> __wasi_path_filestat_get(...)
  -> to_public_stat(...)
  -> if WASI filetype is regular, st_mode |= S_IFREG | 0600
  -> if WASI filetype is directory, st_mode |= S_IFDIR | 0700
  -> C caller sees S_ISREG/S_ISDIR behave normally
```

Sketch:

```c
mode_t mode = 0;

switch (wasi_filetype) {
case __WASI_FILETYPE_DIRECTORY:
  mode |= S_IFDIR | 0700;
  break;
case __WASI_FILETYPE_REGULAR_FILE:
  mode |= S_IFREG | 0600;
  break;
}

st->st_mode = mode;
```

## Proposed Solution References

### Commits without PR

- Sadhbh: wasix-libc [631ef5d5](https://github.com/Anodized-Titanium/wasix-libc/commit/631ef5d5289a56b584fd84e4296627879d8d5e5c) Fix dir and file flags
