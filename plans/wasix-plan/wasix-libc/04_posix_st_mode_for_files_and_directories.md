# POSIX `st_mode` for Files and Directories

Why this is a problem:

Node inspects POSIX `st_mode` bits to determine whether a path is a regular
file, directory, executable, or readable/writable. WASI file metadata does not
automatically look like POSIX mode bits.

When it occurs:

- FastUTF8 stream stat tests;
- module loader file checks;
- any Node code calling `fs.stat()`.

Minimal manifestation:

```js
const fs = require('node:fs');
const st = fs.statSync(__filename);
console.log((st.mode & 0o170000).toString(8)); // should show regular-file type
```

Boundary:

```text
Node fs.stat()
  -> libuv uv_fs_stat()
  -> wasix-libc stat()
  -> WASI fd_filestat_get/path_filestat_get
  -> POSIX st_mode synthesis
```

Proposed solution:

Synthesize POSIX type bits and conservative permission bits in `wasix-libc`.
Do not normalize this in EdgeJS after the fact.

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

Proposed solution can be found in:

**Reference commits:**

```text
wasix-libc 57b1083 Fix dir and file flags
```
