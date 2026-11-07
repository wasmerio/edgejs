#!/usr/bin/env python3
"""Validate the engine-free EdgeJS WASIX artifact and its N-API ABI.

Also guards the size of static data in WASIX artifacts built without embedded
builtin JS and ICU data (see scripts/stage_wasix_static_data.py).
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass
from pathlib import Path
import re
import sys


NAPI_MODULE = "napi"
EXTENSION_MODULE = "napi_extension_wasmer_v0"
ENGINE_SENTINELS = (
    b"JS_NewRuntime",
    b"JS_NewContext",
    b"JS_EvalFunction",
    b"quickjs.c",
    b"libquickjs",
    b"_ZN2v8",
    b"V8::InitializePlatform",
    b"libv8_monolith",
)
DATA_SECTION_ID = 11
MEMORY_SECTION_ID = 5
IMPORT_SECTION_ID = 2
WASM_PAGE_SIZE = 64 * 1024
# Builds that keep builtin JS and ICU data in the package filesystem
# (EDGE_EMBED_BUILTIN_SOURCES=OFF, EDGE_EMBED_ICU_DATA=OFF) must stay well
# below the ~45 MB of passive data segments the embedded builds carry, because
# wasm copies every data segment into each instance's linear memory.
MAX_DATA_SECTION_BYTES = 4 * 1024 * 1024
MAX_INITIAL_MEMORY_PAGES = 200
STATIC_DATA_OPTIONS = ("EDGE_EMBED_BUILTIN_SOURCES", "EDGE_EMBED_ICU_DATA")
_CMAKE_FALSE = {"", "0", "OFF", "NO", "FALSE", "N", "IGNORE", "NOTFOUND"}


class ValidationError(ValueError):
    pass


@dataclass(frozen=True)
class WasmImport:
    module: str
    name: str
    kind: int
    minimum: int | None = None  # initial pages, for memory imports


@dataclass(frozen=True)
class StaticDataStats:
    data_section_bytes: int
    initial_memory_pages: int | None


class Reader:
    def __init__(self, data: bytes):
        self.data = data
        self.offset = 0

    def read_byte(self) -> int:
        if self.offset >= len(self.data):
            raise ValidationError("unexpected end of WebAssembly data")
        value = self.data[self.offset]
        self.offset += 1
        return value

    def read_bytes(self, length: int) -> bytes:
        end = self.offset + length
        if end > len(self.data):
            raise ValidationError("unexpected end of WebAssembly data")
        value = self.data[self.offset:end]
        self.offset = end
        return value

    def read_uleb(self) -> int:
        value = 0
        shift = 0
        for _ in range(10):
            byte = self.read_byte()
            value |= (byte & 0x7F) << shift
            if byte & 0x80 == 0:
                return value
            shift += 7
        raise ValidationError("invalid unsigned LEB128 value")

    def read_name(self) -> str:
        raw = self.read_bytes(self.read_uleb())
        try:
            return raw.decode("utf-8")
        except UnicodeDecodeError as error:
            raise ValidationError("invalid UTF-8 import name") from error


def read_limits(reader: Reader) -> int:
    """Reads a limits entry and returns its minimum."""
    flags = reader.read_uleb()
    minimum = reader.read_uleb()
    if flags & 0x01:  # has maximum
        reader.read_uleb()
    if flags & 0x08:  # custom page size
        reader.read_uleb()
    return minimum


def skip_limits(reader: Reader) -> None:
    read_limits(reader)


def parse_import_section(payload: bytes) -> list[WasmImport]:
    reader = Reader(payload)
    imports: list[WasmImport] = []
    for _ in range(reader.read_uleb()):
        module = reader.read_name()
        name = reader.read_name()
        kind = reader.read_byte()
        minimum = None
        if kind == 0:  # function
            reader.read_uleb()
        elif kind == 1:  # table
            reader.read_byte()
            skip_limits(reader)
        elif kind == 2:  # memory
            minimum = read_limits(reader)
        elif kind == 3:  # global
            reader.read_byte()
            reader.read_byte()
        elif kind == 4:  # exception tag
            reader.read_byte()
            reader.read_uleb()
        else:
            raise ValidationError(f"unsupported import kind {kind}")
        imports.append(WasmImport(module, name, kind, minimum))
    if reader.offset != len(payload):
        raise ValidationError("trailing bytes in WebAssembly import section")
    return imports


def iter_sections(data: bytes):
    if not data.startswith(b"\x00asm\x01\x00\x00\x00"):
        raise ValidationError("not a WebAssembly 1.0 module")
    reader = Reader(data[8:])
    while reader.offset < len(reader.data):
        section_id = reader.read_byte()
        yield section_id, reader.read_bytes(reader.read_uleb())


def parse_imports(data: bytes) -> list[WasmImport]:
    imports: list[WasmImport] = []
    for section_id, payload in iter_sections(data):
        if section_id == IMPORT_SECTION_ID:
            imports.extend(parse_import_section(payload))
    return imports


def measure_static_data(data: bytes) -> StaticDataStats:
    """Returns the data section size and the initial size of memory 0."""
    data_section_bytes = 0
    memory_minimums: list[int] = []
    for section_id, payload in iter_sections(data):
        if section_id == DATA_SECTION_ID:
            data_section_bytes += len(payload)
        elif section_id == IMPORT_SECTION_ID:
            memory_minimums.extend(
                item.minimum
                for item in parse_import_section(payload)
                if item.kind == 2 and item.minimum is not None
            )
        elif section_id == MEMORY_SECTION_ID:
            reader = Reader(payload)
            for _ in range(reader.read_uleb()):
                memory_minimums.append(read_limits(reader))
    return StaticDataStats(
        data_section_bytes,
        memory_minimums[0] if memory_minimums else None,
    )


def validate_static_data(
    data: bytes,
    max_data_section_bytes: int = MAX_DATA_SECTION_BYTES,
    max_initial_memory_pages: int = MAX_INITIAL_MEMORY_PAGES,
) -> StaticDataStats:
    stats = measure_static_data(data)
    if stats.data_section_bytes >= max_data_section_bytes:
        raise ValidationError(
            f"data section is {stats.data_section_bytes} bytes, expected under "
            f"{max_data_section_bytes}; builtin JS or ICU data may be embedded again"
        )
    if stats.initial_memory_pages is None:
        raise ValidationError("artifact declares no linear memory")
    if stats.initial_memory_pages > max_initial_memory_pages:
        raise ValidationError(
            f"initial memory is {stats.initial_memory_pages} pages "
            f"({stats.initial_memory_pages * WASM_PAGE_SIZE} bytes), expected at most "
            f"{max_initial_memory_pages}"
        )
    return stats


def is_napi_name(name: str) -> bool:
    return name.startswith(("napi_", "node_api_"))


def read_cmake_cache(path: Path) -> dict[str, str]:
    """Returns cache entries keyed as NAME:TYPE."""
    try:
        lines = path.read_text(encoding="utf-8").splitlines()
    except OSError as error:
        raise ValidationError(f"unable to read CMake cache {path}: {error}") from error
    return {
        key: value
        for line in lines
        if not line.startswith(("#", "//")) and "=" in line
        for key, value in (line.split("=", 1),)
    }


def cmake_bool(value: str) -> bool:
    upper = value.strip().upper()
    return not (upper in _CMAKE_FALSE or upper.endswith("-NOTFOUND"))


def static_data_external(settings: dict[str, str]) -> bool:
    """True when the cache turns every static-data embedding option OFF.

    Options missing from the cache take their CMake default (ON).
    """
    return all(
        not cmake_bool(settings.get(f"{option}:BOOL", "ON"))
        for option in STATIC_DATA_OPTIONS
    )


def validate_cmake_cache(path: Path) -> None:
    settings = read_cmake_cache(path)
    provider = settings.get("EDGE_NAPI_PROVIDER:STRING")
    if provider != "imports":
        raise ValidationError(
            f"CMake cache selected EDGE_NAPI_PROVIDER={provider!r}, expected 'imports'"
        )


def read_provider_operations(napi_root: Path) -> tuple[set[str], set[str]]:
    provider_sets: list[tuple[set[str], set[str]]] = []
    for relative in ("src/guest/napi.rs", "src/guest/napi_js.rs"):
        path = napi_root / relative
        try:
            source = path.read_text(encoding="utf-8")
        except OSError as error:
            raise ValidationError(f"unable to read provider source {path}: {error}") from error
        operation_sets: list[set[str]] = []
        for namespace_name, pattern in (
            ("napi_namespace", r'"((?:napi_|node_api_)[^"]+)"\s*=>'),
            ("napi_extension_wasmer_namespace", r'"(unofficial_napi_[^"]+)"\s*=>'),
        ):
            match = re.search(
                rf"let {namespace_name} = namespace!\s*\{{(.*?)\n\s*\}};",
                source,
                re.DOTALL,
            )
            if match is None:
                raise ValidationError(f"provider {namespace_name} not found in {path}")
            operations = set(re.findall(pattern, match.group(1)))
            if not operations:
                raise ValidationError(f"provider {namespace_name} is empty in {path}")
            operation_sets.append(operations)
        provider_sets.append((operation_sets[0], operation_sets[1]))
    if provider_sets[0] != provider_sets[1]:
        raise ValidationError(
            "native and host-JavaScript provider operation inventories differ"
        )
    return provider_sets[0]


def read_provider_extension_operations(napi_root: Path) -> set[str]:
    return read_provider_operations(napi_root)[1]


def validate_provider_operations(imports: list[WasmImport], operations: set[str]) -> None:
    required = {
        item.name
        for item in imports
        if item.kind == 0 and item.module == EXTENSION_MODULE
    }
    missing = sorted(required - operations)
    if missing:
        raise ValidationError(
            "Wasmer provider does not implement Edge extension imports: "
            + ", ".join(missing[:12])
        )


def validate_provider_imports(
    imports: list[WasmImport],
    standard_operations: set[str],
    extension_operations: set[str],
) -> None:
    required_standard = {
        item.name
        for item in imports
        if item.kind == 0 and item.module == NAPI_MODULE and is_napi_name(item.name)
    }
    missing_standard = sorted(required_standard - standard_operations)
    if missing_standard:
        raise ValidationError(
            "Wasmer provider does not implement Edge standard imports: "
            + ", ".join(missing_standard[:12])
        )
    validate_provider_operations(imports, extension_operations)


def validate_artifact(data: bytes) -> list[WasmImport]:
    imports = parse_imports(data)
    function_imports = [item for item in imports if item.kind == 0]
    napi_imports = [
        item
        for item in function_imports
        if item.module == NAPI_MODULE and is_napi_name(item.name)
    ]
    extension_imports = [
        item
        for item in function_imports
        if item.module == EXTENSION_MODULE
        and item.name.startswith("unofficial_napi_")
    ]
    if not napi_imports:
        raise ValidationError("artifact has no standard function imports from module 'napi'")
    if not extension_imports:
        raise ValidationError(
            "artifact has no EdgeJS function imports from module "
            "'napi_extension_wasmer_v0'"
        )

    misplaced = [
        item
        for item in function_imports
        if (
            is_napi_name(item.name) and item.module != NAPI_MODULE
        ) or (
            item.name.startswith("unofficial_napi_")
            and item.module != EXTENSION_MODULE
        )
    ]
    if misplaced:
        details = ", ".join(f"{item.module}.{item.name}" for item in misplaced[:8])
        raise ValidationError(f"N-API imports use the wrong module: {details}")

    found_sentinels = [token.decode("ascii") for token in ENGINE_SENTINELS if token in data]
    if found_sentinels:
        raise ValidationError(
            "artifact contains embedded JavaScript engine symbols: "
            + ", ".join(found_sentinels)
        )
    return imports


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("wasm", type=Path)
    parser.add_argument("--cmake-cache", type=Path)
    parser.add_argument(
        "--provider-napi-root",
        type=Path,
        help="N-API source tree used by the Wasmer provider",
    )
    parser.add_argument("--print-imports", action="store_true")
    parser.add_argument(
        "--static-data-only",
        action="store_true",
        help=(
            "only run the static-data size guard (for artifacts that embed an "
            "engine, such as the QuickJS WASIX build)"
        ),
    )
    parser.add_argument(
        "--enforce-static-data-limits",
        action="store_true",
        help=(
            "always run the static-data size guard; by default it runs when "
            "--cmake-cache has EDGE_EMBED_BUILTIN_SOURCES and EDGE_EMBED_ICU_DATA OFF"
        ),
    )
    args = parser.parse_args()
    if args.static_data_only and not (args.cmake_cache or args.enforce_static_data_limits):
        parser.error("--static-data-only needs --cmake-cache or --enforce-static-data-limits")
    try:
        enforce_static_data = args.enforce_static_data_limits
        if args.cmake_cache:
            if not args.static_data_only:
                validate_cmake_cache(args.cmake_cache)
            enforce_static_data = enforce_static_data or static_data_external(
                read_cmake_cache(args.cmake_cache)
            )
        data = args.wasm.read_bytes()
        stats = validate_static_data(data) if enforce_static_data else None
        if not args.static_data_only:
            imports = validate_artifact(data)
            if args.provider_napi_root:
                standard, extension = read_provider_operations(args.provider_napi_root)
                validate_provider_imports(imports, standard, extension)
    except (OSError, ValidationError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1

    if stats is not None:
        print(
            f"validated static data: {stats.data_section_bytes} data-section bytes "
            f"(limit {MAX_DATA_SECTION_BYTES}), {stats.initial_memory_pages} initial "
            f"memory pages (limit {MAX_INITIAL_MEMORY_PAGES})"
        )
    elif args.static_data_only:
        print("static-data size guard skipped: the build embeds its static data")
    if args.static_data_only:
        return 0

    if args.print_imports:
        for item in imports:
            print(f"{item.module}.{item.name}")
    napi_count = sum(
        item.kind == 0 and item.module == NAPI_MODULE and is_napi_name(item.name)
        for item in imports
    )
    extension_count = sum(
        item.kind == 0
        and item.module == EXTENSION_MODULE
        and item.name.startswith("unofficial_napi_")
        for item in imports
    )
    print(
        f"validated engine-free EdgeJS wasm: {napi_count} N-API imports, "
        f"{extension_count} extension imports"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
