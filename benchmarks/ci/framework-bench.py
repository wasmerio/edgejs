#!/usr/bin/env python3
"""Pull request framework benchmarks: every runtime, on one machine.

Used by the `bench` and `bench-comment` jobs in
.github/workflows/test-and-build.yml. GitHub runners differ from each other by
more than the changes being measured, so one job measures all of these on the
same runner:

  v8-native, v8-wasix, quickjs-native, quickjs-wasix
              Edge.js: the PR's build (from this workflow run) against the
              newest finished build of the base branch.
  node24, node26
              Node.js references: the same binary in both lanes, which also
              measures the noise of the run (an A/A comparison).

Subcommands:

  prepare  Unpack the PR's builds and download the base branch's.
  run      Benchmark every runtime and framework within a time budget. The two
           lanes of a runtime run at the same time, each pinned to one half of
           the CPUs, and swap halves between their two measurements.
  report   Compare the lanes, write the Markdown report to the job summary
           and, with --post, create or update the pull request's comment.
"""

import argparse
import base64
from concurrent.futures import ThreadPoolExecutor
import importlib.util
import json
import math
import os
from pathlib import Path
import select
import shutil
import socket
import statistics
import subprocess
import sys
import threading
import time
import zipfile

HERE = Path(__file__).resolve().parent
BENCH = HERE.parent
HARNESS = BENCH / "bench-framework-wasix-hosts.py"
MARKER = "<!-- edgejs-framework-bench -->"
HISTORY_PREFIX = "<!-- edgejs-framework-bench-history: "
# Base builds come from pushes to the base branch, from either the merged
# workflow or the QuickJS workflow it replaced.
WORKFLOWS = ("test-and-build.yml", "test-and-build-quickjs.yml")
RUNNER_ARTIFACT = "napi-wasmer-linux-amd64"
RUNTIMES = {
    "v8-native": {"title": "Edge.js V8 native", "artifact": "edge-linux-amd64"},
    "v8-wasix": {"title": "Edge.js V8 WASIX", "artifact": "edge-wasix", "runner": True},
    "quickjs-native": {"title": "Edge.js QuickJS native", "artifact": "edge-quickjs-linux-amd64"},
    "quickjs-wasix": {"title": "Edge.js QuickJS WASIX", "artifact": "edge-quickjs-wasix"},
    "node24": {"title": "Node.js 24", "reference": True},
    "node26": {"title": "Node.js 26", "reference": True},
}
FIXTURE_NAMES = {"http": "node:http", "express": "Express", "hono": "Hono", "next": "Next.js"}
SIDES = ("head", "base")
CONNECTIONS = 32
# A server start that takes longer than this counts as hung.
START_TIMEOUT = 20
# A change counts only when both measurements move the same way by more than
# a bar (twice it with a single measurement): this floor, or NOISE_FACTOR times
# the largest difference Node.js shows against itself on that server in the
# same run, whichever is higher. Separately built identical sources differ by
# a few percent, and these measurement windows are about a second long.
THROUGHPUT_FLOOR = 0.05
STARTUP_FLOOR = 0.10
NOISE_FACTOR = 1.5


def gh(*args, **kwargs):
    return subprocess.run(["gh", *args], check=True, capture_output=True, text=True, **kwargs).stdout


def gh_json(*args):
    return json.loads(gh("api", *args))


def log(message):
    print(message, file=sys.stderr, flush=True)


def load_harness():
    spec = importlib.util.spec_from_file_location("framework_harness", HARNESS)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


# --- prepare ----------------------------------------------------------------


def artifacts_named(repo, name, pages=3):
    for page in range(1, pages + 1):
        data = gh_json("-X", "GET", f"repos/{repo}/actions/artifacts", "-f", f"name={name}",
                       "-f", "per_page=100", "-f", f"page={page}")
        yield from (artifact for artifact in data["artifacts"] if not artifact["expired"])
        if len(data["artifacts"]) < 100:
            return


def run_is_base_build(repo, run_id, branch):
    run = gh_json(f"repos/{repo}/actions/runs/{run_id}")
    return (run["event"] == "push" and run["head_branch"] == branch
            and run["path"].split("@")[0].rsplit("/", 1)[-1] in WORKFLOWS)


def download(repo, run_id, name, dest):
    dest.mkdir(parents=True, exist_ok=True)
    gh("run", "download", str(run_id), "-R", repo, "-n", name, "-D", str(dest))


def submodule_pin(repo, ref):
    return gh_json(f"repos/{repo}/contents/napi?ref={ref}")["sha"]


def unpack(zip_path, dest):
    """Extracts a dist zip, keeping the executable bits zipfile drops."""
    with zipfile.ZipFile(zip_path) as archive:
        for info in archive.infolist():
            path = archive.extract(info, dest)
            mode = (info.external_attr >> 16) & 0o777
            if mode:
                os.chmod(path, mode)


