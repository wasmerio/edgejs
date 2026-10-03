"""Focused tests for the release boundaries, without registry writes."""

import importlib.util
import json
import os
from pathlib import Path
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
"/pnpm" = "./pnpm"
[dependencies]
"wasmer/bash" = "1.0.25"
'''


class ReleaseTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.directory = Path(self.temporary.name)

    def archives(self):
        for name in release.ASSETS:
            with zipfile.ZipFile(self.directory / name, "w") as archive:
                archive.writestr("bin/edge", b"test wasm or native executable")
                if name in release.PACKAGES:
                    archive.writestr("wasmer.toml", MANIFEST)
                    archive.writestr("pnpm/edge-pnpm.cjs", "bundle")

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
                self.assertEqual(source.read("bin/edge"), b"test wasm or native executable")
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
