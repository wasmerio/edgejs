#!/usr/bin/env python3
"""Unpack an ICU common-data package into ICU's individual-file layout.

Builds that set EDGE_EMBED_ICU_DATA=OFF do not link the ICU common data into
the binary. Instead the package ships every item as a file, and ICU opens only
the items it needs from u_getDataDirectory():

    <output>/icudt78l/coll/de.res
    <output>/icudt78l/ibm-5348_P100-1997.cnv
    ...

The input is the same icudtNNl.dat(.bz2) that cmake/embed_binary.py embeds, so
both build modes serve identical data.
"""

from __future__ import annotations

import argparse
import bz2
import re
import shutil
import struct
import sys
from pathlib import Path, PurePosixPath

# DataHeader magic bytes and the little-endian "CmnD" common-data format.
_MAGIC = (0xDA, 0x27)
_COMMON_DATA_FORMAT = b"CmnD"
# Top-level entries of an extracted tree, such as icudt78l.
_ICU_TREE_NAME = re.compile(r"^icudt[0-9]+[lbe]$")


def read_toc(data: bytes) -> list[tuple[str, int, int]]:
    """Returns (item name, start, end) for every item in a common-data package."""
    header_size, magic1, magic2 = struct.unpack_from("<HBB", data, 0)
    if (magic1, magic2) != _MAGIC:
        raise ValueError("not an ICU data file (bad magic)")
    # UDataInfo follows the 4-byte MappedData prefix: size, reserved,
    # isBigEndian, charsetFamily, sizeofUChar, reserved, dataFormat[4], ...
    is_big_endian = data[4 + 4]
    if is_big_endian:
        raise ValueError("big-endian ICU data is not supported")
    data_format = data[4 + 8 : 4 + 12]
    if data_format != _COMMON_DATA_FORMAT:
        raise ValueError(f"not an ICU common-data package (format {data_format!r})")

    toc = header_size
    (count,) = struct.unpack_from("<I", data, toc)
    entries = [struct.unpack_from("<II", data, toc + 4 + 8 * i) for i in range(count)]
    # An item ends where the next item (by data offset) starts; do not assume
    # the TOC lists the items in data order.
    starts = sorted({toc + data_offset for _, data_offset in entries})
    next_start = {start: end for start, end in zip(starts, starts[1:] + [len(data)])}
    items: list[tuple[str, int, int]] = []
    for name_offset, data_offset in entries:
        name_start = toc + name_offset
        name = data[name_start : data.index(b"\0", name_start)].decode("ascii")
        start = toc + data_offset
        items.append((name, start, next_start[start]))
    return items


def extract(data: bytes, output: Path, expect_count: int | None = None) -> int:
    items = read_toc(data)
    if expect_count is not None and len(items) != expect_count:
        raise ValueError(f"expected {expect_count} ICU items, found {len(items)}")
    destinations: list[tuple[Path, int, int]] = []
    for name, start, end in items:
        relative = PurePosixPath(name)
        if relative.is_absolute() or ".." in relative.parts or not relative.parts:
            raise ValueError(f"unsafe ICU item name: {name}")
        if relative.suffix == ".dat":
            # Packaged .dat files would make ICU prefer a common-data blob over
            # the individual items; this tree must only hold items.
            raise ValueError(f"unexpected common-data package in ICU data: {name}")
        destinations.append((output.joinpath(*relative.parts), start, end))

    # Start from an empty directory so items from an older ICU version or a
    # previous run never survive next to the current ones.
    if output.is_symlink() or output.is_file():
        raise ValueError(f"ICU output {output} exists and is not a directory")
    if output.exists():
        foreign = sorted(
            child.name for child in output.iterdir() if not _ICU_TREE_NAME.match(child.name)
        )
        if foreign:
            # Refuse to wipe a directory this script did not create.
            raise ValueError(
                f"ICU output {output} holds non-ICU entries ({', '.join(foreign[:5])}); "
                "remove it or choose another directory"
            )
        shutil.rmtree(output)
    output.mkdir(parents=True)
    for destination, start, end in destinations:
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_bytes(data[start:end])
    return len(items)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--compression", choices=("none", "bz2"), default="none")
    parser.add_argument(
        "--expect-count",
        type=int,
        help="fail unless the package holds exactly this many items",
    )
    args = parser.parse_args()

    data = args.input.read_bytes()
    if args.compression == "bz2":
        data = bz2.decompress(data)
    try:
        count = extract(data, args.output, expect_count=args.expect_count)
    except ValueError as error:
        print(f"error: {error}", file=sys.stderr)
        return 1
    print(f"extracted {count} ICU items to {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