def fetch_base(args, name, head_runner, out):
    """Downloads one runtime's base build into `out` and returns its origin."""
    runtime = RUNTIMES[name]
    artifact_name = runtime["artifact"]
    candidates = [a for a in artifacts_named(args.repo, artifact_name)
                  if a["workflow_run"]["head_branch"] == args.base_ref]
    exact = [a for a in candidates if a["workflow_run"]["head_sha"] == args.base_sha]
    chosen = None
    checked = set()
    for artifact in exact + candidates:  # the artifacts API lists newest first
        run_id = artifact["workflow_run"]["id"]
        if run_id in checked:
            continue
        checked.add(run_id)
        if run_is_base_build(args.repo, run_id, args.base_ref):
            chosen = artifact
            break
    if chosen is None:
        raise RuntimeError(f"no {artifact_name} artifact found for {args.base_ref}")
    run_id, sha = chosen["workflow_run"]["id"], chosen["workflow_run"]["head_sha"]
    log(f"{name} base build: run {run_id}, commit {sha}")
    download(args.repo, run_id, artifact_name, out / "artifact")
    unpack(out / "artifact" / f"{artifact_name}.zip", out / "dist")

    meta = {"sha": sha, "run_id": run_id, "exact": sha == args.base_sha, "requested_sha": args.base_sha}
    if runtime.get("runner"):
        try:
            download(args.repo, run_id, RUNNER_ARTIFACT, out / "runner")
            meta["runner"] = "base"
        except subprocess.CalledProcessError:
            # Builds from before the runner was uploaded. The runner is built
            # from the pinned N-API revision, so the PR's runner is the same
            # program when both pin the same revision.
            base_pin, head_pin = submodule_pin(args.repo, sha), submodule_pin(args.repo, args.head_sha)
            meta["runner"] = "head-same-napi" if base_pin == head_pin else "head-different-napi"
            (out / "runner").mkdir(parents=True, exist_ok=True)
            shutil.copy2(head_runner, out / "runner" / "napi_wasmer")
        (out / "runner" / "napi_wasmer").chmod(0o755)
    return meta


def cmd_prepare(args):
    """<out>/head/<runtime> and <out>/base/<runtime>, plus <out>/builds.json."""
    out, artifacts = Path(args.out), Path(args.artifacts)
    out.mkdir(parents=True, exist_ok=True)

    def prepare(name):
        runtime = RUNTIMES[name]
        head = out / "head" / name
        archive = artifacts / runtime["artifact"] / f"{runtime['artifact']}.zip"
        if not archive.exists():
            raise RuntimeError(f"this run has no {runtime['artifact']} artifact (its build failed?)")
        unpack(archive, head / "dist")
        head_runner = head / "runner" / "napi_wasmer"
        if runtime.get("runner"):
            source = artifacts / RUNNER_ARTIFACT / "napi_wasmer"
            if not source.exists():
                raise RuntimeError(f"this run has no {RUNNER_ARTIFACT} artifact")
            head_runner.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(source, head_runner)
            head_runner.chmod(0o755)
        return fetch_base(args, name, head_runner, out / "base" / name)

    names = [name for name, runtime in RUNTIMES.items() if "artifact" in runtime]
    builds = {}
    with ThreadPoolExecutor(len(names)) as pool:
        for name, future in [(name, pool.submit(prepare, name)) for name in names]:
            try:
                builds[name] = {"meta": future.result()}
            except Exception as error:  # noqa: BLE001 - reported per runtime
                log(f"{name}: {error}")
                builds[name] = {"error": str(error)}
    (out / "builds.json").write_text(json.dumps(builds, indent=2) + "\n")


# --- run --------------------------------------------------------------------


def lane_for(runtime, location):
    """Harness lane for one build. `location` is the node binary for Node.js
    lanes, else a directory holding dist/ (and runner/ for V8 WASIX)."""
    root = Path(location).resolve()
    dist = root / "dist"
    if runtime == "v8-wasix":
        return {"runner": str(root / "runner" / "napi_wasmer"), "guest": str(dist / "bin" / "edge"),
                "builtins": str(dist / "node-lib"), "mounts": {str(dist / "icu"): "/usr/share/edge-icu"}}
    if runtime in ("v8-native", "quickjs-native"):
        return {"binary": str(dist / "bin" / "edge")}
    if runtime == "quickjs-wasix":
        return {"package": str(dist), "wasmer": os.environ.get("WASMER_BIN", "wasmer")}
    return {"binary": str(root), "node": True}


def cpu_halves():
    """Two disjoint CPU sets, or None where they cannot be pinned."""
    if not hasattr(os, "sched_getaffinity"):
        return None
    cpus = sorted(os.sched_getaffinity(0))
    if len(cpus) < 4:
        return None
    half = len(cpus) // 2
    return set(cpus[:half]), set(cpus[half:])


def cpu_list():
    return sorted(os.sched_getaffinity(0)) if hasattr(os, "sched_getaffinity") else list(range(os.cpu_count() or 2))


def pin_process(pid, cpus):
    """Moves every thread of a running process to `cpus`."""
    if cpus is None:
        return
    for task in Path(f"/proc/{pid}/task").iterdir():
        try:
            os.sched_setaffinity(int(task.name), cpus)
        except (ProcessLookupError, FileNotFoundError):
            pass  # A thread that just exited.


def machine_info():
    info = {"cpus": len(os.sched_getaffinity(0)) if hasattr(os, "sched_getaffinity") else os.cpu_count()}
    try:
        for line in Path("/proc/cpuinfo").read_text().splitlines():
            if line.startswith("model name"):
                info["cpu_model"] = line.split(":", 1)[1].strip()
                break
        for line in Path("/proc/meminfo").read_text().splitlines():
            if line.startswith("MemTotal"):
                info["memory_gb"] = round(int(line.split()[1]) / 1024 / 1024)
                break
    except OSError:
        pass
    return info


def touch_files(root):
    """Reads a tree once, so the first server start is not the one that pays
    for a cold page cache."""
    for path in Path(root).rglob("*"):
        try:
            if path.is_file():
                with open(path, "rb") as file:
                    while file.read(1 << 20):
                        pass
        except OSError:
            pass


