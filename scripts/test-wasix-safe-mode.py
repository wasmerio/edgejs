#!/usr/bin/env python3

import argparse
import os
import re
import subprocess
import sys
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path


IGNORED_STDERR_PATTERNS = (
    re.compile(r"Skipping duplicate additional import env\.memory"),
)


def sanitize_stderr(stderr: str) -> str:
    lines = []
    for line in stderr.splitlines():
        if any(pattern.search(line) for pattern in IGNORED_STDERR_PATTERNS):
            continue
        if line.strip():
            lines.append(line)
    return "\n".join(lines)


@dataclass
class CaseResult:
    name: str
    passed: bool
    log_name: str
    message: str


def slugify(value: str) -> str:
    return re.sub(r"[^A-Za-z0-9_.-]+", "-", value).strip("-").lower() or "case"


def write_log(log_path: Path, cmd: list[str], stdout: str, stderr: str, returncode: int,
              message: str) -> None:
    log_path.parent.mkdir(parents=True, exist_ok=True)
    log_path.write_text(
        "\n".join([
            f"command: {' '.join(cmd)}",
            f"exit_code: {returncode}",
            "",
            "stdout:",
            stdout,
            "",
            "stderr:",
            stderr,
            "",
            "message:",
            message,
            "",
        ]),
        encoding="utf-8",
    )


def run_case(wasmer_bin: str, package_dir: Path, timeout: int, name: str, script: str,
             expected_stdout: str, log_path: Path) -> CaseResult:
    cmd = [
        wasmer_bin,
        "run",
        ".",
        "--experimental-napi",
        "--net",
        "--",
        "-e",
        script,
    ]

    try:
        completed = subprocess.run(
            cmd,
            cwd=package_dir,
            capture_output=True,
            text=True,
            timeout=timeout,
            check=False,
        )
    except subprocess.TimeoutExpired as exc:
        stdout = exc.stdout or ""
        stderr = sanitize_stderr(exc.stderr or "")
        message = f"{name} timed out after {timeout}s"
        write_log(log_path, cmd, stdout, stderr, 124, message)
        return CaseResult(name, False, log_path.name, message)

    stdout = completed.stdout
    stderr = sanitize_stderr(completed.stderr)

    if completed.returncode != 0:
        message = f"{name} exited with {completed.returncode}"
        write_log(log_path, cmd, stdout, stderr, completed.returncode, message)
        return CaseResult(name, False, log_path.name, message)

    if stdout != expected_stdout:
        message = (
            f"{name} stdout mismatch; expected {expected_stdout!r}, "
            f"actual {stdout!r}"
        )
        write_log(log_path, cmd, stdout, stderr, completed.returncode, message)
        return CaseResult(name, False, log_path.name, message)

    if stderr:
        message = f"{name} emitted unexpected stderr"
        write_log(log_path, cmd, stdout, stderr, completed.returncode, message)
        return CaseResult(name, False, log_path.name, message)

    write_log(log_path, cmd, stdout, stderr, completed.returncode, "passed")
    print(f"[ok] {name}: {stdout.strip()}")
    return CaseResult(name, True, log_path.name, "passed")


