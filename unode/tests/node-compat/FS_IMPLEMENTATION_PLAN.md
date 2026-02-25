# FS Full Implementation Plan

Goal: implement the Node.js `fs` module fully enough to run Node's own fs tests (and existing unode tests), using the reference Node.js implementation and tests.

---

## 1. Current state (unode)

### C++ binding (`unode_fs.h` / `unode_fs.cc`)

- **Installed as:** `globalThis.__unode_fs`
- **Methods:** `readFileUtf8(path, flags)`, `writeFileUtf8(path, data, flags, mode)`, `mkdir(path, mode, recursive)`, `rmSync(path, maxRetries, recursive, retryDelay)`, `readdir(path, withFileTypes)`, `realpath(path)`
- **Error reporting:** `ThrowUVException`, `ThrowErrnoException` (code, errno, syscall, path on error object)
- **Constants:** O_* flags, UV_DIRENT_* types, exposed on the binding object

### JS builtin (`builtins/fs.js`)

- **Exports:** `readFileSync`, `writeFileSync`, `mkdirSync`, `rmSync`, `readdirSync`, `realpathSync`, `constants`, `Dirent`
- **Helpers:** `getValidatedPath`, `getOptions`, `stringToFlags`, `parseFileMode`, `validateRmOptionsSync`, `Dirent` class (with `isFile`, `isDirectory`, etc.)
- **Limitations:** UTF-8 only for read/write; no `openSync`/`closeSync`; no `stat`/`lstat`/`fstat`; no `Stats`; no `existsSync`; no `accessSync`; no Buffer support; no async APIs

---

## 2. Node.js fs API reference

Source: `node/lib/fs.js` and `node/lib/internal/fs/utils.js`.

### Sync APIs (priority for unode)

| API | Node binding / impl | Used by tests / common |
|-----|---------------------|------------------------|
| readFileSync | binding.open → read → close (or readFile) | Many tests, utf8 + buffer |
| writeFileSync | binding.open → write → close | Many tests |
| readdirSync | binding.getDirents / getDirents | test-fs-readdir, tmpdir |
| realpathSync | binding.realpath | tmpdir, test-fs-realpath |
| mkdirSync | binding.mkdir | tmpdir, test-fs-mkdir |
| rmSync | binding.rm (sync) | tmpdir, test-fs-rm |
| openSync | binding.open | test-fs-write, test-fs-read, test-fs-stat |
| closeSync | binding.close | test-fs-write, test-fs-stat |
| statSync | binding.stat → getStatsFromBinding | test-fs-stat, test-fs-exists (via binding) |
| lstatSync | binding.lstat | test-fs-stat, symlinks |
| fstatSync | binding.fstat | test-fs-stat |
| existsSync | binding.existsSync | test-fs-exists, common |
| accessSync | binding.access | existsSync in Node |
| writeSync | binding.writeSync | test-fs-write-sync |
| readSync | binding.readSync | test-fs-read |
| renameSync | binding.rename | test-fs-rename |
| unlinkSync | binding.unlink | test-fs-unlink |
| rmdirSync | binding.rmdir | test-fs-rmdir |
| truncateSync / ftruncateSync | binding.truncate / ftruncate | test-fs-truncate |
| copyFileSync | binding.copyFile | test-fs-copyfile |
| appendFileSync | open(append) + write + close | test-fs-append-file |
| statfsSync | binding.statfs | common/tmpdir (optional; we stub hasEnoughSpace) |
| readlinkSync | binding.readlink | test-fs-readlink |
| symlinkSync | binding.symlink | test-fs-symlink |
| chmodSync / fchmodSync | binding.chmod / fchmod | test-fs-chmod |
| chownSync / fchownSync | binding.chown / fchown | test-fs-chown (often skipped non-root) |
| utimesSync / futimesSync | binding.utimes / futimes | test-fs-utimes |
| mkdtempSync | binding.mkdtemp | test-fs-mkdtemp |

### Stats object (Node)

- **Source:** `node/lib/internal/fs/utils.js` – `Stats` and `StatsBase`, `getStatsFromBinding(stats, offset)`.
- **Binding returns:** A typed array of 18 fields (see `node/src/node_file.h` `FsStatsOffset`): dev, mode, nlink, uid, gid, rdev, blksize, ino, size, blocks, atime_sec, atime_nsec, mtime_sec, mtime_nsec, ctime_sec, ctime_nsec, birthtime_sec, birthtime_nsec.
- **Stats constructor:** `Stats(dev, mode, nlink, uid, gid, rdev, blksize, ino, size, blocks, atimeMs, mtimeMs, ctimeMs, birthtimeMs)` plus lazy `atime`/`mtime`/`ctime`/`birthtime` Date getters.
- **Methods:** `isDirectory()`, `isFile()`, `isBlockDevice()`, `isCharacterDevice()`, `isSymbolicLink()`, `isFIFO()`, `isSocket()` (via `_checkModeProperty(S_IFMT)`).

