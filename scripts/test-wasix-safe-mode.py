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
    alternate_outcomes: tuple[tuple[int, str], ...] = ()
    # Extra guest environment variables, passed as `wasmer run --env K=V`.
    env: tuple[tuple[str, str], ...] = ()


def sanitize_stderr(stderr: str) -> str:
    lines = []
    for line in stderr.splitlines():
        if any(pattern.search(line) for pattern in IGNORED_STDERR_PATTERNS):
            continue
        if line.strip():
            lines.append(line)
    return "\n".join(lines)


def build_command(wasmer_bin: str, case: Case) -> list[str]:
    cmd = [
        wasmer_bin,
        "run",
        ".",
        "--experimental-napi",
        "--net",
    ]
    for key, value in case.env:
        cmd.extend(["--env", f"{key}={value}"])
    cmd.extend(["--", "-e", case.script])
    return cmd


def run_case(wasmer_bin: str, package_dir: Path, timeout: int, case: Case) -> None:
    name = case.name
    cmd = build_command(wasmer_bin, case)

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

    expected_outcomes = (
        (case.expected_returncode, case.expected_stderr_contains),
        *case.alternate_outcomes,
    )
    matching_outcomes = [
        stderr_marker
        for returncode, stderr_marker in expected_outcomes
        if completed.returncode == returncode
    ]
    if not matching_outcomes:
        expected_returncodes = ", ".join(str(code) for code, _ in expected_outcomes)
        raise RuntimeError(
            f"{name} exited with {completed.returncode}, "
            f"expected one of: {expected_returncodes}\n"
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

    expected_stderr_markers = [marker for marker in matching_outcomes if marker]
    if expected_stderr_markers:
        if not any(marker in stderr for marker in expected_stderr_markers):
            raise RuntimeError(
                f"{name} stderr missing expected text\n"
                f"expected to contain one of: {expected_stderr_markers!r}\n"
                f"stderr: {stderr}"
            )
    elif stderr:
        raise RuntimeError(
            f"{name} emitted unexpected stderr\n"
            f"stderr: {stderr}"
        )

    print(f"[ok] {name}: {stdout.strip() or completed.returncode}")


def build_cases(host: str, include_network: bool, external_static_data: bool = True) -> list[Case]:
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
        # re-raises so the default disposition terminates the process. Wasmer
        # Released Wasmer reports that termination using the conventional
        # 128 + signal status. The current source-built Linux CLI reports its
        # generic terminated-program status (127) instead; in both cases the
        # stderr marker proves that Wasmer observed the termination signal.
        # Note this only reproduces under WASIX: on native ABIs the surplus
        # arguments are harmless, so the equivalent native test cannot catch it.
        Case(
            name="SIGINT without a JS listener terminates",
            script="process.kill(process.pid, 'SIGINT'); setTimeout(() => console.log('NOT REACHED'), 500);",
            expected_returncode=128 + 2,
            expected_stderr_contains="ExitCode::130",
            alternate_outcomes=((127, "termination signal"),),
        ),
        Case(
            name="SIGTERM reaches a JS listener",
            script="process.on('SIGTERM', () => { console.log('HANDLED'); process.exit(0); }); process.kill(process.pid, 'SIGTERM'); setTimeout(() => {}, 5000);",
            expected_stdout="HANDLED\n",
        ),
        # WASIX packages read builtin JS from /node-lib and ICU's individual
        # data items from /usr/share/edge-icu (wasmer.toml [fs]) instead of
        # embedding them. These cases fail if either tree is not mounted or is
        # out of date: without ICU data, collation and month names fall back to
        # root/English and legacy decoders are missing.
        Case(
            name="Intl.Collator de and DateTimeFormat de use ICU data",
            script="const s = ['z', '\\u00e4', 'a'].sort(new Intl.Collator('de').compare).join(','); const m = new Intl.DateTimeFormat('de', { month: 'long', timeZone: 'UTC' }).format(0); if (s !== 'a,\\u00e4,z' || m !== 'Januar') throw new Error(`unexpected ICU result: ${JSON.stringify([s, m])}`); console.log('INTL DE OK');",
            expected_stdout="INTL DE OK\n",
        ),
        Case(
            name="TextDecoder shift_jis uses ICU converter data",
            script="const s = new TextDecoder('shift_jis').decode(new Uint8Array([0x93, 0xfa, 0x96, 0x7b])); if (s !== '\\u65e5\\u672c') throw new Error(`unexpected shift_jis decode: ${JSON.stringify(s)}`); console.log('SHIFT_JIS OK');",
            expected_stdout="SHIFT_JIS OK\n",
        ),
        # internal/deps sources are served from /node-lib/internal/deps. The
        # natives entry may be an accessor (read lazily from the filesystem) or
        # a cached data property, so read it through its descriptor.
        Case(
            name="process.binding('natives') reads internal/deps sources",
            script="const natives = process.binding('natives'); const id = 'internal/deps/acorn/acorn/dist/acorn'; const d = Object.getOwnPropertyDescriptor(natives, id); if (!d) throw new Error(`missing natives entry ${id}`); const src = 'value' in d ? d.value : d.get.call({}); if (typeof src !== 'string' || src.length === 0) throw new Error(`empty natives source for ${id}`); console.log('NATIVES OK');",
            expected_stdout="NATIVES OK\n",
        ),
        # fetch() lazily loads internal/deps/undici from /node-lib; a data: URL
        # keeps the check offline.
        Case(
            name="fetch data: URL loads undici from /node-lib",
            script="fetch('data:text/plain,edge').then((r) => r.text()).then((t) => console.log('FETCH DATA', t));",
            expected_stdout="FETCH DATA edge\n",
        ),
    ]

    if external_static_data:
        # An ICU data directory override that holds no ICU data must be
        # reported on stderr, naming the directory, and fall back to the
        # package's /usr/share/edge-icu so ICU-backed decoding keeps working.
        # Builds that embed ICU (EDGE_WASIX_EMBED_STATIC_DATA=1) ignore the
        # variable.
        cases.append(Case(
            name="unusable ICU data directory falls back to the package mount",
            script="console.log(new TextDecoder('shift_jis').decode(new Uint8Array([0x82, 0xa0])).charCodeAt(0));",
            expected_stdout="12354\n",
            expected_stderr_contains="/nonexistent-edge-icu",
            env=(("EDGE_ICU_DATA", "/nonexistent-edge-icu"),),
        ))

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
    parser.add_argument(
        "--embedded-static-data",
        action="store_true",
        help="The package embeds builtin JS and ICU data (EDGE_WASIX_EMBED_STATIC_DATA=1); skip checks that only apply to filesystem-backed data.",
    )
    args = parser.parse_args()

    package_dir = Path(args.package_dir).resolve()
    if not (package_dir / "wasmer.toml").is_file():
        raise RuntimeError(f"missing wasmer.toml in {package_dir}")

    cases = build_cases(
        args.https_host,
        include_network=args.include_network,
        external_static_data=not args.embedded_static_data,
    )

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