def write_summary(summary_path: Path, package_dir: Path, wasmer_bin: str, host: str,
                  results: list[CaseResult]) -> None:
    passed = sum(1 for result in results if result.passed)
    failed = len(results) - passed
    lines = [
        "# WASIX Safe-Mode Smoke Report",
        "",
        "| Field | Value |",
        "|---|---|",
        f"| Generated | `{datetime.now(timezone.utc).isoformat()}` |",
        f"| Package dir | `{package_dir}` |",
        f"| Wasmer binary | `{wasmer_bin}` |",
        f"| HTTPS host | `{host}` |",
        "",
        "## Results",
        "",
        "| Case | Status | Log | Message |",
        "|---|---|---|---|",
    ]
    for result in results:
        status = "PASS" if result.passed else "FAIL"
        lines.append(
            f"| `{result.name}` | {status} | `{result.log_name}` | {result.message} |"
        )
    lines.extend([
        "",
        "## Summary",
        "",
        f"- Passed: {passed}",
        f"- Failed: {failed}",
        "",
    ])
    summary_path.parent.mkdir(parents=True, exist_ok=True)
    summary_path.write_text("\n".join(lines), encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(description="Run WASIX safe-mode smoke tests through Wasmer.")
    parser.add_argument(
        "--wasmer-bin",
        default=os.environ.get("WASMER_BIN", "wasmer"),
        help="Path to the Wasmer CLI binary.",
    )
    parser.add_argument(
        "--package-dir",
        default=str(Path(__file__).resolve().parents[1]),
        help="Directory containing wasmer.toml and the built WASIX package.",
    )
    parser.add_argument(
        "--timeout",
        type=int,
        default=30,
        help="Per-case timeout in seconds.",
    )
    parser.add_argument(
        "--https-host",
        default="example.com",
        help="Host used for verified HTTPS/TLS smoke coverage.",
    )
    parser.add_argument(
        "--artifact-dir",
        default=os.environ.get("WASIX_SAFE_MODE_ARTIFACT_DIR", "artifacts/wasix-safe-mode"),
        help="Directory where markdown summary and per-case logs are written.",
    )
    args = parser.parse_args()

    package_dir = Path(args.package_dir).resolve()
    if not (package_dir / "wasmer.toml").is_file():
        raise RuntimeError(f"missing wasmer.toml in {package_dir}")
    host = args.https_host
    artifact_dir = Path(args.artifact_dir).resolve()
    run_id = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    run_dir = artifact_dir / run_id

    cases = [
        (
            "queueMicrotask",
            "console.log('A'); queueMicrotask(() => console.log('B')); console.log('C');",
            "A\nC\nB\n",
        ),
        (
            "blob.arrayBuffer",
            "new Blob([new Uint8Array([65,66,67])]).arrayBuffer().then((ab) => console.log('BLOB', ab.byteLength));",
            "BLOB 3\n",
        ),
        (
            f"fetch http://{host}/",
            f"fetch('http://{host}/').then((r) => console.log('FETCH', r.status)).catch((e) => {{ console.error('FETCHERR', e && (e.stack || e.message || e)); process.exit(1); }});",
            "FETCH 200\n",
        ),
        (
            f"fetch https://{host}/",
            f"fetch('https://{host}/').then((r) => console.log('FETCH HTTPS', r.status)).catch((e) => {{ console.error('FETCHHTTPSERR', e && (e.stack || e.message || e)); process.exit(1); }});",
            "FETCH HTTPS 200\n",
        ),
        (
            f"https.get https://{host}/",
            f"require('node:https').get({{ hostname: '{host}', port: 443, path: '/', servername: '{host}' }}, (r) => {{ console.log('HTTPS', r.statusCode); r.resume(); }}).on('error', (e) => {{ console.error('HTTPSERR', e && (e.stack || e.message || e)); process.exit(1); }});",
            "HTTPS 200\n",
        ),
        (
            f"tls.connect verified {host}",
            f"const tls=require('node:tls'); const s=tls.connect(443,'{host}',{{servername:'{host}'}},()=>{{ console.log('TLS CONNECTED', s.authorized); s.destroy(); }}); s.on('close',()=>console.log('TLS CLOSE')); process.on('exit',(code)=>console.log('TLS EXIT', code)); s.on('error',(e)=>{{ console.error('TLSERR', e && (e.stack || e.message || e)); process.exitCode = 1; }});",
            "TLS CONNECTED true\nTLS CLOSE\nTLS EXIT 0\n",
        ),
    ]

    results = []
    for index, (name, script, expected_stdout) in enumerate(cases, start=1):
        log_path = run_dir / f"{index:02d}-{slugify(name)}.log"
        result = run_case(
            args.wasmer_bin,
            package_dir,
            args.timeout,
            name,
            script,
            expected_stdout,
            log_path,
        )
        results.append(result)
        if not result.passed:
            print(f"[fail] {name}: {result.message}", file=sys.stderr)

    summary_path = run_dir / "summary.md"
    write_summary(summary_path, package_dir, args.wasmer_bin, host, results)
    print(f"WASIX safe-mode smoke report: {summary_path}")

    if any(not result.passed for result in results):
        return 1

    print("All WASIX safe-mode smoke tests passed.")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except RuntimeError as exc:
        print(str(exc), file=sys.stderr)
        raise SystemExit(1)