### Constants

- **O_*** (from binding): O_RDONLY, O_WRONLY, O_RDWR, O_CREAT, O_EXCL, O_TRUNC, O_APPEND, O_SYNC, etc.
- **UV_DIRENT_*** (from binding): UV_DIRENT_FILE, UV_DIRENT_DIR, UV_DIRENT_LINK, etc.
- **fs constants:** R_OK, W_OK, X_OK, F_OK (for access); S_IFMT, S_IFREG, S_IFDIR, S_IFBLK, S_IFCHR, S_IFLNK, S_IFIFO, S_IFSOCK (for Stats.is*); copyFile (COPYFILE_EXCL, etc.) – many come from `internal/fs/utils` or binding.

---

## 3. Node.js fs tests (reference)

Location: `node/test/parallel/test-fs-*.js`.

### Tests that need minimal extra API (good first targets)

- **test-fs-exists.js** – `existsSync`, `exists` (callback). Uses `binding.existsSync` in Node.
- **test-fs-readdir.js** – `readdirSync`, `readdir`, `openSync`, `closeSync`. We have readdir; need openSync/closeSync to create empty files.
- **test-fs-stat.js** – `stat`, `lstat`, `fstat`, `open`, `close`, `fstatSync`, `statSync`; asserts on `stats.mtime`, `stats.isFile()`, `stats.isDirectory()`, etc. Needs Stats + stat/lstat/fstat + open/close.
- **test-fs-write-file-sync.js** – `writeFileSync`, `readFileSync`, `mkdirSync`, `rmSync`, `existsSync`. Mostly covered; may need existsSync.
- **test-fs-readfile-sync.js** (if present) – readFileSync. Covered.
- **test-fs-realpath.js** – realpathSync. Covered.
- **test-fs-rm.js** – rmSync, readdirSync, mkdirSync, writeFileSync. Covered.
- **test-fs-mkdir.js** – mkdirSync (recursive), rmSync, statSync. Needs statSync (or skip stat assertions).
- **test-fs-truncate-sync.js** – truncateSync, ftruncateSync, openSync, closeSync, readFileSync. Needs open/close/truncate.
- **test-fs-write-sync.js** – openSync, writeSync, closeSync, readFileSync. Needs open/write/close.
- **test-fs-rename.js** – renameSync, writeFileSync, readFileSync, existsSync. Needs renameSync, existsSync.
- **test-fs-unlink.js** – unlinkSync, writeFileSync, existsSync. Needs unlinkSync, existsSync.
- **test-fs-copyfile-sync.js** – copyFileSync. Needs copyFileSync.

### Tests that need streams / async (later)

- createReadStream, createWriteStream – many test-fs-write-stream-*, test-fs-read-stream-*.
- Async variants (open, read, write, stat with callbacks) – for parity.
- watch / watchFile – test-fs-watch*, test-fs-watchfile*.

---

## 4. Implementation phases

### Phase A: Stats + exists + access (enables test-fs-stat, test-fs-exists)

**C++**

- Add `stat(path)` and `lstat(path)` returning a fixed-size array of 18 numbers (same order as Node’s FsStatsOffset: dev, mode, nlink, uid, gid, rdev, blksize, ino, size, blocks, atime_sec, atime_nsec, mtime_sec, mtime_nsec, ctime_sec, ctime_nsec, birthtime_sec, birthtime_nsec). Use `uv_fs_stat` / `uv_fs_lstat` and fill the array from `uv_stat_t`.
- Add `fstat(fd)` same layout (use `uv_fs_fstat`).
- Add `existsSync(path)` – uv_fs_access or uv_fs_stat; return boolean (no throw).
- Add `accessSync(path, mode)` – uv_fs_access; throw on failure. Expose F_OK, R_OK, W_OK, X_OK on binding if not already.
- Expose S_IFMT, S_IFREG, S_IFDIR, S_IFBLK, S_IFCHR, S_IFLNK, S_IFIFO, S_IFSOCK on binding for Stats.

**JS**