class ServerHung(RuntimeError):
    """A server that neither answered nor exited in time."""


class LoadStream:
    """A load generator that keeps running and reports windows on request."""

    def __init__(self, bench, port):
        harness, config = bench.harness, bench.config
        command = [config["node"], str(BENCH / "bench-framework-stream.cjs"), str(harness.autocannon_dir(config)),
                   f"http://{harness.HOST}:{port}{harness.ROUTE}", str(CONNECTIONS), harness.BODY.decode()]
        self.process = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                        stderr=subprocess.DEVNULL, start_new_session=True)
        self.buffer = b""
        bench.track(self.process)

    def read(self, timeout):
        deadline = time.monotonic() + timeout
        descriptor = self.process.stdout.fileno()
        while b"\n" not in self.buffer:
            left = deadline - time.monotonic()
            if left <= 0 or not select.select([descriptor], [], [], left)[0]:
                raise ServerHung(f"no reply from the server within {timeout:g} s")
            chunk = os.read(descriptor, 65536)
            if not chunk:
                raise RuntimeError("the load generator exited")
            self.buffer += chunk
        line, self.buffer = self.buffer.split(b"\n", 1)
        return json.loads(line)

    def mark(self):
        self.process.stdin.write(b"mark\n")
        self.process.stdin.flush()
        return self.read(10)


class Bench:
    """Runs the cells: one runtime and fixture, both lanes at the same time."""

    def __init__(self, harness, config, out):
        self.harness, self.config, self.out = harness, config, out
        (out / "logs").mkdir(parents=True, exist_ok=True)
        self.sequence = 0
        self.lock = threading.Lock()
        self.processes = set()

    def path(self, directory, lane, fixture, suffix):
        with self.lock:
            self.sequence += 1
            return self.out / directory / f"{lane}-{fixture}-{self.sequence}{suffix}"

    def track(self, process):
        with self.lock:
            self.processes.add(process.pid)

    def stop(self, process):
        self.harness.kill_group(process.pid)
        with self.lock:
            self.processes.discard(process.pid)

    def kill_all(self):
        """The hard limit: killing the processes unblocks whatever waits on them."""
        with self.lock:
            pids = list(self.processes)
        for pid in pids:
            self.harness.kill_group(pid)

    def start(self, lane, fixture):
        log_path = self.path("logs", lane, fixture, ".log")
        process, port, started, _ = self.harness.spawn_server(self.config, lane, fixture, log_path)
        self.track(process)
        try:
            _, first_ms = self.harness.wait_ready(process, port, started, timeout=START_TIMEOUT)
        except Exception as error:
            self.stop(process)
            tail = "\n".join(log_path.read_text(errors="replace").strip().splitlines()[-8:])
            hung = isinstance(error, (TimeoutError, socket.timeout)) or "timeout" in str(error)
            kind = ServerHung if hung else RuntimeError
            raise kind(f"no reply within {START_TIMEOUT} s: {error}\n{tail}" if kind is ServerHung
                       else f"{error}\n{tail}") from error
        return process, port, first_ms

    def lane(self, lane, fixture, pins, warmup, seconds, barrier, result):
        """One lane of a cell: a timed start, then a fresh server that is
        warmed up and measured on each CPU half under one continuous load."""
        process = load = None

        def on(cpus):
            if cpus is not None:
                os.sched_setaffinity(0, cpus)  # This thread, and what it starts.
            barrier.wait(timeout=45)

        try:
            on(pins[0])
            probe, _, first_ms = self.start(lane, fixture)
            self.stop(probe)
            result["first_ms"].append(first_ms)

            on(pins[1])
            process, port, first_ms = self.start(lane, fixture)
            result["first_ms"].append(first_ms)
            barrier.wait(timeout=45)
            load = LoadStream(self, port)
            load.read(START_TIMEOUT)
            time.sleep(warmup)
            for cpus in (pins[1], pins[0]):
                pin_process(process.pid, cpus)
                pin_process(load.process.pid, cpus)
                barrier.wait(timeout=45)
                load.mark()  # Drops what came before this window.
                time.sleep(seconds)
                sample = load.mark()
                if sample["requests"] == 0:
                    raise ServerHung(f"no replies in a {seconds:.2f} s window")
                if not sample["ok"]:
                    raise RuntimeError(f"bad replies: {sample}")
                result["runs"].append({"req_per_sec": sample["req_per_sec"], "requests": sample["requests"],
                                       "p99_ms": sample["latency_ms"]["p99"]})
        except threading.BrokenBarrierError:
            result["error"] = "stopped because the other lane failed"
            result["stopped"] = True
        except Exception as error:  # noqa: BLE001 - reported per cell
            result["error"] = str(error)[-1500:] or type(error).__name__
            result["hung"] = isinstance(error, (ServerHung, subprocess.TimeoutExpired))
            barrier.abort()
        finally:
            if load is not None:
                self.stop(load.process)
            if process is not None:
                self.stop(process)

    def cell(self, runtime, fixture, halves, warmup, seconds, hard_deadline):
        barrier = threading.Barrier(2)
        pins = {"head": (halves[0], halves[1]) if halves else (None, None),
                "base": (halves[1], halves[0]) if halves else (None, None)}
        results = {side: {"first_ms": [], "runs": [], "error": None} for side in SIDES}
        threads = [threading.Thread(target=self.lane, args=(f"{runtime}-{side}", fixture, pins[side], warmup,
                                                            seconds, barrier, results[side]))
                   for side in SIDES]
        watchdog = threading.Timer(max(0.1, hard_deadline - time.monotonic()), self.kill_all)
        watchdog.start()
        for thread in threads:
            thread.start()
        for thread in threads:
            thread.join()
        watchdog.cancel()
        return results

    def calibrate(self, names, fixtures):
        """Starts every server once, several at a time. This absorbs first-start
        effects, finds broken servers before any time is planned around them,
        and measures what each cell's server starts will cost.

        Returns ({(runtime, fixture): seconds}, {(runtime, fixture): error},
        {runtimes that hung})."""
        costs, broken, hung = {}, {}, set()

        def one(name, side, fixture):
            if name in hung:
                return
            try:
                process, _, first_ms = self.start(f"{name}-{side}", fixture)
                self.stop(process)
                with self.lock:
                    costs[(name, fixture)] = max(costs.get((name, fixture), 0.0), first_ms / 1000)
            except Exception as error:  # noqa: BLE001 - reported per cell
                lane = "PR" if side == "head" else "base"
                with self.lock:
                    broken[(name, fixture)] = f"{lane} build: {str(error).splitlines()[0][:160]}"
                    if isinstance(error, ServerHung):
                        hung.add(name)

        jobs = [(name, side, fixture) for fixture in fixtures for name in names for side in SIDES]
        with ThreadPoolExecutor(max(2, len(cpu_list()))) as pool:
            for future in [pool.submit(one, *job) for job in jobs]:
                future.result()
        return costs, broken, hung


