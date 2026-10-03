#!/usr/bin/env python3
"""Validate stable release inputs, prepare archives, and publish exact packages.

Requires Python 3.11+. Registry credentials come only from WASMER_TOKEN.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import stat
import subprocess
import tempfile
import tomllib
import urllib.request
import zipfile


ROOT = Path(__file__).resolve().parents[2]
ASSETS = (
    "edge-linux-amd64.zip",
    "edge-darwin-arm64.zip",
    "edge-wasix.zip",
    "edge-quickjs-linux-amd64.zip",
    "edge-quickjs-darwin-arm64.zip",
    "edge-quickjs-wasix.zip",
)
PACKAGES = {"edge-wasix.zip": "edgejs", "edge-quickjs-wasix.zip": "edgejs-quickjs"}
REGISTRIES = {"wasmer.io": "https://registry.wasmer.io/graphql",
              "wasmer.wtf": "https://registry.wasmer.wtf/graphql"}


def stable_version(value):
    if not re.fullmatch(r"(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)", value):
        raise ValueError(f"Expected a stable semantic version, got {value!r}")
    return value


def source_version(tag, root=ROOT):
    if not tag.startswith("v"):
        raise ValueError("Release tag must start with v")
    version = stable_version(tag[1:])
    header = (root / "src/edge_version.h").read_text()
    parts = [re.search(rf"^#define EDGE_{part}_VERSION (\d+)$", header, re.M)
             for part in ("MAJOR", "MINOR", "PATCH")]
    if not all(parts) or ".".join(part[1] for part in parts) != version:
        raise ValueError("Release tag and C++ version header disagree")
    if (root / "VERSION.txt").read_text().strip() != version:
        raise ValueError("Release tag and VERSION.txt disagree")
    if json.loads((root / ".release-please-manifest.json").read_text())["."] != version:
        raise ValueError("Release tag and release-please manifest disagree")
    for path in ("wasmer.toml", "quickjs-wasm/wasmer.toml"):
        if tomllib.loads((root / path).read_text())["package"]["version"] != version:
            raise ValueError(f"Release tag and {path} disagree")
    return version


def safe_member(info):
    path = PurePosixPath(info.filename)
    mode = info.external_attr >> 16
    if path.is_absolute() or ".." in path.parts or "\\" in info.filename:
        raise ValueError(f"Unsafe ZIP path: {info.filename}")
    if stat.S_ISLNK(mode):
        raise ValueError(f"Symlink in release archive: {info.filename}")


def package_manifest(content, name, version):
    data = tomllib.loads(content)
    if data["package"].get("entrypoint") != "edge":
        raise ValueError("WASIX package must use the edge entrypoint")
    if len(data.get("module", [])) != 1 or data["module"][0]["name"] != "edge":
        raise ValueError("WASIX package must contain exactly one edge module")
    if data["module"][0]["source"] != "./bin/edge":
        raise ValueError("WASIX package must use ./bin/edge")
    for source in data.get("fs", {}).values():
        if PurePosixPath(source).is_absolute() or ".." in PurePosixPath(source).parts:
            raise ValueError(f"Package filesystem source escapes distribution: {source}")
    # Only rewrite release output metadata. Preserve all commands, annotations,
    # dependency versions, and filesystem layout from the built distribution.
    start = re.search(r"^\[package\]\s*$", content, re.M)
    if not start:
        raise ValueError("Missing package table")
    next_table = re.search(r"^\[", content[start.end():], re.M)
    end = start.end() + next_table.start() if next_table else len(content)
    package = content[start.end():end]
    for key, value in (("name", f"wasmer/{name}"), ("version", version)):
        package, count = re.subn(rf'^{key}\s*=\s*"[^"\n]*"',
                                 f'{key} = "{value}"', package, count=1, flags=re.M)
        if count != 1:
            raise ValueError(f"Missing package {key}")
    return content[:start.end()] + package + content[end:]


def prepare(assets, version):
    stable_version(version)
    found = {path.name for path in assets.glob("*.zip")}
    if found != set(ASSETS):
        raise ValueError(f"Expected exactly six archives; missing={set(ASSETS) - found}, extra={found - set(ASSETS)}")
    package_root = assets / "packages"
    if package_root.exists():
        raise ValueError("Package output already exists; use a fresh assets directory")
    for filename in ASSETS:
        archive = assets / filename
        with zipfile.ZipFile(archive) as source:
            infos = source.infolist()
            names = [info.filename for info in infos]
            if len(names) != len(set(names)):
                raise ValueError(f"Duplicate paths in {filename}")
            for info in infos:
                safe_member(info)
            if "bin/edge" not in names or "bin/edgejs" in names:
                raise ValueError(f"Noncanonical executable in {filename}")
            if source.testzip():
                raise ValueError(f"Corrupt release archive: {filename}")
            if filename in PACKAGES:
                name = PACKAGES[filename]
                manifest = package_manifest(source.read("wasmer.toml").decode(), name, version)
                temp = archive.with_suffix(".zip.tmp")
                with zipfile.ZipFile(temp, "w") as target:
                    for info in infos:
                        content = manifest.encode() if info.filename == "wasmer.toml" else source.read(info)
                        target.writestr(info, content)
        if filename in PACKAGES:
            temp.replace(archive)
            with zipfile.ZipFile(archive) as source:
                source.extractall(package_root / PACKAGES[filename])
    sums = "".join(f"{digest(assets / filename)}  {filename}\n" for filename in ASSETS)
    (assets / "SHA256SUMS").write_text(sums)


def digest(path):
    result = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            result.update(block)
    return result.hexdigest()


def existing_version(registry, name, version):
    query = "query($name: String!, $version: String!) { getPackageVersion(name: $name, version: $version) { version } }"
    request = urllib.request.Request(REGISTRIES[registry], method="POST", headers={
        "Content-Type": "application/json", "Authorization": f"Bearer {os.environ['WASMER_TOKEN']}"
    }, data=json.dumps({"query": query, "variables": {
        "name": f"wasmer/{name}", "version": version
    }}).encode())
    with urllib.request.urlopen(request, timeout=60) as response:
        result = json.load(response)
    if result.get("errors") or "data" not in result:
        raise ValueError("Registry version lookup failed; refusing to assume version is absent")
    return result["data"]["getPackageVersion"] is not None


def container_contents(path):
    # JSON key order and ZIP/container timestamps do not affect package content.
    return {str(file.relative_to(path)): (
        json.loads(file.read_text()) if file.relative_to(path) == Path("manifest.json") else digest(file)
    ) for file in path.rglob("*") if file.is_file()}


def publish(packages, registry, version):
    stable_version(version)
    if not os.environ.get("WASMER_TOKEN"):
        raise ValueError("WASMER_TOKEN is required for registry publication")
    for name in PACKAGES.values():
        package = packages / name
        metadata = tomllib.loads((package / "wasmer.toml").read_text())["package"]
        if metadata["name"] != f"wasmer/{name}" or metadata["version"] != version:
            raise ValueError(f"Unexpected publication metadata for {name}")
        if existing_version(registry, name, version):
            with tempfile.TemporaryDirectory(prefix="edge-release-") as directory:
                temporary = Path(directory)
                local, remote = temporary / "local.webc", temporary / "remote.webc"
                subprocess.run(["wasmer", "package", "build", str(package), "--out", str(local)], check=True)
                subprocess.run(["wasmer", "package", "download", f"wasmer/{name}@{version}",
                                "--registry", registry, "--validate", "--out-path", str(remote)], check=True)
                for webc in (local, remote):
                    subprocess.run(["wasmer", "package", "unpack", str(webc), "--format", "webc",
                                    "--out-dir", str(webc.with_suffix(""))], check=True)
                if container_contents(local.with_suffix("")) != container_contents(remote.with_suffix("")):
                    raise ValueError(f"{registry}/wasmer/{name}@{version} already contains different files")
            print(f"Already published identical package: {registry}/wasmer/{name}@{version}")
            continue
        subprocess.run(["wasmer", "publish", str(package), "--registry", registry,
                        "--version", version, "--non-interactive", "--wait=container"], check=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    version = commands.add_parser("version")
    version.add_argument("--tag", required=True)
    archives = commands.add_parser("prepare")
    archives.add_argument("--assets", type=Path, required=True)
    archives.add_argument("--version", required=True)
    registry = commands.add_parser("publish")
    registry.add_argument("--packages", type=Path, required=True)
    registry.add_argument("--registry", choices=REGISTRIES, required=True)
    registry.add_argument("--version", required=True)
    args = parser.parse_args()
    if args.command == "version":
        print(source_version(args.tag))
    elif args.command == "prepare":
        prepare(args.assets, args.version)
    else:
        publish(args.packages, args.registry, args.version)


if __name__ == "__main__":
    main()