- Add `Stats` class in `builtins/fs.js` (or separate `builtins/fs-stats.js`): constructor(dev, mode, nlink, uid, gid, rdev, blksize, ino, size, blocks, atimeMs, mtimeMs, ctimeMs, birthtimeMs); lazy Date getters for atime, mtime, ctime, birthtime; isDirectory(), isFile(), isBlockDevice(), isCharacterDevice(), isSymbolicLink(), isFIFO(), isSocket() using (mode & S_IFMT) === S_IF*.
- Add `statSync(path, options)`, `lstatSync(path, options)`, `fstatSync(fd, options)` – call binding stat/lstat/fstat, build Stats from the 18-element array (convert atime/mtime/ctime/birthtime sec+nsec to ms). Support `options.throwIfNoEntry` (default true) for statSync/lstatSync.
- Add `existsSync(path)` – call binding.existsSync; handle invalid path (return false, match Node deprecation behavior if desired).
- Add `accessSync(path, mode)` – call binding.accessSync.

**Tests to run**

- Raw: `test-fs-exists.js`, `test-fs-stat.js` (with common that uses our builtins).

---

### Phase B: open / close / read / write (enables test-fs-write-sync, test-fs-read, test-fs-readdir)

**C++**

- Add `open(path, flags, mode)` – uv_fs_open; return integer fd.
- Add `close(fd)` – uv_fs_close.
- Add `readSync(fd, buffer, offset, length, position)` – accept a buffer (Node passes Buffer; we can accept a TypedArray or add a variant that returns a new buffer). Use uv_fs_read. For simplicity, first support “read into pre-allocated buffer” and/or “read whole file into new buffer” and return bytes read.
- Add `writeSync(fd, buffer, offset, length, position)` – uv_fs_write; buffer from JS (string or TypedArray/Buffer). Return bytes written.

**JS**

- Add `openSync(path, flags, mode)` – call binding.open; return fd (number).
- Add `closeSync(fd)` – call binding.close.
- Add `readSync(fd, buffer, offsetOrOptions, length, position)` – validate fd and buffer/options; call binding.readSync; return bytes read. Support Buffer and encoding for “return string” behavior if we add a read variant that returns data.
- Add `writeSync(fd, buffer, offsetOrOptions, length, position)` – same shape as Node; call binding.writeSync.

**Encoding / Buffer**

- Node’s readSync/writeSync support Buffer and encoding. Phase B can start with Buffer-only (or buffer + utf8 string) and extend later.

**Tests to run**

- test-fs-write-sync.js, test-fs-read.js (sync parts), test-fs-readdir.js (already uses openSync/closeSync to create files).

---

### Phase C: More sync APIs (rename, unlink, rmdir, truncate, copyFile, appendFile)

**C++**

- Add `rename(oldPath, newPath)`.
- Add `unlink(path)`.
- Add `rmdir(path)` (and optionally options for recursive – Node has rmdirSync(path, options)).
- Add `truncateSync(path, len)` and `ftruncateSync(fd, len)` (uv_fs_ftruncate, uv_fs_truncate).
- Add `copyFileSync(src, dest, mode)` (uv_fs_copyfile).

**JS**

- Add `renameSync`, `unlinkSync`, `rmdirSync`, `truncateSync`, `ftruncateSync`, `copyFileSync`, `appendFileSync` (implement via openSync with O_APPEND, writeSync, closeSync, or add binding.appendFile if preferred).

**Tests to run**

- **Subset (green):** `unode/tests/node-compat/parallel/test-fs-phase-c-subset.js` – covers truncateSync, renameSync, unlinkSync, copyFileSync, appendFileSync (string + buffer), rmdirSync. Run via `FsPhaseCSubsetTest`.
- **Raw Node tests (all passing):** The following Node tests from `node/test/parallel/` are run unchanged by the phase02 runner (when `NAPI_V8_NODE_ROOT_PATH` is set). Common, fixtures, and builtins were extended so they all pass.
  - **test-fs-rename-type-check.js** – `RawFsRenameFromNodeTest`.
  - **test-fs-unlink-type-check.js** – `RawFsUnlinkFromNodeTest`.
  - **test-fs-truncate-sync.js** – `RawFsTruncateSyncFromNodeTest`.
  - **test-fs-copyfile.js** – `RawFsCopyfileSyncFromNodeTest` (internal/test/binding stub, copyfile constants and error path/dest in C++, copyFile/copyFileSync validation in fs.js).
  - **test-fs-append-file-sync.js** – `RawFsAppendFileSyncFromNodeTest` (fixtures.utf8TestText, appendFileSync data validation and fd support in fs.js).