def identify(harness, config, lane, timeout):
    """Runs the lane once: this compiles a WASIX guest into its cache."""
    command, extra_env = harness.guest_command(config["lanes"][lane], ("-p", "JSON.stringify(process.versions)"))
    done = subprocess.run(command, capture_output=True, text=True, timeout=timeout,
                          env={**harness.host_env(config, lane), **extra_env})
    if done.returncode != 0:
        raise RuntimeError((done.stdout + done.stderr).strip()[-1000:])
    versions = json.loads(done.stdout.strip().splitlines()[-1])
    return {"edge": versions.get("edge"), "node": versions.get("node"), "v8": versions.get("v8")}


def cmd_run(args):
    started = time.monotonic()
    deadline = started + args.budget
    out = Path(args.out).resolve()
    out.mkdir(parents=True, exist_ok=True)
    builds_root = Path(args.builds).resolve()
    builds_path = builds_root / "builds.json"
    builds = json.loads(builds_path.read_text()) if builds_path.exists() else {}
    runtimes = {}
    lanes = {}
    for name, runtime in RUNTIMES.items():
        if runtime.get("reference"):
            binary = getattr(args, name)
            entry = {"meta": {}} if binary else {"error": "no binary given"}
            locations = {side: binary for side in SIDES}
        else:
            entry = builds.get(name, {"error": "not prepared"})
            locations = {side: builds_root / side / name for side in SIDES}
        runtimes[name] = {**entry, "versions": {}}
        if "error" not in entry:
            for side in SIDES:
                lanes[f"{name}-{side}"] = lane_for(name, locations[side])

    harness = load_harness()
    config_path = out / "lanes.json"
    config_path.write_text(json.dumps({"state": str(out / "state"), "lanes": lanes}, indent=2))
    config = harness.load_config(config_path)
    (out / "state").mkdir(parents=True, exist_ok=True)

    # Compile every guest up front, using all the CPUs.
    def warm(lane):
        return lane, identify(harness, config, lane, max(5.0, deadline - time.monotonic()))

    with ThreadPoolExecutor(max(1, len(lanes))) as pool:
        for lane, future in [(lane, pool.submit(warm, lane)) for lane in lanes]:
            name, side = lane.rsplit("-", 1)
            try:
                runtimes[name]["versions"][side] = future.result()[1]
            except Exception as error:  # noqa: BLE001 - reported per runtime
                log(f"{lane}: {error}")
                runtimes[name]["error"] = f"{side} build did not start: {str(error)[-400:]}"
    touch_files(config["fixtures"])
    warm_s = time.monotonic() - started
    log(f"warm-up took {warm_s:.0f} s")

    names = [name for name in RUNTIMES if "error" not in runtimes[name]]
    fixtures = list(FIXTURE_NAMES)
    halves = cpu_halves()
    bench = Bench(harness, config, out)
    cells, notes = [], []
    hard_deadline = started + args.hard_limit
    calibrate_s = 0.0

    def save():
        """After every cell, so even a killed step leaves what it measured."""
        data = {"machine": {**machine_info(), "cpu_pinning": bool(halves)},
                "params": {"warmup_s": args.warmup, "connections": CONNECTIONS, "budget_s": args.budget},
                "runtimes": runtimes, "cells": cells, "notes": notes,
                "warmup_elapsed_s": warm_s, "calibrate_elapsed_s": calibrate_s,
                "elapsed_s": time.monotonic() - started}
        temporary = out / "results.json.tmp"
        temporary.write_text(json.dumps(data, indent=2) + "\n")
        os.replace(temporary, out / "results.json")
        return data

    save()
    calibrate_started = time.monotonic()
    costs, broken, hung = bench.calibrate(names, fixtures)
    for (name, fixture), error in broken.items():
        notes.append(f"{RUNTIMES[name]['title']} {FIXTURE_NAMES[fixture]} failed to start ({error})")
    for name in hung:
        notes.append(f"{RUNTIMES[name]['title']}: not measured, a server hung")
    names = [name for name in names if name not in hung]
    # Let the compile caches and logs reach the disk, so their writeback does
    # not land on the first measurements.
    os.sync()
    calibrate_s = time.monotonic() - calibrate_started
    log(f"calibration took {calibrate_s:.0f} s")

    pending = [(name, fixture) for fixture in fixtures for name in names if (name, fixture) not in broken]

    def cell_cost(cell):
        return args.warmup + 2 * costs.get(cell, 1.0) + args.overhead

    stop = False
    for fixture in fixtures:
        # One measuring time per fixture, so its rows compare like for like,
        # from what is left after every remaining cell's starts and warm-up.
        left = deadline - time.monotonic() - sum(cell_cost(cell) for cell in pending)
        seconds = min(args.max_seconds, max(args.min_seconds, left / max(1, 2 * len(pending))))
        for name in list(names):
            if (name, fixture) not in pending:
                continue
            if time.monotonic() + cell_cost((name, fixture)) + 2 * seconds > deadline + args.grace:
                notes.append(f"stopped at the {args.budget:.0f} s budget before {RUNTIMES[name]['title']} "
                             f"{FIXTURE_NAMES[fixture]}")
                stop = True
                break
            cell_started = time.monotonic()
            results = bench.cell(name, fixture, halves, args.warmup, seconds, hard_deadline)
            pending.remove((name, fixture))
            errors = {side: r["error"] for side, r in results.items() if r["error"]}
            for side, error in errors.items():
                log(f"{name} {fixture} {side}: {error}")
            causes = {side: r for side, r in results.items() if r["error"] and not r.get("stopped")}
            if causes:
                side, cause = next(iter(causes.items()))
                lane_name = "PR" if side == "head" else "base"
                notes.append(f"{RUNTIMES[name]['title']} {FIXTURE_NAMES[fixture]} failed ({lane_name} build): "
                             + cause["error"].splitlines()[0][:160])
                # A runtime that hangs would use up the others' time.
                if any(r.get("hung") for r in causes.values()):
                    later = [cell for cell in pending if cell[0] == name]
                    pending = [cell for cell in pending if cell[0] != name]
                    names.remove(name)
                    if later:
                        notes.append(f"{RUNTIMES[name]['title']}: skipped its remaining servers after a hang")
            cells.append({"runtime": name, "fixture": fixture, "seconds": seconds, "lanes": results,
                          "elapsed_s": time.monotonic() - cell_started})
            log(f"{name:15} {fixture:8} {time.monotonic() - cell_started:4.1f} s  "
                + "  ".join(f"{side} " + ", ".join(f"{run['req_per_sec']:,.0f}" for run in r["runs"])
                            for side, r in results.items()))
            save()
        if stop:
            break

    data = save()
    log(f"benchmark took {data['elapsed_s']:.0f} s")
    if not any(cell["lanes"]["head"]["runs"] and cell["lanes"]["base"]["runs"] for cell in cells):
        sys.exit(1)


