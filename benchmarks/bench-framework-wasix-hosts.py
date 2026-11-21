#!/usr/bin/env python3
"""Compare Edge.js WASIX builds on framework HTTP servers.

A "lane" is one (napi_wasmer runner, Edge.js guest) pair described in a JSON
config; the default config, bench-framework-wasix-hosts.example.json,
compares a saved "baseline" snapshot with the current build. Each fixture
(node:http, Express, Hono, Next.js standalone) answers GET /api/bench with
the 11-byte body "hello edge\\n". Build the fixtures first with
benchmarks/frameworks/setup.sh.

Subcommands:

  snapshot NAME
          Copy the current runner and guest build (edgejs.wasm, node-lib, icu)
          so a later build can be compared with it. A lane uses it with
          {"snapshot": "NAME"}.

  smoke   Start every fixture once per lane, send a few thousand requests and
          check every reply. Takes about a minute; use it to see that a change
          did not break a server before timing it.
  run     The benchmark. For every fixture, hyperfine runs once per round and
          lane: `--setup` starts a fresh server and warms it up, the timed
          command is a fixed-request-count load (bench-framework-load.cjs,
          exact timing), and `--cleanup` stops the server. A second pass times
          process start to the first validated response. Each cell can wait
          for an idle machine first and is redone when the machine was busy
          or its runs disagree. With two or more lanes, the summary compares
          every lane with the first one.

hyperfine calls `start`, `stop` and `probe` itself.
"""

import argparse
import hashlib
import http.client
import json
import os
from pathlib import Path
import platform
import shlex
import shutil
import signal
import socket
import statistics
import subprocess
import sys
import time

HERE = Path(__file__).resolve().parent
HOST = "127.0.0.1"
ROUTE = "/api/bench"
BODY = b"hello edge\n"
FIXTURES = {
    "http": ("http", "server.cjs"),
    "express": ("express", "server.cjs"),
    "hono": ("hono", "server.cjs"),
    "next": ("next/.next/standalone", "server.js"),
}
# Fixed request counts per timed run: a few seconds of work on the slowest lane.
AMOUNTS = {"http": 250_000, "express": 150_000, "hono": 250_000, "next": 20_000}
REPO = HERE.parent
DEFAULT_CONFIG = HERE / "bench-framework-wasix-hosts.example.json"
DEFAULT_BUILD = REPO / "build-wasix"
DEFAULT_RUNNER = REPO / "build-wasix-napi" / "target" / "release" / "napi_wasmer"
DEFAULTS = {
    "fixtures": str(HERE / "frameworks" / ".build"),
    "snapshots": str(HERE / "frameworks" / ".build" / "snapshots"),
    "state": str(HERE / "frameworks" / ".build" / "state"),
    "node": "node",
    "compiler": "cranelift",
    "connections": 32,
    "warmup": 3,
}


def load_config(path):
    """Reads a config; relative paths in it are relative to the config file."""
    path = Path(path).resolve()
    config = {**DEFAULTS, **json.loads(path.read_text())}
    base = path.parent

    def resolve(value):
        return str((base / Path(value).expanduser()).resolve())

    for key in ("fixtures", "state", "snapshots"):
        config[key] = Path(resolve(config[key]))
    if os.sep in config["node"]:
        config["node"] = resolve(config["node"])
    for lane in config["lanes"].values():
        # {"snapshot": NAME} is a saved build with its runner inside;
        # {"build": DIR} is a build-wasix-style directory. Explicit keys win.
        if lane.get("snapshot"):
            build = config["snapshots"] / lane["snapshot"]
            lane.setdefault("runner", str(build / "napi_wasmer"))
        else:
            build = Path(resolve(lane["build"])) if lane.get("build") else None
        if build is not None:
            lane.setdefault("guest", str(build / "edgejs.wasm"))
            lane.setdefault("builtins", str(build / "node-lib"))
            lane["mounts"] = {str(build / "icu"): "/usr/share/edge-icu", **lane.get("mounts", {})}
        for key in ("runner", "guest", "builtins", "binary", "package"):
            if lane.get(key):
                lane[key] = resolve(lane[key])
        # "wasmer" may be a command name on PATH or a path.
        if lane.get("wasmer") and os.sep in lane["wasmer"]:
            lane["wasmer"] = resolve(lane["wasmer"])
        lane["mounts"] = {resolve(host): guest for host, guest in lane.get("mounts", {}).items()}
    return config


