#!/usr/bin/env python3

import argparse
import json
import os
import subprocess
import tempfile
from pathlib import Path


def run(command: list[str], *, cwd: Path, timeout: int) -> subprocess.CompletedProcess[str]:
    completed = subprocess.run(
        command,
        cwd=cwd,
        capture_output=True,
        text=True,
        timeout=timeout,
        check=False,
    )
    if completed.returncode != 0:
        raise RuntimeError(
            f"command exited with {completed.returncode}: {' '.join(command)}\n"
            f"stdout:\n{completed.stdout}\n"
            f"stderr:\n{completed.stderr}"
        )
    return completed


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Verify the npm/pnpm aliases and delegated npm operations under WASIX."
    )
    parser.add_argument(
        "--wasmer-bin",
        default=os.environ.get("WASMER_BIN", "wasmer"),
        help="Path to the Wasmer CLI binary.",
    )
    parser.add_argument(
        "--package-dir",
        default=str(Path(__file__).resolve().parents[1] / "quickjs-wasm"),
        help="WASIX package directory.",
    )
    parser.add_argument("--timeout", type=int, default=60)
    args = parser.parse_args()

    package_dir = Path(args.package_dir).resolve()
    if not (package_dir / "wasmer.toml").is_file():
        raise RuntimeError(f"missing wasmer.toml in {package_dir}")

    with tempfile.TemporaryDirectory(prefix="edgejs-pnpm-smoke.") as temp_dir:
        project_dir = Path(temp_dir)
        (project_dir / "package.json").write_text(
            json.dumps(
                {
                    "name": "edge-pnpm-smoke",
                    "private": True,
                    "scripts": {"smoke": "node -p \"require('react').version\""},
                },
                indent=2,
            ) + "\n",
            encoding="utf-8",
        )

        base = [args.wasmer_bin, "run", "--stack-size", "4194304", str(package_dir)]
        # A project registry verifies the fallback reads npm configuration;
        # a naive npm -> pnpm alias would recurse until the command times out.
        registry = "https://alias-registry.invalid/"
        npmrc = project_dir / ".npmrc"
        npmrc.write_text(f"registry={registry}\n", encoding="utf-8")
        for command in ("npm", "pnpm"):
            version = run(
                base + [f"--command={command}", "--volume=.", "--", "--version"],
                cwd=project_dir,
                timeout=args.timeout,
            ).stdout.strip()
            if version != "10.34.5":
                raise RuntimeError(f"unexpected {command} version: {version!r}")

            configured_registry = run(
                base + [f"--command={command}", "--volume=.", "--", "config", "get", "registry"],
                cwd=project_dir,
                timeout=args.timeout,
            ).stdout.strip()
            if configured_registry != registry:
                raise RuntimeError(f"{command} did not read .npmrc: {configured_registry!r}")

            store_path = run(
                base + [f"--command={command}", "--volume=.", "--", "store", "path"],
                cwd=project_dir,
                timeout=args.timeout,
            ).stdout.strip()
            if store_path != "/tmp/.pnpm-store/v10":
                raise RuntimeError(f"unexpected {command} store path: {store_path!r}")
        npmrc.unlink()

        install = run(
            base
            + [
                "--command=npm",
                "--net",
                "--volume=.",
                "--",
                "install",
                "react@19.2.8",
            ],
            cwd=project_dir,
            timeout=args.timeout,
        )
        install_output = install.stdout + install.stderr
        if "Update available!" in install_output:
            raise RuntimeError(
                "pnpm's update notification was not disabled:\n" + install_output
            )

        manifest = json.loads((project_dir / "package.json").read_text(encoding="utf-8"))
        if manifest.get("dependencies", {}).get("react") != "19.2.8":
            raise RuntimeError(f"React was not persisted in package.json: {manifest!r}")
        if not (project_dir / "pnpm-lock.yaml").is_file():
            raise RuntimeError("pnpm-lock.yaml was not created")
        if not (project_dir / "node_modules" / "react" / "package.json").is_file():
            raise RuntimeError("hoisted node_modules/react package was not materialized")

        # The other public alias must consume the same lockfile and install.
        run(
            base + ["--command=pnpm", "--volume=.", "--", "install", "--offline", "--frozen-lockfile"],
            cwd=project_dir,
            timeout=args.timeout,
        )
        for command in ("npm", "pnpm"):
            script = run(
                base + [f"--command={command}", "--volume=.", "--", "run", "smoke"],
                cwd=project_dir,
                timeout=args.timeout,
            )
            if not script.stdout.strip().endswith("19.2.8"):
                raise RuntimeError(f"{command} could not run the package script: {script.stdout!r}")

        edge = run(
            base
            + [
                "--command=edge",
                "--volume=.",
                "--",
                "-e",
                "console.log(require('react').version)",
            ],
            cwd=project_dir,
            timeout=args.timeout,
        )
        if edge.stdout.strip() != "19.2.8":
            raise RuntimeError(f"Edge could not resolve installed React: {edge.stdout!r}")

    print("Bundled npm/pnpm WASIX smoke test passed (pnpm 10.34.5, React 19.2.8).")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (RuntimeError, subprocess.TimeoutExpired) as error:
        print(error, file=os.sys.stderr)
        raise SystemExit(1)