# --- report -----------------------------------------------------------------


def geometric_mean(values):
    return math.exp(statistics.mean(math.log(value) for value in values))


def agreement(changes, bar, higher_is_better=True):
    """better/worse only when every paired measurement moves the same way by
    more than the bar (twice the bar with a single measurement)."""
    limit = bar if len(changes) > 1 else 2 * bar
    if changes and all(c > limit for c in changes):
        return "better" if higher_is_better else "worse"
    if changes and all(c < -limit for c in changes):
        return "worse" if higher_is_better else "better"
    return "same"


VERDICT_TEXT = {"same": "≈ no significant change", "better": "🟢 improved", "worse": "🔴 regressed"}
MARKS = {"better": " 🟢", "worse": " 🔴", "same": ""}


def paired_changes(cell):
    """The PR/base differences of a cell's paired runs and starts."""
    head, base = cell["lanes"]["head"], cell["lanes"]["base"]
    runs = [h["req_per_sec"] / b["req_per_sec"] - 1 for h, b in zip(head["runs"], base["runs"])]
    starts = [h / b - 1 for h, b in zip(head["first_ms"], base["first_ms"])]
    return runs, starts


def noise_bars(data):
    """{fixture: (throughput bar, startup bar)} from the Node.js A/A cells."""
    largest = {fixture: [0.0, 0.0] for fixture in FIXTURE_NAMES}
    for cell in data.get("cells", []):
        if RUNTIMES[cell["runtime"]].get("reference"):
            runs, starts = paired_changes(cell)
            entry = largest[cell["fixture"]]
            entry[0] = max([entry[0]] + [abs(c) for c in runs])
            entry[1] = max([entry[1]] + [abs(c) for c in starts])
    return {fixture: (max(THROUGHPUT_FLOOR, NOISE_FACTOR * runs), max(STARTUP_FLOOR, NOISE_FACTOR * starts))
            for fixture, (runs, starts) in largest.items()}


