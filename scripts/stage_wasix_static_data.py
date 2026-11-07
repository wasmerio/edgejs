#!/usr/bin/env python3
"""Stage the static data that a WASIX build did not embed in its binary.

A build configured with EDGE_EMBED_BUILTIN_SOURCES=OFF or EDGE_EMBED_ICU_DATA=OFF
reads that data from the package filesystem instead of from passive data
segments, which wasm would otherwise copy into every instance's linear memory.
This script writes the data next to the build, where the package manifests
(wasmer.toml, quickjs-wasm/wasmer.toml) map it into the guest:

    <build>/node-lib  -> /node-lib            builtin JS, deps under internal/deps
    <build>/icu       -> /usr/share/edge-icu  ICU items as individual files

Each tree is staged only when the build's CMakeCache.txt has the matching
option OFF, and is removed and recreated on every run. node-lib also gets a
.edge-builtin-catalog file holding the catalog digest that the binary was
built with (kBuiltinCatalogDigest), so the runtime can reject a stale tree.
"""

from __future__ import annotations

import argparse
import bz2
from pathlib import Path
import re
import shutil
import sys

SCRIPTS_DIR = Path(__file__).resolve().parent
sys.path.insert(0, str(SCRIPTS_DIR))
# Do not leave __pycache__ directories in the source tree.
sys.dont_write_bytecode = True

import extract_icu_data  # noqa: E402
import generate_builtin_catalog_header as builtin_catalog  # noqa: E402

CATALOG_MARKER = ".edge-builtin-catalog"
ICU_DATA_PACKAGE = Path("deps/icu-small/source/data/in/icudt78l.dat.bz2")
ICU_DATA_NAME = "icudt78l"
# Items in ICU 78's common data package. Update together with the ICU version.
ICU_EXPECTED_ITEMS = 4305

_CMAKE_FALSE = {"", "0", "OFF", "NO", "FALSE", "N", "IGNORE", "NOTFOUND"}


class StageError(RuntimeError):
    pass


def cmake_bool(value: str) -> bool:
    upper = value.strip().upper()
    return not (upper in _CMAKE_FALSE or upper.endswith("-NOTFOUND"))


def read_cmake_cache(path: Path) -> dict[str, str]:
    """Returns cache entries keyed by name, without the :TYPE suffix."""
    try:
        text = path.read_text(encoding="utf-8")
    except FileNotFoundError as error:
        raise StageError(
            f"{path} not found; configure the build before staging its static data"
        ) from error
    settings: dict[str, str] = {}
    for line in text.splitlines():
        match = re.match(r"^([A-Za-z0-9_.+-]+)(?::[A-Z_]+)?=(.*)$", line)
        if match:
            settings[match.group(1)] = match.group(2)
    return settings


def option_enabled(settings: dict[str, str], name: str) -> bool:
    # Options missing from the cache take their CMake default, which is ON.
    return cmake_bool(settings.get(name, "ON"))


def _replace_directory(staging: Path, destination: Path) -> None:
    if destination.is_symlink() or destination.is_file():
        destination.unlink()
    elif destination.exists():
        shutil.rmtree(destination)
    staging.rename(destination)


def stage_builtin_sources(project_root: Path, build_dir: Path) -> int:
    """Writes exactly the builtin catalog's files to <build>/node-lib."""
    entries = builtin_catalog.collect_builtin_entries(project_root)
    destination = build_dir / "node-lib"
    staging = build_dir / "node-lib.staging"
    if staging.exists():
        shutil.rmtree(staging)
    staging.mkdir(parents=True)

    written: set[Path] = set()
    for entry in entries:
        relative = Path(str(entry["relative_path"]))
        if relative.is_absolute() or ".." in relative.parts:
            raise StageError(f"unsafe builtin path: {relative}")
        root = staging / "internal" / "deps" if entry["is_internal_dep"] else staging
        target = root / relative
        if target in written:
            raise StageError(f"two builtin entries stage to {target}")
        written.add(target)
        target.parent.mkdir(parents=True, exist_ok=True)
        # Write the exact text the embedded catalog would contain (and that the
        # digest covers), not a byte copy, so both build modes serve the same
        # source. This also never produces a symlink.
        target.write_bytes(str(entry["source"]).encode("utf-8"))

    digest = builtin_catalog.catalog_digest(entries)
    (staging / CATALOG_MARKER).write_text(digest + "\n", encoding="utf-8")
    _replace_directory(staging, destination)
    return len(entries)


def stage_icu_data(project_root: Path, build_dir: Path) -> int:
    """Writes ICU's individual-item tree to <build>/icu/icudt78l."""
    package = project_root / ICU_DATA_PACKAGE
    try:
        data = bz2.decompress(package.read_bytes())
    except OSError as error:
        raise StageError(f"unable to read ICU data package {package}: {error}") from error
    destination = build_dir / "icu"
    try:
        count = extract_icu_data.extract(data, destination, expect_count=ICU_EXPECTED_ITEMS)
    except ValueError as error:
        raise StageError(f"ICU data extraction failed: {error}") from error
    if not (destination / ICU_DATA_NAME / "res_index.res").is_file():
        raise StageError(f"ICU data tree has no {ICU_DATA_NAME}/res_index.res")
    return count


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--project-root", required=True, type=Path)
    parser.add_argument("--build-dir", required=True, type=Path)
    args = parser.parse_args(argv)

    project_root = args.project_root.resolve()
    build_dir = args.build_dir.resolve()
    try:
        settings = read_cmake_cache(build_dir / "CMakeCache.txt")
        if option_enabled(settings, "EDGE_EMBED_BUILTIN_SOURCES"):
            print("builtin JS sources are embedded; not staging node-lib")
        else:
            count = stage_builtin_sources(project_root, build_dir)
            print(f"staged {count} builtin JS sources to {build_dir / 'node-lib'}")
        if option_enabled(settings, "EDGE_EMBED_ICU_DATA"):
            print("ICU data is embedded; not staging icu")
        else:
            count = stage_icu_data(project_root, build_dir)
            print(f"staged {count} ICU items to {build_dir / 'icu' / ICU_DATA_NAME}")
    except StageError as error:
        print(f"error: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
