#!/usr/bin/env python3

from __future__ import annotations

import contextlib
import importlib.util
import io
from pathlib import Path
import sys
import tempfile
import unittest


MODULE_PATH = Path(__file__).with_name("validate-imported-napi-wasm.py")
SPEC = importlib.util.spec_from_file_location("validate_imported_napi_wasm", MODULE_PATH)
assert SPEC is not None and SPEC.loader is not None
validator = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = validator
SPEC.loader.exec_module(validator)


def uleb(value: int) -> bytes:
    output = bytearray()
    while True:
        byte = value & 0x7F
        value >>= 7
        output.append(byte | (0x80 if value else 0))
        if not value:
            return bytes(output)


def name(value: str) -> bytes:
    encoded = value.encode("utf-8")
    return uleb(len(encoded)) + encoded


def function_import(module: str, field: str) -> bytes:
    return name(module) + name(field) + b"\x00\x00"


def module(*imports: tuple[str, str], trailer: bytes = b"") -> bytes:
    payload = uleb(len(imports)) + b"".join(function_import(*item) for item in imports)
    return b"\x00asm\x01\x00\x00\x00" + b"\x02" + uleb(len(payload)) + payload + trailer


def section(section_id: int, payload: bytes) -> bytes:
    return bytes([section_id]) + uleb(len(payload)) + payload


def shared_memory_import(minimum: int, maximum: int = 65536) -> bytes:
    # Like WASIX's env.memory: shared limits with a maximum (flags 0x03).
    return name("env") + name("memory") + b"\x02\x03" + uleb(minimum) + uleb(maximum)


def passive_data_section(*segment_sizes: int) -> bytes:
    payload = uleb(len(segment_sizes)) + b"".join(
        b"\x01" + uleb(size) + b"\xAB" * size for size in segment_sizes
    )
    return section(11, payload)


def static_data_module(
    memory_pages: int | None,
    *segment_sizes: int,
    defined_memory: bool = False,
) -> bytes:
    imports = [
        function_import("napi", "napi_create_object"),
        function_import("napi_extension_wasmer_v0", "unofficial_napi_create_env"),
    ]
    if memory_pages is not None and not defined_memory:
        imports.append(shared_memory_import(memory_pages))
    output = b"\x00asm\x01\x00\x00\x00" + section(
        2, uleb(len(imports)) + b"".join(imports)
    )
    if memory_pages is not None and defined_memory:
        output += section(5, uleb(1) + b"\x00" + uleb(memory_pages))
    if segment_sizes:
        output += passive_data_section(*segment_sizes)
    return output


def cache_text(provider: str, builtin_sources: str | None, icu_data: str | None) -> str:
    lines = ["# This is the CMakeCache file.", f"EDGE_NAPI_PROVIDER:STRING={provider}"]
    if builtin_sources is not None:
        lines.append(f"EDGE_EMBED_BUILTIN_SOURCES:BOOL={builtin_sources}")
    if icu_data is not None:
        lines.append(f"EDGE_EMBED_ICU_DATA:BOOL={icu_data}")
    return "\n".join(lines) + "\n"


def run_main(*argv: str) -> tuple[int, str, str]:
    stdout = io.StringIO()
    stderr = io.StringIO()
    saved_argv = sys.argv
    sys.argv = ["validate-imported-napi-wasm.py", *argv]
    try:
        with contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr):
            try:
                code = validator.main()
            except SystemExit as error:
                code = error.code
    finally:
        sys.argv = saved_argv
    return code, stdout.getvalue(), stderr.getvalue()