def analyze_cell(cell, bars):
    head, base = cell["lanes"]["head"], cell["lanes"]["base"]
    row = {"seconds": cell["seconds"]}
    runs, starts = paired_changes(cell)
    # Each pair of measurements ran at the same time, on opposite CPU halves.
    pairs = list(zip(head["runs"], base["runs"]))
    if pairs and not head["error"] and not base["error"]:
        row.update(base=statistics.mean(b["req_per_sec"] for _, b in pairs),
                   head=statistics.mean(h["req_per_sec"] for h, _ in pairs),
                   change=geometric_mean([1 + c for c in runs]) - 1, round_changes=runs,
                   verdict=agreement(runs, bars[0]),
                   p99_base=statistics.median(b["p99_ms"] for _, b in pairs),
                   p99_head=statistics.median(h["p99_ms"] for h, _ in pairs))
    first_pairs = list(zip(head["first_ms"], base["first_ms"]))
    if first_pairs:
        row.update(first_base=statistics.median(b for _, b in first_pairs),
                   first_head=statistics.median(h for h, _ in first_pairs),
                   first_change=geometric_mean([1 + c for c in starts]) - 1, first_pass_changes=starts,
                   first_verdict=agreement(starts, bars[1], higher_is_better=False))
    return row


def analyze(data):
    """{runtime: {"rows": {fixture: row}, "meta": ..., "error": ...}}"""
    found = {name: {"rows": {}, "meta": (data.get("runtimes", {}).get(name) or {}).get("meta") or {},
                    "error": (data.get("runtimes", {}).get(name) or {}).get("error")} for name in RUNTIMES}
    bars = noise_bars(data)
    for cell in data.get("cells", []):
        row = analyze_cell(cell, bars[cell["fixture"]])
        if "head" in row or "first_head" in row:
            found[cell["runtime"]]["rows"][cell["fixture"]] = row
    return found


def short(sha):
    return sha[:9] if sha else "?"


def read_history(body):
    for line in (body or "").splitlines():
        if line.startswith(HISTORY_PREFIX) and line.endswith(" -->"):
            try:
                return json.loads(base64.b64decode(line[len(HISTORY_PREFIX):-4]))
            except ValueError:
                return []
    return []


def throughput_cell(row, reference):
    if not row or "head" not in row:
        return "–"
    if reference:
        return f"{row['head']:,.0f} <sub>(A/A {row['change']:+.1%})</sub>"
    return f"{row['head']:,.0f} <sub>({row['change']:+.1%})</sub>{MARKS[row['verdict']]}"


def startup_cell(row, reference):
    if not row or "first_head" not in row:
        return "–"
    if reference:
        return f"{row['first_head']:.0f} <sub>(A/A {row['first_change']:+.0%})</sub>"
    return f"{row['first_head']:.0f} <sub>({row['first_change']:+.0%})</sub>{MARKS[row['first_verdict']]}"


def runtime_table(name, entry, args):
    """The per-server table of one runtime, with its notes."""
    reference = RUNTIMES[name].get("reference")
    meta, rows = entry["meta"], entry["rows"]
    repo_url = f"https://github.com/{args.repo}"
    if reference:
        lines = ["Both lanes run the same Node.js binary, so their difference is the noise of this run.", ""]
    else:
        base = f"[`{short(meta.get('sha'))}`]({repo_url}/commit/{meta.get('sha')})" if meta.get("sha") else "?"
        lines = [f"Base build: `{args.base_ref}` at {base}.", ""]
    if rows:
        lines += ["| Server | Base req/s | PR req/s | Change | Verdict | p99 base → PR "
                  "| First reply base → PR | Verdict |",
                  "| --- | ---: | ---: | ---: | --- | --- | --- | --- |"]
        for fixture in FIXTURE_NAMES:
            r = rows.get(fixture)
            if not r:
                continue
            if "head" in r:
                per_run = ", ".join(f"{c:+.1%}" for c in r["round_changes"])
                throughput = (f"{r['base']:,.0f} | {r['head']:,.0f} | {r['change']:+.1%} <sub>({per_run})</sub> | "
                              + ("reference" if reference else VERDICT_TEXT[r["verdict"]])
                              + f" | {r['p99_base']:.2f} → {r['p99_head']:.2f} ms")
            else:
                throughput = "– | – | – | no result | –"
            if "first_head" in r:
                per_start = ", ".join(f"{c:+.0%}" for c in r["first_pass_changes"])
                startup = (f"{r['first_base']:.0f} → {r['first_head']:.0f} ms ({r['first_change']:+.0%} "
                           f"<sub>({per_start})</sub>) | "
                           + ("reference" if reference else VERDICT_TEXT[r["first_verdict"]]))
            else:
                startup = "– | no result"
            lines.append(f"| {FIXTURE_NAMES[fixture]} | {throughput} | {startup} |")
        lines.append("")
    notes = []
    if entry["error"]:
        notes.append(f"⚠️ {entry['error']}")
    if meta and not meta.get("exact", True):
        notes.append(f"`{args.base_ref}` at `{short(meta.get('requested_sha'))}` has no finished build yet, "
                     f"so the base is the newest finished one, `{short(meta.get('sha'))}`.")
    if meta.get("runner") == "head-same-napi":
        notes.append("The base build has no N-API runner artifact; it ran on the PR's runner, which is "
                     "built from the same pinned N-API revision.")
    elif meta.get("runner") == "head-different-napi":
        notes.append("⚠️ The base build has no N-API runner artifact and pins a different N-API revision; "
                     "it ran on the PR's runner, so N-API changes are not part of this comparison.")
    return lines + [f"- {note}" for note in notes] + ([""] if notes else [])