- **Not yet in runner:** No raw test for rmdir (Node has `test-fs-rmdir-type-check.js` and recursive variants; rmdirSync is covered by the Phase C subset test only).

---

### Phase D: Symlinks, chmod, utimes, mkdtemp (optional)

**C++ (implemented)**

- `readlink`, `symlink` (with UV_FS_SYMLINK_DIR / UV_FS_SYMLINK_JUNCTION constants), `chmod`, `fchmod`, `utimes`, `futimes`, `mkdtemp` in `unode_fs.cc`; all use libuv sync API and throw via `ThrowUVException` on error.

**JS (implemented)**

- `readlinkSync`, `readlink`, `symlinkSync`, `symlink`, `chmodSync`, `chmod`, `fchmodSync`, `fchmod`, `utimesSync`, `utimes`, `futimesSync`, `mkdtempSync`, `mkdtemp`; `_toUnixTimestamp` for tests; symlink type → flags via `symlinkTypeToFlags`; mkdtemp appends `XXXXXX` if missing.
- Path builtin: `dirname`, `basename` added for Phase D tests.
- Common: `canCreateSymLink()` added (returns true).
- `getValidatedPath` accepts Buffer-like / Uint8Array and file URL (strip `file://`).

**Raw Node tests in runner**

- `RawFsMkdtempFromNodeTest` (test-fs-mkdtemp.js) – may fail on basename length assertion (14 vs 12) due to platform.
- `RawFsReadlinkTypeCheckFromNodeTest` (test-fs-readlink-type-check.js) – **passing**.
- `RawFsSymlinkFromNodeTest` (test-fs-symlink.js) – needs `assert.rejects` in assert builtin.
- `RawFsChmodFromNodeTest` (test-fs-chmod.js) – needs error message shape (path argument name).
- `RawFsUtimesFromNodeTest` (test-fs-utimes.js) – needs `util.inspect` in util builtin.

**Tests (Node)**

- test-fs-symlink.js, test-fs-readlink-type-check.js, test-fs-chmod.js, test-fs-utimes.js, test-fs-mkdtemp.js.

---

### Phase E: Async APIs and streams (optional)

- Async versions of open, close, read, write, stat, readFile, writeFile, etc. (callbacks and/or promises).
- createReadStream, createWriteStream (or defer to Node’s streams when running in Node).

---

## 5. Test strategy

1. **Unode-owned tests** – Add small unit tests under `unode/tests/` that call our fs builtin directly (e.g. readFileSync, writeFileSync, statSync, existsSync) and assert behavior. These don’t require Node’s test harness.
2. **Node test copy / subset** – Optionally copy a few Node tests into `unode/tests/node-compat/parallel/` (e.g. test-fs-exists-sync-subset.js) that use only our implemented APIs and our common.
3. **Raw Node tests** – Once Phase A/B are done, add more raw CTest cases (like RawRequireDotFromNodeTest) that run `node/test/parallel/test-fs-exists.js`, `test-fs-stat.js`, `test-fs-write-sync.js`, etc., with `UNODE_FALLBACK_BUILTINS_DIR` and `NODE_TEST_DIR`. Fix any small differences (e.g. error messages, optional options).
4. **CI** – Run the chosen fs tests in CI when `NAPI_V8_NODE_ROOT_PATH` is set.

---

## 6. Reference files (Node)

| What | Path |
|------|------|
| fs API | `node/lib/fs.js` |
| Stats, getStatsFromBinding, stringToFlags, parseFileMode | `node/lib/internal/fs/utils.js` |
| FsStatsOffset (stat array layout) | `node/src/node_file.h` |
| Binding (C++) | `node/src/node_file.cc` (internal binding) |
| Tests | `node/test/parallel/test-fs-*.js` |

---

## 7. Suggested order of work

1. **Phase A** – Implement stat/lstat/fstat (C++), Stats (JS), existsSync, accessSync. Add RawFsExistsFromNodeTest and RawFsStatFromNodeTest.
2. **Phase B** – Implement openSync, closeSync, readSync, writeSync (C++ + JS). Add RawFsWriteSyncFromNodeTest, RawFsReaddirFromNodeTest.
3. **Phase C** – Implement renameSync, unlinkSync, rmdirSync, truncateSync, ftruncateSync, copyFileSync, appendFileSync. All Phase C raw tests in the runner pass (rename, unlink, truncate, copyfile, append-file-sync); rmdir has no raw test in the runner yet.
4. **Phase D/E** – As needed for additional Node tests or compatibility.

This plan gets you to a fully tested, Node-aligned fs implementation in stages, with clear reference to Node’s implementation and tests.