class ValidateImportedNapiWasmTests(unittest.TestCase):
    def test_accepts_engine_free_import_contract(self) -> None:
        imports = validator.validate_artifact(
            module(
                ("wasi_snapshot_preview1", "fd_write"),
                ("napi", "napi_create_object"),
                ("napi", "node_api_create_syntax_error"),
                ("napi_extension_wasmer_v0", "unofficial_napi_create_env"),
            )
        )
        self.assertEqual(len(imports), 4)

    def test_rejects_missing_standard_napi_imports(self) -> None:
        with self.assertRaisesRegex(validator.ValidationError, "no standard"):
            validator.validate_artifact(
                module(("napi_extension_wasmer_v0", "unofficial_napi_create_env"))
            )

    def test_rejects_missing_extension_imports(self) -> None:
        with self.assertRaisesRegex(validator.ValidationError, "no EdgeJS"):
            validator.validate_artifact(module(("napi", "napi_create_object")))

    def test_rejects_napi_imported_from_env(self) -> None:
        with self.assertRaisesRegex(validator.ValidationError, "wrong module"):
            validator.validate_artifact(
                module(
                    ("napi", "napi_create_object"),
                    ("napi_extension_wasmer_v0", "unofficial_napi_create_env"),
                    ("env", "napi_get_global"),
                )
            )

    def test_rejects_embedded_engine_symbol(self) -> None:
        custom_payload = name("fixture") + b"JS_NewRuntime"
        custom_section = b"\x00" + uleb(len(custom_payload)) + custom_payload
        with self.assertRaisesRegex(validator.ValidationError, "engine symbols"):
            validator.validate_artifact(
                module(
                    ("napi", "napi_create_object"),
                    ("napi_extension_wasmer_v0", "unofficial_napi_create_env"),
                    trailer=custom_section,
                )
            )

    def test_rejects_non_wasm_input(self) -> None:
        with self.assertRaisesRegex(validator.ValidationError, "not a WebAssembly"):
            validator.validate_artifact(b"not wasm")

    def test_compares_native_and_host_js_provider_operations(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for relative in ("src/guest/napi.rs", "src/guest/napi_js.rs"):
                path = root / relative
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(
                    'let napi_namespace = namespace! {\n'
                    '  "napi_create_object" => provider,\n'
                    '};\n'
                    'let napi_extension_wasmer_namespace = namespace! {\n'
                    '  "unofficial_napi_create_env" => provider,\n'
                    '};\n',
                    encoding="utf-8",
                )
            standard, extension = validator.read_provider_operations(root)
            imports = validator.validate_artifact(
                module(
                    ("napi", "napi_create_object"),
                    ("napi_extension_wasmer_v0", "unofficial_napi_create_env"),
                )
            )
            validator.validate_provider_imports(imports, standard, extension)

    def test_rejects_provider_missing_an_edge_import(self) -> None:
        imports = validator.validate_artifact(
            module(
                ("napi", "napi_create_object"),
                ("napi_extension_wasmer_v0", "unofficial_napi_create_env"),
            )
        )
        with self.assertRaisesRegex(validator.ValidationError, "does not implement"):
            validator.validate_provider_operations(imports, set())


class StaticDataGuardTests(unittest.TestCase):
    def test_measures_data_section_and_imported_memory(self) -> None:
        stats = validator.measure_static_data(static_data_module(150, 1000, 24))
        # count byte + 2 x (flag byte + length byte + data)
        self.assertEqual(stats.data_section_bytes, 1 + (1 + 2 + 1000) + (1 + 1 + 24))
        self.assertEqual(stats.initial_memory_pages, 150)

    def test_measures_defined_memory(self) -> None:
        stats = validator.measure_static_data(
            static_data_module(42, 16, defined_memory=True)
        )
        self.assertEqual(stats.initial_memory_pages, 42)

    def test_accepts_externalized_static_data(self) -> None:
        stats = validator.validate_static_data(
            static_data_module(validator.MAX_INITIAL_MEMORY_PAGES, 600_000)
        )
        self.assertLess(stats.data_section_bytes, validator.MAX_DATA_SECTION_BYTES)

    def test_rejects_oversize_data_section(self) -> None:
        with self.assertRaisesRegex(validator.ValidationError, "data section is"):
            validator.validate_static_data(
                static_data_module(150, validator.MAX_DATA_SECTION_BYTES)
            )

    def test_rejects_large_initial_memory(self) -> None:
        with self.assertRaisesRegex(validator.ValidationError, "initial memory is 826 pages"):
            validator.validate_static_data(static_data_module(826, 1024))

    def test_rejects_module_without_memory(self) -> None:
        with self.assertRaisesRegex(validator.ValidationError, "no linear memory"):
            validator.validate_static_data(static_data_module(None, 1024))

    def test_static_data_external_needs_both_options_off(self) -> None:
        def settings(builtin: str | None, icu: str | None) -> dict[str, str]:
            with tempfile.TemporaryDirectory() as directory:
                path = Path(directory) / "CMakeCache.txt"
                path.write_text(cache_text("imports", builtin, icu), encoding="utf-8")
                return validator.read_cmake_cache(path)

        self.assertTrue(validator.static_data_external(settings("OFF", "OFF")))
        self.assertTrue(validator.static_data_external(settings("0", "FALSE")))
        self.assertFalse(validator.static_data_external(settings("OFF", "ON")))
        self.assertFalse(validator.static_data_external(settings("ON", "OFF")))
        # A cache from before the options existed means both embedded (ON).
        self.assertFalse(validator.static_data_external(settings(None, None)))

    def test_cli_enforces_guard_when_cache_turns_embedding_off(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            wasm = root / "edgejs.wasm"
            cache = root / "CMakeCache.txt"
            wasm.write_bytes(static_data_module(826, validator.MAX_DATA_SECTION_BYTES + 1))

            cache.write_text(cache_text("imports", "ON", "ON"), encoding="utf-8")
            code, stdout, _ = run_main("--cmake-cache", str(cache), str(wasm))
            self.assertEqual(code, 0)
            self.assertIn("validated engine-free", stdout)

            cache.write_text(cache_text("imports", "OFF", "OFF"), encoding="utf-8")
            code, _, stderr = run_main("--cmake-cache", str(cache), str(wasm))
            self.assertEqual(code, 1)
            self.assertIn("data section is", stderr)

            code, _, stderr = run_main("--enforce-static-data-limits", str(wasm))
            self.assertEqual(code, 1)
            self.assertIn("data section is", stderr)

    def test_cli_static_data_only_skips_napi_checks(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            cache = root / "CMakeCache.txt"
            cache.write_text(cache_text("quickjs", "OFF", "OFF"), encoding="utf-8")
            # An engine-embedding module with no N-API imports at all.
            small = root / "small.wasm"
            small.write_bytes(
                b"\x00asm\x01\x00\x00\x00"
                + section(2, uleb(1) + shared_memory_import(120))
                + passive_data_section(4096)
            )
            code, stdout, _ = run_main("--static-data-only", "--cmake-cache", str(cache), str(small))
            self.assertEqual(code, 0)
            self.assertIn("validated static data", stdout)

            large = root / "large.wasm"
            large.write_bytes(
                b"\x00asm\x01\x00\x00\x00"
                + section(2, uleb(1) + shared_memory_import(828))
                + passive_data_section(validator.MAX_DATA_SECTION_BYTES)
            )
            code, _, stderr = run_main("--static-data-only", "--cmake-cache", str(cache), str(large))
            self.assertEqual(code, 1)
            self.assertIn("data section is", stderr)

            cache.write_text(cache_text("quickjs", "ON", "ON"), encoding="utf-8")
            code, stdout, _ = run_main("--static-data-only", "--cmake-cache", str(cache), str(large))
            self.assertEqual(code, 0)
            self.assertIn("skipped", stdout)

    def test_cli_static_data_only_requires_a_trigger(self) -> None:
        code, _, stderr = run_main("--static-data-only", "missing.wasm")
        self.assertEqual(code, 2)
        self.assertIn("needs --cmake-cache", stderr)


if __name__ == "__main__":
    unittest.main()