def findings(analysis):
    """Significant changes across the Edge.js runtimes, as headline lines."""
    found = {"worse": [], "better": [], "slower": [], "faster": []}
    measured = []
    for name, entry in analysis.items():
        if not entry["rows"] or RUNTIMES[name].get("reference"):
            continue
        measured.append(name)
        for fixture, row in entry["rows"].items():
            label = f"{RUNTIMES[name]['title']} {FIXTURE_NAMES[fixture]}"
            if row.get("verdict") in ("worse", "better"):
                found[row["verdict"]].append(f"{label} ({row['change']:+.1%})")
            if row.get("first_verdict") in ("worse", "better"):
                found["slower" if row["first_verdict"] == "worse" else "faster"].append(
                    f"{label} ({row['first_change']:+.0%})")
    lines = []
    if found["worse"]:
        lines.append("🔴 **Throughput regressed:** " + "; ".join(found["worse"]) + ".")
    if found["slower"]:
        lines.append("🔴 **Slower time to first response:** " + "; ".join(found["slower"]) + ".")
    if found["better"]:
        lines.append("🟢 **Throughput improved:** " + "; ".join(found["better"]) + ".")
    if found["faster"]:
        lines.append("🟢 **Faster time to first response:** " + "; ".join(found["faster"]) + ".")
    if measured and not any(found.values()):
        edge_count = sum(not r.get("reference") for r in RUNTIMES.values())
        lines.append("**No significant change** in throughput or time to first response in "
                     + ("any Edge.js runtime." if len(measured) == edge_count
                        else ", ".join(RUNTIMES[n]["title"] for n in measured) + "."))
    return lines


def method_text(data):
    params, machine = data["params"], data["machine"]
    seconds = sorted({cell["seconds"] for cell in data["cells"]})
    window = f"{seconds[0]:.1f}–{seconds[-1]:.1f} s" if len(seconds) > 1 else f"{seconds[0]:.1f} s" if seconds else "?"
    pinning = ("each pinned to one half of the CPUs (server and load generator together)"
               if machine.get("cpu_pinning") else "sharing all CPUs, as pinning was not available")
    return (f"One job measures every runtime on one machine. For each runtime and server, the two lanes run at "
            f"the same time, {pinning}. Each lane starts the server twice, on opposite halves, to time process "
            "start to the first validated response, warms the second server up for "
            f"{params['warmup_s']:g} s with {params['connections']} connections, and times it for {window} on "
            "each half under one continuous load, with every reply validated; the lanes swap halves between "
            "the two measurements. Every server is started once before measuring, which is not counted. A "
            f"change counts only when both measurements move the same way by more than {THROUGHPUT_FLOOR:.0%} "
            f"(throughput) or {STARTUP_FLOOR:.0%} (first response), or by more than {NOISE_FACTOR:g} times the "
            "largest difference Node.js shows against itself on that server in the same run, if that is "
            "higher. These windows are short: confirm small changes locally with "
            "`benchmarks/bench-framework-wasix-hosts.py`.")


def render(args, data, history):
    repo_url = f"https://github.com/{args.repo}"
    head_link = f"[`{short(args.head_sha)}`]({repo_url}/commit/{args.head_sha})"
    run_link = f"[workflow run]({repo_url}/actions/runs/{args.run_id})" if args.run_id else "workflow run"
    analysis = analyze(data)
    base_shas = sorted({e["meta"]["sha"] for e in analysis.values() if e["meta"].get("sha")})
    base_text = ", ".join(f"[`{short(s)}`]({repo_url}/commit/{s})" for s in base_shas) or "?"
    lines = [MARKER, "## Framework benchmarks", ""]
    lines.append(f"PR {head_link} (merged into `{args.base_ref}`, as CI builds it) against `{args.base_ref}` "
                 f"at {base_text}. {run_link}.")
    lines.append("")
    if not data.get("cells"):
        lines += ["⚠️ **The benchmark produced no results.** See the workflow run.", ""]
    lines += findings(analysis)
    missing = [RUNTIMES[n]["title"] for n, e in analysis.items() if not e["rows"]]
    if missing and data.get("cells"):
        lines.append("⚠️ **No results** for " + ", ".join(missing) + "; see the workflow run.")
    lines.append("")
    if not data.get("cells"):
        reasons = [f"{RUNTIMES[n]['title']}: {e['error']}" for n, e in analysis.items() if e["error"]]
        reasons += data.get("notes", [])
        if reasons:
            lines += ["**Notes**", ""] + [f"- {reason}" for reason in reasons] + [""]

    if data.get("cells"):
        header = ["| Runtime | " + " | ".join(FIXTURE_NAMES.values()) + " |",
                  "| --- | " + " | ".join("---:" for _ in FIXTURE_NAMES) + " |"]
        for title, cell in (("**Requests per second** (PR, change against the base build):", throughput_cell),
                            ("**Time to first response, ms** (PR, change against the base build):", startup_cell)):
            lines += [title, ""] + header
            for name, entry in analysis.items():
                cells = [cell(entry["rows"].get(f), RUNTIMES[name].get("reference")) for f in FIXTURE_NAMES]
                lines.append(f"| {RUNTIMES[name]['title']} | " + " | ".join(cells) + " |")
            lines.append("")
        machine = data["machine"]
        described = ", ".join(filter(None, (machine.get("cpu_model"), f"{machine.get('cpus')} vCPUs",
                                            f"{machine['memory_gb']} GB" if machine.get("memory_gb") else "")))
        lines += [f"All runtimes ran on the same machine ({described}) in {data['elapsed_s']:.0f} s. "
                  "Node.js rows compare a binary with itself and show the noise.", ""]
        if data.get("notes"):
            lines += ["**Notes**", ""] + [f"- {note}" for note in data["notes"]] + [""]
        for name, entry in analysis.items():
            if entry["rows"] or entry["error"]:
                lines += [f"<details><summary><b>{RUNTIMES[name]['title']}</b></summary>", ""]
                lines += runtime_table(name, entry, args) + ["</details>", ""]
        lines += ["<details><summary>Method</summary>", "", method_text(data), "", "</details>", ""]

    entry = {"head": args.head_sha, "run": args.run_id,
             "changes": {name: {f: round(r["change"], 4) for f, r in e["rows"].items() if "change" in r}
                         for name, e in analysis.items() if not RUNTIMES[name].get("reference")},
             "first": {name: {f: round(r["first_change"], 4) for f, r in e["rows"].items() if "first_change" in r}
                       for name, e in analysis.items() if not RUNTIMES[name].get("reference")}}
    # A run that measured nothing must not replace a commit's earlier numbers.
    if any(entry["changes"].values()) or any(entry["first"].values()):
        history = [h for h in history if h.get("head") != args.head_sha and "changes" in h][-9:] + [entry]
    if len(history) > 1:
        lines += ["<details><summary>Earlier runs on this PR</summary>", "",
                  "Change against the base build: requests per second / time to first response.", "",
                  "| PR commit | Runtime | " + " | ".join(FIXTURE_NAMES.values()) + " |",
                  "| --- | --- | " + " | ".join("---:" for _ in FIXTURE_NAMES) + " |"]
        for h in reversed(history):
            for name, changes in h["changes"].items():
                if not isinstance(changes, dict) or name not in RUNTIMES:
                    continue
                first = h.get("first", {}).get(name, {})
                cells = [" / ".join(filter(None, (f"{changes[f]:+.1%}" if f in changes else "",
                                                  f"{first[f]:+.0%}" if f in first else "")))
                         for f in FIXTURE_NAMES]
                lines.append(f"| `{short(h['head'])}` | {RUNTIMES[name]['title']} | " + " | ".join(cells) + " |")
        lines += ["", "</details>", ""]
    lines.append(f"<sub>Updated for {head_link}.</sub>")
    if args.post:
        lines.append(HISTORY_PREFIX + base64.b64encode(json.dumps(history).encode()).decode() + " -->")
    return "\n".join(lines) + "\n"


