"""Focused tests for the release boundaries, without registry writes."""

import importlib.util
import json
import os
from pathlib import Path
import re
import tempfile
import unittest
from unittest import mock
import zipfile


spec = importlib.util.spec_from_file_location("release", Path(__file__).with_name("release.py"))
release = importlib.util.module_from_spec(spec)
spec.loader.exec_module(release)

MANIFEST = '''[package]
name = "legacy/edgejs"
version = "0.1.24"
entrypoint = "edge"
[[module]]
name = "edge"
source = "./bin/edge"
[[command]]
name = "edge"
module = "edge"
[fs]
"/node-lib" = "./node-lib"
"/pnpm" = "./pnpm"
"/usr/share/edge-icu" = "./icu"
[dependencies]
"wasmer/bash" = "1.0.25"
'''
CATALOG = ("0123456789abcdef" * 4 + "\n").encode()
NATIVE = b"test wasm or native executable"


def uleb(value):
    out = bytearray()
    while True:
        byte = value & 0x7F
        value >>= 7
        out.append(byte | (0x80 if value else 0))
        if not value:
            return bytes(out)


def wasm(data_bytes=1024):
    """A module with a type section and a data section of the given payload size."""
    type_section = bytes([1]) + uleb(1) + b"\0"
    return b"\0asm\x01\0\0\0" + type_section + bytes([11]) + uleb(data_bytes) + b"\0" * data_bytes


WASIX_FILES = {
    "node-lib/.edge-builtin-catalog": CATALOG,
    "node-lib/internal/bootstrap/realm.js": b"'use strict';",
    "icu/icudt78l/res_index.res": b"res",
    "pnpm/edge-pnpm.cjs": b"bundle",
}


class ReleaseTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.directory = Path(self.temporary.name)

    def archives(self, manifest=MANIFEST, edge=None, skip=()):
        for name in release.ASSETS:
            with zipfile.ZipFile(self.directory / name, "w") as archive:
                if name not in release.PACKAGES:
                    archive.writestr("bin/edge", NATIVE)
                    continue
                archive.writestr("bin/edge", wasm() if edge is None else edge)
                archive.writestr("wasmer.toml", manifest)
                for member, content in WASIX_FILES.items():
                    if member not in skip:
                        archive.writestr(member, content)

    def dist(self):
        dist = self.directory / "dist"
        for member, content in {"bin/edge": wasm(), "wasmer.toml": MANIFEST.encode(), **WASIX_FILES}.items():
            (dist / member).parent.mkdir(parents=True, exist_ok=True)
            (dist / member).write_bytes(content)
        return dist

    def packages(self):
        for name in release.PACKAGES.values():
            package = self.directory / name
            package.mkdir()
            (package / "wasmer.toml").write_text(release.package_manifest(MANIFEST, name, "0.2.6"))

    def test_prepare_keeps_guest_and_dependency_and_updates_only_package(self):
        self.archives()
        release.prepare(self.directory, "0.2.6")
        for archive, name in release.PACKAGES.items():
            with zipfile.ZipFile(self.directory / archive) as source:
                data = release.tomllib.loads(source.read("wasmer.toml").decode())
                self.assertEqual(data["package"]["name"], f"wasmer/{name}")
                self.assertEqual(data["package"]["version"], "0.2.6")
                self.assertEqual(data["dependencies"]["wasmer/bash"], "1.0.25")
                self.assertEqual(data["fs"]["/node-lib"], "./node-lib")
                self.assertEqual(data["fs"]["/usr/share/edge-icu"], "./icu")
                self.assertEqual(source.read("bin/edge"), wasm())
                self.assertEqual(source.read(release.CATALOG_MEMBER), CATALOG)
            self.assertTrue((self.directory / "packages" / name / "bin/edge").is_file())
        sums = (self.directory / "SHA256SUMS").read_text().splitlines()
        self.assertEqual(len(sums), 6)
        for line in sums:
            checksum, name = line.split("  ")
            self.assertEqual(checksum, release.digest(self.directory / name))

    def test_missing_asset_fails(self):
        self.archives()
        (self.directory / release.ASSETS[0]).unlink()
        with self.assertRaisesRegex(ValueError, "six archives"):
            release.prepare(self.directory, "0.2.6")

    def test_traversal_fails(self):
        self.archives()
        with zipfile.ZipFile(self.directory / release.ASSETS[0], "a") as source:
            source.writestr("../outside", "unsafe")
        with self.assertRaisesRegex(ValueError, "Unsafe ZIP"):
            release.prepare(self.directory, "0.2.6")

    def test_noncanonical_source_fails(self):
        with self.assertRaisesRegex(ValueError, "bin/edge"):
            release.package_manifest(MANIFEST.replace("./bin/edge", "../build-wasix/edgejs.wasm"), "edgejs", "0.2.6")

    def test_missing_static_data_mapping_fails(self):
        for guest in release.REQUIRED_FS:
            manifest = "\n".join(line for line in MANIFEST.splitlines() if not line.startswith(f'"{guest}"'))
            with self.assertRaisesRegex(ValueError, f"must map {guest}"):
                release.package_manifest(manifest, "edgejs", "0.2.6")

    def test_noncanonical_static_data_source_fails(self):
        for old, new in (('"./node-lib"', '"./build-wasix/node-lib"'),
                         ('"./icu"', '"./build-quickjs-wasix/icu"'),
                         ('"./icu"', '"./icu/"')):
            with self.assertRaisesRegex(ValueError, "must map"):
                release.package_manifest(MANIFEST.replace(old, new), "edgejs", "0.2.6")

    def test_missing_static_data_directory_fails(self):
        for member in release.REQUIRED_MEMBERS:
            for path in self.directory.glob("*.zip"):
                path.unlink()
            self.archives(skip=(member,))
            with self.assertRaisesRegex(ValueError, f"missing {re.escape(member)}"):
                release.prepare(self.directory, "0.2.6")

    def test_malformed_catalog_digest_fails(self):
        self.archives()
        for filename in release.PACKAGES:
            with zipfile.ZipFile(self.directory / filename, "w") as archive:
                archive.writestr("bin/edge", wasm())
                archive.writestr("wasmer.toml", MANIFEST)
                for member, content in WASIX_FILES.items():
                    archive.writestr(member, b"" if member == release.CATALOG_MEMBER else content)
        with self.assertRaisesRegex(ValueError, "sha256"):
            release.prepare(self.directory, "0.2.6")

    def test_oversize_data_section_fails(self):
        self.archives(edge=wasm(release.MAX_DATA_SECTION_BYTES))
        with self.assertRaisesRegex(ValueError, "data section"):
            release.prepare(self.directory, "0.2.6")

    def test_data_section_just_under_limit_passes(self):
        self.archives(edge=wasm(release.MAX_DATA_SECTION_BYTES - 1))
        release.prepare(self.directory, "0.2.6")

    def test_non_wasm_wasix_edge_fails(self):
        self.archives(edge=NATIVE)
        with self.assertRaisesRegex(ValueError, "WebAssembly"):
            release.prepare(self.directory, "0.2.6")

    def test_data_section_size_is_summed_and_other_sections_ignored(self):
        module = wasm(10) + bytes([11]) + uleb(5) + b"\0" * 5 + bytes([0]) + uleb(3) + b"abc"
        self.assertEqual(release.wasm_data_section_bytes(module), 15)
        with self.assertRaisesRegex(ValueError, "Truncated"):
            release.wasm_data_section_bytes(wasm(10)[:-1])

    def test_check_dist(self):
        dist = self.dist()
        release.check_dist(dist)
        (dist / "icu/icudt78l/res_index.res").unlink()
        with self.assertRaisesRegex(ValueError, "missing icu/icudt78l/res_index.res"):
            release.check_dist(dist)
        (dist / "icu/icudt78l/res_index.res").write_bytes(b"res")
        (dist / "node-lib/link.js").symlink_to(dist / "node-lib/internal/bootstrap/realm.js")
        with self.assertRaisesRegex(ValueError, "symlinks in node-lib"):
            release.check_dist(dist)
        (dist / "node-lib/link.js").unlink()
        (dist / "wasmer.toml").write_text(MANIFEST.replace('"./icu"', '"./build-wasix/icu"'))
        with self.assertRaisesRegex(ValueError, "must map /usr/share/edge-icu"):
            release.check_dist(dist)
        (dist / "wasmer.toml").write_text(MANIFEST)
        (dist / "bin/edge").write_bytes(wasm(release.MAX_DATA_SECTION_BYTES + 1))
        with self.assertRaisesRegex(ValueError, "data section"):
            release.check_dist(dist)

    def test_release_source_must_match_both_manifests(self):
        (self.directory / "src").mkdir()
        (self.directory / "quickjs-wasm").mkdir()
        (self.directory / "src/edge_version.h").write_text(
            "#define EDGE_MAJOR_VERSION 0\n#define EDGE_MINOR_VERSION 2\n#define EDGE_PATCH_VERSION 6\n")
        (self.directory / "VERSION.txt").write_text("0.2.6\n")
        (self.directory / ".release-please-manifest.json").write_text(json.dumps({".": "0.2.6"}))
        for name in ("wasmer.toml", "quickjs-wasm/wasmer.toml"):
            (self.directory / name).write_text('[package]\nversion = "0.2.6"\n')
        self.assertEqual(release.source_version("v0.2.6", self.directory), "0.2.6")
        (self.directory / "quickjs-wasm/wasmer.toml").write_text('[package]\nversion = "0.2.0"\n')
        with self.assertRaisesRegex(ValueError, "quickjs-wasm"):
            release.source_version("v0.2.6", self.directory)

    def test_prerelease_and_invalid_tag_fail(self):
        for version in ("0.2.6-g123abc", "00.2.6", "v0.2.6", "0.2"):
            with self.assertRaises(ValueError):
                release.stable_version(version)

    @mock.patch.dict(os.environ, {"WASMER_TOKEN": "test credential"})
    @mock.patch.object(release, "existing_version", return_value=False)
    @mock.patch.object(release.subprocess, "run")
    def test_publish_uses_only_verified_version_and_registry(self, run, exists):
        self.packages()
        release.publish(self.directory, "wasmer.wtf", "0.2.6")
        self.assertEqual(run.call_count, 2)
        for call in run.call_args_list:
            command = call.args[0]
            self.assertEqual(command[:2], ["wasmer", "publish"])
            self.assertEqual(command[4], "wasmer.wtf")
            self.assertEqual(command[6], "0.2.6")
            self.assertNotIn("test credential", command)

    @mock.patch.dict(os.environ, {"WASMER_TOKEN": "test credential"})
    @mock.patch.object(release, "existing_version", return_value=True)
    @mock.patch.object(release, "container_contents", side_effect=[{"edge": "same"}, {"edge": "different"}])
    @mock.patch.object(release.subprocess, "run")
    def test_existing_different_package_fails_before_publish(self, run, contents, exists):
        self.packages()
        with self.assertRaisesRegex(ValueError, "different files"):
            release.publish(self.directory, "wasmer.io", "0.2.6")
        self.assertFalse(any(call.args[0][1] == "publish" for call in run.call_args_list))

    @mock.patch.dict(os.environ, {"WASMER_TOKEN": "test credential"})
    @mock.patch.object(release, "existing_version", return_value=True)
    @mock.patch.object(release, "container_contents", return_value={"edge": "same"})
    @mock.patch.object(release.subprocess, "run")
    def test_existing_identical_package_skips_publish(self, run, contents, exists):
        self.packages()
        release.publish(self.directory, "wasmer.io", "0.2.6")
        self.assertFalse(any(call.args[0][1] == "publish" for call in run.call_args_list))


if __name__ == "__main__":
    unittest.main()