def sha256(path):
    digest = hashlib.sha256()
    with open(path, "rb") as file:
        for chunk in iter(lambda: file.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def free_port():
    with socket.socket() as listener:
        listener.bind((HOST, 0))
        return listener.getsockname()[1]


def lane_kind(lane):
    """napi: napi_wasmer runner + guest wasm (Edge.js V8 WASIX).
    native: a host executable (Edge.js native builds, Node.js).
    wasmer: a Wasmer package run with the wasmer CLI (Edge.js QuickJS WASIX)."""
    if lane.get("binary"):
        return "native"
    if lane.get("package"):
        return "wasmer"
    return "napi"


def guest_command(lane, extra, app_dir=None, env=None):
    """The command that runs `extra` in the lane, and the environment it adds.

    `extra` names the app entry as /app/<file> when `app_dir` is given."""
    kind = lane_kind(lane)
    guest_env = {**({} if lane.get("node") else {"EDGE_BYTECODE_CACHE": "0"}),
                 **lane.get("env", {}), **(env or {})}
    if kind == "native":
        args = [str(Path(app_dir) / arg[len("/app/"):]) if app_dir is not None and arg.startswith("/app/")
                else arg for arg in extra]
        return [lane["binary"], *args], guest_env
    if kind == "wasmer":
        command = [lane.get("wasmer", "wasmer"), "run", *lane.get("wasmer_args", ["--cranelift"]),
                   "--stack-size", str(lane.get("stack_size", 4194304)), "--net"]
        for name, value in guest_env.items():
            command += ["--env", f"{name}={value}"]
        for host, guest in lane.get("mounts", {}).items():
            command += ["--volume", f"{host}:{guest}"]
        if app_dir is not None:
            command += ["--volume", f"{app_dir}:/app", "--cwd", "/app"]
        return [*command, lane["package"], "--", *extra], {}
    command = [lane["runner"], lane["guest"], "--program-name", "edge"]
    if lane.get("builtins"):
        command += ["--builtin-js-dir", lane["builtins"]]
    for host, guest in lane.get("mounts", {}).items():
        command += ["--mount", f"{host}:{guest}"]
    if app_dir is not None:
        command += ["--mount", f"{app_dir}:/app", "--cwd", "/app"]
    for name, value in guest_env.items():
        command += ["--env", f"{name}={value}"]
    return [*command, "--", *extra], {}


def host_env(config, lane_name):
    env = dict(os.environ)
    for name in ("NODE_OPTIONS", "NODE_PATH", "NODE_COMPILE_CACHE"):
        env.pop(name, None)
    env["WASMER_NAPI_COMPILER"] = config["compiler"]
    env["WASMER_NAPI_CACHE_DIR"] = str(config["state"] / "module-cache" / lane_name)
    env.update(config["lanes"][lane_name].get("host_env", {}))
    return env


def spawn_server(config, lane_name, fixture, log_path):
    lane = config["lanes"][lane_name]
    work_dir, entry = FIXTURES[fixture]
    cwd = config["fixtures"] / work_dir
    port = free_port()
    env = dict(NODE_ENV="production", NEXT_TELEMETRY_DISABLED="1",
               HOST=HOST, HOSTNAME=HOST, PORT=str(port))
    command, extra_env = guest_command(lane, ("/app/" + entry,), app_dir=cwd, env=env)
    log = open(log_path, "w")
    start = time.monotonic()
    process = subprocess.Popen(command, cwd=cwd, env={**host_env(config, lane_name), **extra_env},
                               stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT,
                               start_new_session=True)
    return process, port, start, command


def wait_ready(process, port, start, timeout=90):
    deadline = start + timeout
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RuntimeError(f"server exited with status {process.returncode}")
        connection = http.client.HTTPConnection(HOST, port, timeout=30)
        try:
            connection.connect()
            listen_ms = (time.monotonic() - start) * 1000
            connection.request("GET", ROUTE)
            response = connection.getresponse()
            body = response.read(1024)
            if response.status != 200 or body != BODY:
                raise RuntimeError(f"unexpected response {response.status}: {body!r}")
            return listen_ms, (time.monotonic() - start) * 1000
        except ConnectionRefusedError:
            time.sleep(0.002)
        finally:
            connection.close()
    raise RuntimeError("server readiness timeout")


def kill_group(pid):
    for sig, wait in ((signal.SIGTERM, 3), (signal.SIGKILL, 3)):
        try:
            os.killpg(pid, sig)
        except ProcessLookupError:
            return
        deadline = time.monotonic() + wait
        while time.monotonic() < deadline:
            try:
                os.waitpid(pid, os.WNOHANG)
            except ChildProcessError:
                pass
            try:
                os.killpg(pid, 0)
            except ProcessLookupError:
                return
            except PermissionError:
                pass  # Darwin reports EPERM while the group only holds a zombie.
            time.sleep(0.01)


def autocannon_dir(config):
    return config["fixtures"] / "tools/node_modules/autocannon"


def autocannon_command(config, port, extra):
    return [config["node"], str(autocannon_dir(config) / "autocannon.js"),
            "--json", "--connections", str(config["connections"]), "--pipelining", "1",
            "--timeout", "10", "--renderStatusCodes", "--expectBody", BODY.decode(),
            *extra, f"http://{HOST}:{port}{ROUTE}"]


def load_command(config, port, amount, connections, output):
    command = [config["node"], str(HERE / "bench-framework-load.cjs"), str(autocannon_dir(config)),
               f"http://{HOST}:{port}{ROUTE}", str(amount), str(connections), BODY.decode(), str(output)]
    if config.get("max_seconds"):
        command.append(str(config["max_seconds"]))
    return command


def check_load(data):
    if any(data[name] for name in ("errors", "timeouts", "non2xx", "mismatches")):
        raise RuntimeError(f"HTTP errors: { {k: data[k] for k in ('errors', 'timeouts', 'non2xx', 'mismatches')} }")
    if data["requests"]["total"] == 0 or set(data["statusCodeStats"]) != {"200"}:
        raise RuntimeError("expected only successful replies")


def cell_paths(config, lane_name, fixture):
    base = config["state"] / "cells" / f"{fixture}-{lane_name}"
    return {"state": Path(str(base) + ".json"), "load": Path(str(base) + ".load.sh"),
            "samples": Path(str(base) + ".samples.jsonl"), "log": Path(str(base) + ".log"),
            "sessions": Path(str(base) + ".sessions.jsonl"), "probes": Path(str(base) + ".probes.jsonl")}


def amount_for(config, fixture):
    return max(1, int(AMOUNTS[fixture] * config.get("amount_scale", 1.0)))


def cmd_start(config, lane_name, fixture):
    paths = cell_paths(config, lane_name, fixture)
    paths["state"].parent.mkdir(parents=True, exist_ok=True)
    process, port, start, command = spawn_server(config, lane_name, fixture, paths["log"])
    # Record the server at once, so whoever kills this harness can find it.
    paths["state"].write_text(json.dumps({"pid": process.pid}))
    try:
        listen_ms, first_ms = wait_ready(process, port, start)
        warmup = subprocess.run(autocannon_command(config, port, ["--duration", str(config["warmup"])]),
                                capture_output=True, text=True, check=True)
        warm = json.loads(warmup.stdout)
        check_load(warm)
    except Exception:
        kill_group(process.pid)
        paths["state"].unlink(missing_ok=True)
        sys.stderr.write(paths["log"].read_text())
        raise
    load = load_command(config, port, amount_for(config, fixture), config["connections"], paths["samples"])
    paths["load"].write_text("#!/bin/sh\nexec " + " ".join(shlex.quote(part) for part in load) + "\n")
    paths["load"].chmod(0o755)
    session = {"pid": process.pid, "port": port, "listen_ms": listen_ms, "first_response_ms": first_ms,
               "warmup_requests": warm["requests"]["total"], "server_command": command}
    paths["state"].write_text(json.dumps(session))
    with paths["sessions"].open("a") as out:
        out.write(json.dumps(session) + "\n")


def cmd_stop(config, lane_name, fixture):
    paths = cell_paths(config, lane_name, fixture)
    if paths["state"].exists():
        kill_group(json.loads(paths["state"].read_text())["pid"])
        paths["state"].unlink()


def cmd_probe(config, lane_name, fixture):
    """Start a fresh server, wait for the first validated response, stop it."""
    paths = cell_paths(config, lane_name, fixture)
    paths["state"].parent.mkdir(parents=True, exist_ok=True)
    process, port, start, _ = spawn_server(config, lane_name, fixture, paths["log"])
    paths["state"].write_text(json.dumps({"pid": process.pid}))
    try:
        listen_ms, first_ms = wait_ready(process, port, start)
    finally:
        kill_group(process.pid)
        paths["state"].unlink(missing_ok=True)
    with paths["probes"].open("a") as out:
        out.write(json.dumps({"listen_ms": listen_ms, "first_response_ms": first_ms}) + "\n")


def read_jsonl(path):
    if not path.exists():
        return []
    return [json.loads(line) for line in path.read_text().splitlines() if line.strip()]


def cpu_idle():
    """System-wide idle CPU percentage over about one second."""
    if sys.platform == "darwin":
        text = subprocess.check_output(["top", "-l", "2", "-n", "0", "-s", "1"], text=True)
        line = [row for row in text.splitlines() if row.startswith("CPU usage:")][-1]
        return float(line.split("sys,")[1].split("%")[0])

    def sample():
        with open("/proc/stat") as stat:
            fields = [int(value) for value in stat.readline().split()[1:]]
        return fields[3] + fields[4], sum(fields)  # idle + iowait, total

    idle_a, total_a = sample()
    time.sleep(1)
    idle_b, total_b = sample()
    return 100.0 * (idle_b - idle_a) / max(1, total_b - total_a)


def wait_quiet(threshold, timeout=900):
    """Wait for three consecutive idle seconds; returns the last idle sample."""
    if threshold <= 0:
        return None
    deadline, streak, idle = time.monotonic() + timeout, 0, 0.0
    while time.monotonic() < deadline:
        idle = cpu_idle()
        streak = streak + 1 if idle >= threshold else 0
        if streak >= 3:
            return idle
    print(f"warning: machine still busy after {timeout}s (idle {idle:.0f}%)", flush=True)
    return idle


def run_hyperfine(lane, command, export, extra):
    argv = ["hyperfine", "-N", "--style", "basic", "--export-json", str(export),
            "--command-name", lane, *extra, command]
    subprocess.run(argv, check=True, stdout=subprocess.DEVNULL)
    return json.loads(Path(export).read_text())["results"][0]


def measure_load(config, self, lane, fixture, round_index, args, export):
    """One fresh server and `args.runs` timed loads; repeated when disturbed."""
    paths = cell_paths(config, lane, fixture)
    discarded = []
    for attempt in range(1, args.attempts + 1):
        idle_before = wait_quiet(args.idle)
        result = run_hyperfine(lane, str(paths["load"]), export,
                               ["--runs", str(args.runs), "--setup", f"{self} start {lane} {fixture}",
                                "--cleanup", f"{self} stop {lane} {fixture}"])
        idle_after = cpu_idle() if args.idle > 0 else None
        samples = read_jsonl(paths["samples"])[-args.runs:]
        if len(samples) != args.runs or not all(sample["ok"] for sample in samples):
            raise RuntimeError(f"load failed for {fixture}/{lane}: {samples}")
        rates = [sample["req_per_sec"] for sample in samples]
        spread = (max(rates) - min(rates)) / max(rates)
        row = {"round": round_index, "attempt": attempt, "cpu_idle_before": idle_before,
               "cpu_idle_after": idle_after, "run_spread": spread,
               "session": read_jsonl(paths["sessions"])[-1],
               "hyperfine_seconds": result["times"], "runs": samples}
        row["session"].pop("server_command", None)
        busy = idle_after is not None and idle_after < args.idle_after
        disturbed = busy or spread > args.spread
        idle_text = f"  idle {idle_before:.0f}%/{idle_after:.0f}%" if idle_after is not None else ""
        print(f"{fixture:8} {lane:28} round {round_index} attempt {attempt}: "
              + ", ".join(f"{rate:,.0f}" for rate in rates)
              + " req/s" + idle_text + ("  DISTURBED" if disturbed else ""),
              flush=True)
        if not disturbed or attempt == args.attempts:
            row["disturbed"] = disturbed
            row["discarded_attempts"] = discarded
            return row
        discarded.append(row)


def check_prerequisites(config, need_hyperfine):
    missing = []
    if need_hyperfine and shutil.which("hyperfine") is None:
        missing.append("hyperfine is not on PATH")
    if shutil.which(config["node"]) is None and not Path(config["node"]).exists():
        missing.append(f"node not found: {config['node']}")
    if not (autocannon_dir(config) / "autocannon.js").exists():
        missing.append(f"fixtures not built in {config['fixtures']}: run benchmarks/frameworks/setup.sh")
    for name, lane in config["lanes"].items():
        if lane.get("snapshot") and not (config["snapshots"] / lane["snapshot"]).exists():
            missing.append(f"lane {name}: no snapshot {lane['snapshot']!r}: run "
                           f"`benchmarks/bench-framework-wasix-hosts.py snapshot {lane['snapshot']}` first")
            continue
        for key in ("runner", "guest", "builtins", "binary", "package"):
            if lane.get(key) and not Path(lane[key]).exists():
                missing.append(f"lane {name}: {key} not found: {lane[key]}")
        if lane_kind(lane) == "wasmer" and shutil.which(lane.get("wasmer", "wasmer")) is None:
            missing.append(f"lane {name}: wasmer CLI not found: {lane.get('wasmer', 'wasmer')}")
        for host in lane.get("mounts", {}):
            if not Path(host).exists():
                missing.append(f"lane {name}: mount not found: {host}")
    if missing:
        sys.exit("error:\n  " + "\n  ".join(missing))


def lane_identity(config, name):
    lane = config["lanes"][name]
    command, extra_env = guest_command(lane, ("-p", "JSON.stringify(process.versions)"))
    output = subprocess.check_output(command, text=True, env={**host_env(config, name), **extra_env})
    versions = json.loads(output.strip().splitlines()[-1])
    kind = lane_kind(lane)
    hashed = {"napi": ("runner", "guest"), "native": ("binary",), "wasmer": ()}[kind]
    identity = {**lane, "kind": kind, "guest_edge_version": versions.get("edge"),
                "node_version": versions.get("node"),
                **{f"{key}_sha256": sha256(lane[key]) for key in hashed}}
    if kind == "wasmer":
        module = Path(lane["package"]) / "bin" / "edge"
        if module.exists():
            identity["guest_sha256"] = sha256(module)
    return identity


def cmd_snapshot(config, args):
    """Copy a runner and a guest build into the snapshot directory."""
    build, runner = Path(args.build).resolve(), Path(args.runner).resolve()
    sources = {"edgejs.wasm": build / "edgejs.wasm", "node-lib": build / "node-lib",
               "icu": build / "icu", "napi_wasmer": runner}
    missing = [str(path) for path in sources.values() if not path.exists()]
    if missing:
        sys.exit("error: not found:\n  " + "\n  ".join(missing))
    target = config["snapshots"] / args.name
    if target.exists():
        shutil.rmtree(target)
    target.mkdir(parents=True)
    for name, source in sources.items():
        if source.is_dir():
            shutil.copytree(source, target / name)
        else:
            shutil.copy2(source, target / name)
    print(f"snapshot {args.name}: {target}")
    print(f"  runner sha256 {sha256(target / 'napi_wasmer')}")
    print(f"  guest  sha256 {sha256(target / 'edgejs.wasm')}")


def cmd_smoke(config, args):
    """Start every fixture once per lane and check `args.requests` replies."""
    check_prerequisites(config, need_hyperfine=False)
    (config["state"] / "cells").mkdir(parents=True, exist_ok=True)
    failures = 0
    for lane in config["lanes"]:
        for fixture in args.fixtures or list(FIXTURES):
            paths = cell_paths(config, lane, fixture)
            output = config["state"] / "cells" / f"{fixture}-{lane}.smoke.jsonl"
            output.unlink(missing_ok=True)
            process, port, start, _ = spawn_server(config, lane, fixture, paths["log"])
            paths["state"].write_text(json.dumps({"pid": process.pid}))
            status = "ok"
            try:
                _, first_ms = wait_ready(process, port, start)
                load = subprocess.run(load_command(config, port, args.requests, 8, output),
                                      capture_output=True, text=True)
                result = read_jsonl(output)[-1] if output.exists() else None
                if load.returncode != 0 or result is None or not result["ok"]:
                    status = f"FAILED load: {result or load.stderr.strip()[-300:]}"
            except Exception as error:  # noqa: BLE001 - reported per cell
                first_ms, result, status = None, None, f"FAILED: {error}"
            finally:
                kill_group(process.pid)
                paths["state"].unlink(missing_ok=True)
            log = paths["log"].read_text(errors="replace")
            notes = [marker for marker in ("store is not lent", "Cannot call a native function",
                                           "Check failed", "panicked") if marker in log]
            if notes and status == "ok":
                status = "WARN log mentions " + ", ".join(repr(note) for note in notes)
            if status.startswith("FAILED"):
                failures += 1
            rate = f"{result['req_per_sec']:>9,.0f} req/s" if result and result.get("ok") else " " * 15
            first = f"first reply {first_ms:>5.0f} ms" if first_ms is not None else ""
            print(f"{fixture:8} {lane:28} {rate}  {first}  {status}", flush=True)
            if status != "ok":
                print(f"         log: {paths['log']}", flush=True)
                for line in log.strip().splitlines()[-15:]:
                    print(f"         | {line}", flush=True)
    if failures:
        sys.exit(f"{failures} smoke check(s) failed")


def cmd_run(config, config_path, args):
    check_prerequisites(config, need_hyperfine=True)
    state = config["state"]
    lanes = list(config["lanes"])
    fixtures = args.fixtures or list(FIXTURES)
    self = (f"{shlex.quote(sys.executable)} {shlex.quote(str(Path(__file__).resolve()))} "
            f"--config {shlex.quote(str(config_path))} --amount-scale {config['amount_scale']}"
            + (f" --max-seconds {config['max_seconds']}" if config.get("max_seconds") else ""))
    results_dir = state / "hyperfine"
    results_dir.mkdir(parents=True, exist_ok=True)
    (state / "cells").mkdir(parents=True, exist_ok=True)
    for stale in (state / "cells").glob("*"):
        stale.unlink()

    data = {
        "host": platform.platform(),
        "date_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "config": {name: config[name] for name in ("compiler", "connections", "warmup")},
        "method": {"rounds": args.rounds, "runs_per_round": args.runs, "startup_runs": args.startup_runs,
                   "amounts": {fixture: amount_for(config, fixture) for fixture in fixtures},
                   "max_seconds": config.get("max_seconds"),
                   "route": ROUTE, "expected_body": BODY.decode(),
                   "idle_gate_percent": args.idle, "idle_after_percent": args.idle_after,
                   "max_run_spread": args.spread, "attempts": args.attempts},
        "lanes": {name: lane_identity(config, name) for name in lanes},
        "throughput": {},
        "startup": {},
    }

    output = Path(args.output)
    output.parent.mkdir(parents=True, exist_ok=True)
    for round_index in range(1, args.rounds + 1):
        offset = (round_index - 1) % len(fixtures)
        order = lanes if round_index % 2 else lanes[::-1]
        for fixture in fixtures[offset:] + fixtures[:offset]:
            for lane in order:
                export = results_dir / f"load-{fixture}-{lane}-round{round_index}.json"
                row = measure_load(config, self, lane, fixture, round_index, args, export)
                data["throughput"].setdefault(fixture, {}).setdefault(lane, {"rounds": []})["rounds"].append(row)
                output.write_text(json.dumps(data, indent=2) + "\n")

    for fixture in fixtures if args.startup_runs > 0 else ():
        for lane in lanes:
            export = results_dir / f"startup-{fixture}-{lane}.json"
            idle_before = wait_quiet(args.idle)
            result = run_hyperfine(lane, f"{self} probe {lane} {fixture}", export,
                                   ["--warmup", str(args.startup_warmup), "--runs", str(args.startup_runs)])
            probes = read_jsonl(cell_paths(config, lane, fixture)["probes"])[-args.startup_runs:]
            data["startup"].setdefault(fixture, {})[lane] = {
                "cpu_idle_before": idle_before,
                "cpu_idle_after": cpu_idle() if args.idle > 0 else None,
                "hyperfine_seconds": result["times"],
                "listen_ms": [p["listen_ms"] for p in probes],
                "first_response_ms": [p["first_response_ms"] for p in probes],
            }
            first = data["startup"][fixture][lane]["first_response_ms"]
            print(f"{fixture:8} {lane:28} first reply median {statistics.median(first):.1f} ms "
                  f"[{min(first):.1f}-{max(first):.1f}]", flush=True)
            output.write_text(json.dumps(data, indent=2) + "\n")

    summary = {}
    for fixture, by_lane in data["throughput"].items():
        for lane, cell in by_lane.items():
            rates = [run["req_per_sec"] for r in cell["rounds"] for run in r["runs"]]
            walls = [t for r in cell["rounds"] for t in r["hyperfine_seconds"]]
            p99 = [run["latency_ms"]["p99"] for r in cell["rounds"] for run in r["runs"]]
            startup = data["startup"].get(fixture, {}).get(lane, {})
            summary.setdefault(fixture, {})[lane] = {
                "req_per_sec_mean": statistics.mean(rates), "req_per_sec_min": min(rates),
                "req_per_sec_max": max(rates), "hyperfine_wall_s_mean": statistics.mean(walls),
                "p99_ms_median": statistics.median(p99),
                "disturbed_rounds": sum(1 for r in cell["rounds"] if r["disturbed"]),
                "first_response_ms_median": statistics.median(startup["first_response_ms"]) if startup else None,
                "listen_ms_median": statistics.median(startup["listen_ms"]) if startup else None,
            }
        baseline = summary[fixture][lanes[0]]["req_per_sec_mean"]
        for row in summary[fixture].values():
            row["req_per_sec_vs_first_lane"] = row["req_per_sec_mean"] / baseline - 1
    data["summary"] = summary
    output.write_text(json.dumps(data, indent=2) + "\n")

    print()
    for fixture, by_lane in summary.items():
        for lane, row in by_lane.items():
            change = "" if lane == lanes[0] else f"  {row['req_per_sec_vs_first_lane']:+.1%} vs {lanes[0]}"
            first = row["first_response_ms_median"]
            first_text = f"  first reply {first:.0f} ms" if first is not None else ""
            print(f"{fixture:8} {lane:28} {row['req_per_sec_mean']:>10,.0f} req/s "
                  f"[{row['req_per_sec_min']:,.0f}-{row['req_per_sec_max']:,.0f}]  "
                  f"p99 {row['p99_ms_median']:.2f} ms{first_text}{change}"
                  + ("  (disturbed)" if row["disturbed_rounds"] else ""))
    print(f"\nwrote {output}")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--config", default=str(DEFAULT_CONFIG),
                        help="lanes to compare (default: %(default)s)")
    parser.add_argument("--amount-scale", type=float, default=1.0,
                        help="multiply the fixed request counts, e.g. 0.2 for a quick run")
    parser.add_argument("--max-seconds", type=float,
                        help="also end each timed run after this many seconds (rate then counts the replies so far)")
    sub = parser.add_subparsers(dest="command", required=True)
    for name in ("start", "stop", "probe"):
        cell = sub.add_parser(name)
        cell.add_argument("lane")
        cell.add_argument("fixture")
    snapshot = sub.add_parser("snapshot", help="save the current build for later comparison")
    snapshot.add_argument("name")
    snapshot.add_argument("--build", default=str(DEFAULT_BUILD), help="guest build directory (default: %(default)s)")
    snapshot.add_argument("--runner", default=str(DEFAULT_RUNNER), help="napi_wasmer binary (default: %(default)s)")
    smoke = sub.add_parser("smoke", help="check that every fixture serves on every lane")
    smoke.add_argument("--requests", type=int, default=2000)
    smoke.add_argument("--fixtures", nargs="+", choices=list(FIXTURES))
    run = sub.add_parser("run", help="the benchmark")
    run.add_argument("--output", default=str(HERE / "results" / "framework-wasix-hosts.json"))
    run.add_argument("--rounds", type=int, default=2)
    run.add_argument("--runs", type=int, default=2)
    run.add_argument("--startup-runs", type=int, default=5, help="0 skips the startup pass")
    run.add_argument("--startup-warmup", type=int, default=2, help="unmeasured server starts before them")
    run.add_argument("--idle", type=float, default=85.0,
                     help="minimum system idle CPU %% before a cell; 0 disables the idle checks")
    run.add_argument("--idle-after", type=float, default=70.0, help="idle CPU %% below which a finished cell is redone")
    run.add_argument("--spread", type=float, default=0.06, help="maximum relative gap between a cell's runs")
    run.add_argument("--attempts", type=int, default=3)
    run.add_argument("--fixtures", nargs="+", choices=list(FIXTURES))
    args = parser.parse_args()
    config = load_config(args.config)
    config["amount_scale"] = args.amount_scale
    config["max_seconds"] = args.max_seconds
    if args.command == "run":
        cmd_run(config, Path(args.config).resolve(), args)
    elif args.command == "smoke":
        cmd_smoke(config, args)
    elif args.command == "snapshot":
        cmd_snapshot(config, args)
    else:
        {"start": cmd_start, "stop": cmd_stop, "probe": cmd_probe}[args.command](config, args.lane, args.fixture)


if __name__ == "__main__":
    main()
