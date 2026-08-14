#!/usr/bin/env python3

import argparse
import os
import re
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path


IGNORED_STDERR_PATTERNS = (
    re.compile(r"Skipping duplicate additional import env\.memory"),
)


@dataclass(frozen=True)
class Case:
    name: str
    script: str
    expected_stdout: str = ""
    # Cases that exercise process termination expect a specific exit status and
    # are allowed to say so on stderr; everything else must exit 0 silently.
    expected_returncode: int = 0
    expected_stderr_contains: str = ""


def sanitize_stderr(stderr: str) -> str:
    lines = []
    for line in stderr.splitlines():
        if any(pattern.search(line) for pattern in IGNORED_STDERR_PATTERNS):
            continue
        if line.strip():
            lines.append(line)
    return "\n".join(lines)


def run_case(wasmer_bin: str, package_dir: Path, timeout: int, case: Case) -> None:
    name = case.name
    cmd = [
        wasmer_bin,
        "run",
        ".",
        "--experimental-napi",
        "--net",
        "--",
        "-e",
        case.script,
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
        raise RuntimeError(
            f"{name} timed out after {timeout}s\n"
            f"stdout: {exc.stdout or ''}\n"
            f"stderr: {exc.stderr or ''}"
        ) from exc

    stdout = completed.stdout
    stderr = sanitize_stderr(completed.stderr)

    if completed.returncode != case.expected_returncode:
        raise RuntimeError(
            f"{name} exited with {completed.returncode}, "
            f"expected {case.expected_returncode}\n"
            f"stdout: {stdout}\n"
            f"stderr: {stderr}"
        )

    if stdout != case.expected_stdout:
        raise RuntimeError(
            f"{name} stdout mismatch\n"
            f"expected: {case.expected_stdout!r}\n"
            f"actual:   {stdout!r}\n"
            f"stderr: {stderr}"
        )

    if case.expected_stderr_contains:
        if case.expected_stderr_contains not in stderr:
            raise RuntimeError(
                f"{name} stderr missing expected text\n"
                f"expected to contain: {case.expected_stderr_contains!r}\n"
                f"stderr: {stderr}"
            )
    elif stderr:
        raise RuntimeError(
            f"{name} emitted unexpected stderr\n"
            f"stderr: {stderr}"
        )

    print(f"[ok] {name}: {stdout.strip() or completed.returncode}")


def build_cases(host: str, include_network: bool) -> list[Case]:
    cases = [
        Case(
            name="process.versions.webcontainer",
            script="const version = process.versions.webcontainer; if (typeof version !== 'string' || !version.startsWith('wasix')) throw new Error(`unexpected webcontainer version: ${version}`); console.log('WEBCONTAINER OK');",
            expected_stdout="WEBCONTAINER OK\n",
        ),
        Case(
            name="queueMicrotask",
            script="console.log('A'); queueMicrotask(() => console.log('B')); console.log('C');",
            expected_stdout="A\nC\nB\n",
        ),
        Case(
            name="blob.arrayBuffer",
            script="new Blob([new Uint8Array([65,66,67])]).arrayBuffer().then((ab) => console.log('BLOB', ab.byteLength));",
            expected_stdout="BLOB 3\n",
        ),
        # Keep the JS wrapper alive until environment teardown. This covers the
        # cleanup ordering between QuickJS finalizers and the embedder-owned
        # WebAssembly state.
        Case(
            name="WebAssembly.Memory survives environment teardown",
            script="globalThis.memory = new WebAssembly.Memory({ initial: 1 }); console.log('MEMORY', memory.buffer.byteLength);",
            expected_stdout="MEMORY 65536\n",
        ),
        # Next.js uses this value to decide whether the worker is approaching
        # its heap limit. A zero limit makes its memory watchdog exit eagerly.
        Case(
            name="v8 heap statistics report the embedder limit",
            script="const limit = require('node:v8').getHeapStatistics().heap_size_limit; console.log('HEAP', limit > 0);",
            expected_stdout="HEAP true\n",
        ),
        # Regression coverage for the signal handler arity bug. SIGINT and
        # SIGTERM are installed by RegisterSignalHandler() as three-argument
        # sa_sigaction handlers; when SA_SIGINFO was missing, wasix-libc
        # dispatched them through the one-argument signature and the
        # mismatched call_indirect trapped ("indirect call type mismatch"),
        # killing the instance with exit 27 before node::SignalExit could run.
        #
        # Without a JS listener the signal must instead reach SignalExit, which
        # re-raises so the default disposition terminates the process. Note
        # this only reproduces under WASIX: on native ABIs the surplus
        # arguments are harmless, so the equivalent native test cannot catch it.
        Case(
            name="SIGINT without a JS listener terminates",
            script="process.kill(process.pid, 'SIGINT'); setTimeout(() => console.log('NOT REACHED'), 500);",
            expected_returncode=127,
            expected_stderr_contains="termination signal",
        ),
        Case(
            name="SIGTERM reaches a JS listener",
            script="process.on('SIGTERM', () => { console.log('HANDLED'); process.exit(0); }); process.kill(process.pid, 'SIGTERM'); setTimeout(() => {}, 5000);",
            expected_stdout="HANDLED\n",
        ),
    ]

    if include_network:
        cases.extend([
            Case(
                name=f"fetch http://{host}/",
                script=f"const keepAlive = setTimeout(() => {{}}, 30000); fetch('http://{host}/').then((r) => console.log('FETCH', r.status)).catch((e) => {{ console.error('FETCHERR', e && (e.stack || e.message || e)); process.exitCode = 1; }}).finally(() => clearTimeout(keepAlive));",
                expected_stdout="FETCH 200\n",
            ),
            Case(
                name=f"https.get https://{host}/",
                script=f"require('node:https').get({{ hostname: '{host}', port: 443, path: '/', servername: '{host}' }}, (r) => {{ console.log('HTTPS', r.statusCode); r.resume(); }}).on('error', (e) => {{ console.error('HTTPSERR', e && (e.stack || e.message || e)); process.exit(1); }});",
                expected_stdout="HTTPS 200\n",
            ),
            Case(
                name=f"tls.connect verified {host}",
                script=f"const tls=require('node:tls'); const s=tls.connect(443,'{host}',{{servername:'{host}'}},()=>{{ console.log('TLS CONNECTED', s.authorized); s.destroy(); }}); s.on('close',()=>console.log('TLS CLOSE')); process.on('exit',(code)=>console.log('TLS EXIT', code)); s.on('error',(e)=>{{ console.error('TLSERR', e && (e.stack || e.message || e)); process.exitCode = 1; }});",
                expected_stdout="TLS CONNECTED true\nTLS CLOSE\nTLS EXIT 0\n",
            ),
        ])

    return cases


def build_cases(host: str, include_network: bool) -> list[tuple[str, str, str]]:
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
    ]

    if include_network:
        cases.extend([
            (
                f"fetch http://{host}/",
                f"const keepAlive = setTimeout(() => {{}}, 30000); fetch('http://{host}/').then((r) => console.log('FETCH', r.status)).catch((e) => {{ console.error('FETCHERR', e && (e.stack || e.message || e)); process.exitCode = 1; }}).finally(() => clearTimeout(keepAlive));",
                "FETCH 200\n",
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
        ])

    return cases


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
        "--include-network",
        action="store_true",
        help="Run outbound HTTP/TLS smoke checks in addition to deterministic local checks.",
    )
    args = parser.parse_args()

    package_dir = Path(args.package_dir).resolve()
    if not (package_dir / "wasmer.toml").is_file():
        raise RuntimeError(f"missing wasmer.toml in {package_dir}")

    cases = build_cases(args.https_host, include_network=args.include_network)

    for case in cases:
        run_case(args.wasmer_bin, package_dir, args.timeout, case)

    if not args.include_network:
        print("Skipped outbound network smoke tests; pass --include-network to run them.")
    print("All WASIX safe-mode smoke tests passed.")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except RuntimeError as exc:
        print(str(exc), file=sys.stderr)
        raise SystemExit(1)
