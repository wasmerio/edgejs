#!/usr/bin/env python3

from __future__ import annotations

import pathlib
import re
import sys


DECL_RE = re.compile(r"NAPI_EXTERN\s+[^;]*?\b([A-Za-z_][A-Za-z0-9_]*)\s*\(", re.DOTALL)


def collect_declared_symbols(headers: list[pathlib.Path]) -> set[str]:
    symbols: set[str] = set()
    for header in headers:
        text = header.read_text()
        for name in DECL_RE.findall(text):
            if name in {"NAPI_EXTERN", "NAPI_CDECL"} or name.startswith("__"):
                continue
            symbols.add(name)
    return symbols


def collect_defined_text(sources: list[pathlib.Path]) -> str:
    return "\n".join(source.read_text() for source in sources)


def main() -> int:
    if len(sys.argv) < 5:
        print("usage: check_symbol_coverage.py <headers...> -- <sources...>", file=sys.stderr)
        return 2

    try:
        sep = sys.argv.index("--")
    except ValueError:
        print("missing -- separator", file=sys.stderr)
        return 2

    header_paths = [pathlib.Path(p) for p in sys.argv[1:sep]]
    source_paths = [pathlib.Path(p) for p in sys.argv[sep + 1 :]]
    declared = collect_declared_symbols(header_paths)
    defined_text = collect_defined_text(source_paths)

    missing = sorted(
        name for name in declared if re.search(rf"\b{name}\s*\(", defined_text) is None
    )
    if missing:
        print("Missing JSC provider symbols:", file=sys.stderr)
        for name in missing:
            print(f"  {name}", file=sys.stderr)
        return 1

    print(f"Symbol coverage OK: {len(declared)} declarations matched.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