def find_comment(repo, pr):
    comments = json.loads(gh("api", "--paginate", "--slurp", f"repos/{repo}/issues/{pr}/comments"))
    for comment in (c for page in comments for c in page):
        if MARKER in (comment.get("body") or "") and comment["user"]["type"] == "Bot":
            return comment
    return None


def cmd_report(args):
    path = Path(args.results)
    data = json.loads(path.read_text()) if path.exists() else {}
    existing = find_comment(args.repo, args.pr) if args.pr and args.post else None
    body = render(args, data, read_history(existing and existing["body"]))
    if args.out:
        Path(args.out).write_text(body)
    if os.environ.get("GITHUB_STEP_SUMMARY"):
        with open(os.environ["GITHUB_STEP_SUMMARY"], "a") as summary:
            summary.write(body)
    if not args.post:
        print(body)
        return
    payload = json.dumps({"body": body})
    if existing:
        gh("api", "-X", "PATCH", f"repos/{args.repo}/issues/comments/{existing['id']}", "--input", "-",
           input=payload)
        log(f"updated comment {existing['html_url']}")
    else:
        created = json.loads(gh("api", "-X", "POST", f"repos/{args.repo}/issues/{args.pr}/comments",
                                "--input", "-", input=payload))
        log(f"created comment {created['html_url']}")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)
    prepare = sub.add_parser("prepare")
    prepare.add_argument("--artifacts", required=True, help="this run's artifacts, one directory each")
    prepare.add_argument("--repo", required=True)
    prepare.add_argument("--base-ref", required=True)
    prepare.add_argument("--base-sha", required=True)
    prepare.add_argument("--head-sha", required=True, help="commit whose N-API pin the PR's runner was built from")
    prepare.add_argument("--out", required=True)
    run = sub.add_parser("run")
    run.add_argument("--builds", required=True, help="the directory `prepare` wrote")
    run.add_argument("--node24", help="Node.js 24 binary")
    run.add_argument("--node26", help="Node.js 26 binary")
    run.add_argument("--out", required=True)
    run.add_argument("--budget", type=float, default=115, help="seconds for the whole benchmark")
    run.add_argument("--grace", type=float, default=5, help="seconds a last cell may run past the budget")
    run.add_argument("--hard-limit", type=float, default=125, help="seconds after which running servers are killed")
    run.add_argument("--warmup", type=float, default=0.75, help="load before measuring, seconds")
    run.add_argument("--min-seconds", type=float, default=0.5, help="shortest measurement")
    run.add_argument("--max-seconds", type=float, default=2.0, help="longest measurement")
    run.add_argument("--overhead", type=float, default=0.5, help="seconds per cell beyond starts and measuring")
    report = sub.add_parser("report")
    report.add_argument("--results", required=True)
    report.add_argument("--repo", required=True)
    report.add_argument("--pr", type=int)
    report.add_argument("--head-sha", required=True)
    report.add_argument("--base-ref", required=True)
    report.add_argument("--run-id")
    report.add_argument("--out")
    report.add_argument("--post", action="store_true")
    args = parser.parse_args()
    {"prepare": cmd_prepare, "run": cmd_run, "report": cmd_report}[args.command](args)


if __name__ == "__main__":
    main()
