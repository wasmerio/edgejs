#!/usr/bin/env python3
"""Pull request framework benchmarks: the PR's builds against its base branch.

Used by the `bench` and `bench-comment` jobs in
.github/workflows/test-and-build.yml. One `bench` job runs per runtime:

  v8-native, v8-wasix, quickjs-native, quickjs-wasix
              Edge.js: the PR's build (from this workflow run) against the
              newest finished build of the base branch.
  node24, node26
              Node.js references: the same binary in both lanes, which also
              measures the noise of the run (an A/A comparison).

Subcommands:

  fetch-base  Download the base branch's build of one runtime (and, for V8
              WASIX, its N-API runner).
  run         Benchmark both lanes at once on one runner, each pinned to its
              own half of the CPUs, swapping halves between rounds, within a
              time budget.
  comment     Compare every runtime's lanes, write one Markdown report and
              create or update the pull request's benchmark comment.

Running both lanes at the same time on one machine keeps each comparison
fair: GitHub runners differ by more than the changes being measured, and
every round compares the two builds under the same conditions.
"""

import argparse
import base64
import json
import math
import os
from pathlib import Path
import shutil
import signal
import statistics
import subprocess
import sys
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
# Throughput: a change counts only when every round moves the same way by
# more than this (twice this with a single round). Shared runners produce
# one-off outliers of 10-20% even between identical binaries, which an average
# of the rounds would report as a change.
THROUGHPUT_FLOOR = 0.03
# First-reply times: a change must exceed this and the spread of the probes.
STARTUP_FLOOR = 0.05


def gh(*args, **kwargs):
    return subprocess.run(["gh", *args], check=True, capture_output=True, text=True, **kwargs).stdout


def gh_json(*args):
    return json.loads(gh("api", *args))


def log(message):
    print(message, file=sys.stderr, flush=True)


# --- fetch-base -------------------------------------------------------------


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


def cmd_fetch_base(args):
    """Downloads one runtime's base build into args.out and records its origin."""
    runtime = RUNTIMES[args.runtime]
    artifact_name = runtime["artifact"]
    out = Path(args.out)
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
        sys.exit(f"no {artifact_name} artifact found for {args.base_ref}")
    run_id, sha = chosen["workflow_run"]["id"], chosen["workflow_run"]["head_sha"]
    log(f"{args.runtime} base build: run {run_id}, commit {sha}")
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
            meta["base_napi"], meta["head_napi"] = base_pin, head_pin
            (out / "runner").mkdir(parents=True, exist_ok=True)
            shutil.copy2(Path(args.head_runner), out / "runner" / "napi_wasmer")
        (out / "runner" / "napi_wasmer").chmod(0o755)
    (out / "meta.json").write_text(json.dumps(meta, indent=2) + "\n")
    print(json.dumps(meta))


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
    cpus = sorted(os.sched_getaffinity(0)) if hasattr(os, "sched_getaffinity") else []
    if len(cpus) < 4 or shutil.which("taskset") is None:
        return None
    half = len(cpus) // 2
    return ",".join(map(str, cpus[:half])), ",".join(map(str, cpus[half:]))


def harness(config, output, args, startup_runs, cpus, rounds=1):
    """The harness command for one lane: `rounds` throughput rounds, then
    `startup_runs` first-reply probes per fixture after one unmeasured start
    (0 skips either)."""
    command = [sys.executable, str(HARNESS), "--config", str(config),
               "--amount-scale", str(args.amount_scale), "--max-seconds", str(args.max_seconds),
               "run", "--rounds", str(rounds), "--runs", str(args.runs), "--startup-runs", str(startup_runs),
               "--startup-warmup", "1",
               "--idle", "0", "--attempts", "1", "--spread", "1", "--output", str(output)]
    if cpus:
        command = ["taskset", "-c", cpus, *command]
    return command


def kill_tree(process, state_dir):
    """Kills a harness started in its own session, and the servers it left:
    those run in sessions of their own, recorded in the cell state files."""
    try:
        os.killpg(process.pid, signal.SIGKILL)
    except ProcessLookupError:
        pass
    process.wait()
    for cell in Path(state_dir).glob("cells/*.json"):
        try:
            pid = json.loads(cell.read_text()).get("pid")
        except (ValueError, OSError):
            continue
        if pid:
            try:
                os.killpg(pid, signal.SIGKILL)
            except (ProcessLookupError, PermissionError):
                pass


def run_lanes(commands, out, name, deadline):
    """Runs the head and base commands at once until `deadline`. Returns
    {lane: "ok" | "failed" | "timed_out"}; logs go to <out>/<lane>-<name>.log."""
    processes = {}
    for lane, (command, _) in commands.items():
        log_file = open(out / f"{lane}-{name}.log", "w")
        processes[lane] = subprocess.Popen(command, stdout=log_file, stderr=subprocess.STDOUT,
                                           start_new_session=True)
    status = {}
    for lane, process in processes.items():
        try:
            process.wait(timeout=max(1.0, deadline - time.monotonic()))
            status[lane] = "ok" if process.returncode == 0 else "failed"
        except subprocess.TimeoutExpired:
            kill_tree(process, commands[lane][1])
            status[lane] = "timed_out"
    return status


def cmd_run(args):
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    configs, states = {}, {}
    for lane, location in (("head", args.head), ("base", args.base)):
        configs[lane] = out / f"{lane}.config.json"
        states[lane] = (out / "state" / lane).resolve()
        config = {"state": str(states[lane]), "warmup": args.warmup,
                  "lanes": {"lane": lane_for(args.runtime, location)}}
        configs[lane].write_text(json.dumps(config, indent=2))
    halves = cpu_halves()
    started = time.monotonic()
    deadline = started + args.budget + args.grace
    rounds, notes = [], []

    # Compile both guests (and check every server starts) before timing, so
    # the first round is not the one that pays for compilation.
    warm = {lane: ([*(["taskset", "-c", halves[i]] if halves else []), sys.executable, str(HARNESS),
                    "--config", str(configs[lane]), "smoke", "--requests", "200", "--fixtures", "http"],
                   states[lane]) for i, lane in enumerate(("head", "base"))}
    warm_status = run_lanes(warm, out, "warmup", deadline)
    warm_s = time.monotonic() - started
    log(f"warm-up took {warm_s:.0f} s: {warm_status}")
    if any(s != "ok" for s in warm_status.values()):
        notes.append("warm-up failed for " + ", ".join(l for l, s in warm_status.items() if s != "ok"))
        for lane, status in warm_status.items():
            if status != "ok":
                log(f"--- {lane} warm-up ({status}):\n"
                    + (out / f"{lane}-warmup.log").read_text(errors="replace")[-3000:])

    # Time to first response, measured first so the throughput budget can
    # never squeeze it out. Both lanes probe at the same time, each on its
    # half of the CPUs, in two passes that swap the halves.
    startup = None
    if not notes and args.startup_runs > 0:
        passes = []
        per_pass = max(2, (args.startup_runs + 1) // 2)
        startup_started = time.monotonic()
        for index in range(2):
            pins = {"head": halves[index], "base": halves[1 - index]} if halves else {}
            outputs = {lane: out / f"{lane}-startup{index + 1}.json" for lane in ("head", "base")}
            commands = {lane: (harness(configs[lane], outputs[lane], args, per_pass, pins.get(lane), rounds=0),
                               states[lane]) for lane in outputs}
            status = run_lanes(commands, out, f"startup{index + 1}", deadline)
            if not all(s == "ok" for s in status.values()):
                notes.append(f"first-reply pass {index + 1}: "
                             + ", ".join(f"{l} {s}" for l, s in status.items() if s != "ok"))
                break
            passes.append({"pins": pins, **{lane: json.loads(outputs[lane].read_text()) for lane in outputs}})
        if passes:
            startup = {"passes": passes}
            log(f"first-reply probes took {time.monotonic() - startup_started:.0f} s")

    last_round_s = 0.0
    for index in range(args.rounds if not any(n.startswith("warm-up") for n in notes) else 0):
        elapsed = time.monotonic() - started
        # The next round takes about as long as the last one.
        if rounds and elapsed + last_round_s > args.budget:
            notes.append(f"stopped after {len(rounds)} of {args.rounds} rounds to stay within "
                         f"{args.budget:.0f} s")
            break
        round_started = time.monotonic()
        # Swap the CPU halves every round, so neither lane keeps the better half.
        pins = {"head": halves[index % 2], "base": halves[1 - index % 2]} if halves else {}
        outputs = {lane: out / f"{lane}-round{index + 1}.json" for lane in ("head", "base")}
        commands = {lane: (harness(configs[lane], outputs[lane], args, 0, pins.get(lane)), states[lane])
                    for lane in outputs}
        status = run_lanes(commands, out, f"round{index + 1}", deadline)
        if all(s == "ok" for s in status.values()):
            rounds.append({"index": index + 1, "pins": pins,
                           "results": {lane: json.loads(outputs[lane].read_text()) for lane in outputs}})
            last_round_s = time.monotonic() - round_started
            log(f"round {index + 1} took {last_round_s:.0f} s")
            continue
        if all(s in ("ok", "timed_out") for s in status.values()):
            notes.append(f"round {index + 1} was stopped at the {args.budget + args.grace:.0f} s time limit"
                         + ("; results are from the rounds before it" if rounds else ""))
            rounds.append({"index": index + 1, "timed_out": True})
        else:
            failed = [lane for lane, s in status.items() if s == "failed"]
            for lane in failed:
                tail = (out / f"{lane}-round{index + 1}.log").read_text(errors="replace")[-2000:]
                log(f"{lane} round {index + 1} failed:\n{tail}")
            notes.append("round {} failed for {}".format(index + 1, " and ".join(failed)))
            rounds.append({"index": index + 1, "failed": failed})
        break
    startup_runs = sum(1 for p in (startup or {}).get("passes", [])) * max(2, (args.startup_runs + 1) // 2)
    summary = {"runtime": args.runtime, "startup": startup, "startup_runs": startup_runs,
               "rounds": rounds, "notes": notes,
               "elapsed_s": time.monotonic() - started, "warmup_elapsed_s": warm_s,
               "cpu_pinning": bool(halves), "max_seconds": args.max_seconds, "runs": args.runs,
               "warmup_s": args.warmup}
    (out / "results.json").write_text(json.dumps(summary, indent=2) + "\n")
    log(f"benchmark took {summary['elapsed_s']:.0f} s")
    # A failure, or no comparable round at all, fails the job; a round
    # stopped at the time limit after earlier ones finished does not.
    if not any("results" in r for r in rounds) or any("failed" in r for r in rounds):
        sys.exit(1)


# --- comment ----------------------------------------------------------------


def geometric_mean(values):
    return math.exp(statistics.mean(math.log(value) for value in values))


def throughput_verdict(round_changes):
    floor = THROUGHPUT_FLOOR if len(round_changes) > 1 else 2 * THROUGHPUT_FLOOR
    if round_changes and all(c > floor for c in round_changes):
        return "better"
    if round_changes and all(c < -floor for c in round_changes):
        return "worse"
    return "same"


def verdict(change, noise, floor, higher_is_better=True):
    if abs(change) < max(floor, noise):
        return "same"
    return "better" if (change > 0) == higher_is_better else "worse"


VERDICT_TEXT = {"same": "≈ no significant change", "better": "🟢 improved", "worse": "🔴 regressed"}


def analyze(results):
    """Per-fixture head/base comparison from the rounds that completed."""
    rounds = [r for r in results["rounds"] if "results" in r]
    rows = {}
    for fixture in FIXTURE_NAMES:
        ratios, head_rates, base_rates, head_p99, base_p99 = [], [], [], [], []
        for round_ in rounds:
            head = round_["results"]["head"]["throughput"].get(fixture, {}).get("lane")
            base = round_["results"]["base"]["throughput"].get(fixture, {}).get("lane")
            if not head or not base:
                continue
            h = [run["req_per_sec"] for r in head["rounds"] for run in r["runs"]]
            b = [run["req_per_sec"] for r in base["rounds"] for run in r["runs"]]
            head_rates += h
            base_rates += b
            head_p99 += [run["latency_ms"]["p99"] for r in head["rounds"] for run in r["runs"]]
            base_p99 += [run["latency_ms"]["p99"] for r in base["rounds"] for run in r["runs"]]
            ratios.append(statistics.mean(h) / statistics.mean(b))
        if not ratios:
            continue
        change = geometric_mean(ratios) - 1
        noise = (max(ratios) - min(ratios)) / 2 if len(ratios) > 1 else 0.0
        row = {"base": statistics.mean(base_rates), "head": statistics.mean(head_rates),
               "change": change, "noise": noise, "round_changes": [ratio - 1 for ratio in ratios],
               "verdict": throughput_verdict([ratio - 1 for ratio in ratios]),
               "p99_base": statistics.median(base_p99), "p99_head": statistics.median(head_p99)}
        rows[fixture] = row
    for fixture, startup in analyze_startup(results).items():
        rows.setdefault(fixture, {}).update(startup)
    return rows


def spread(samples):
    """Relative interquartile range of the probes: how much they disagree."""
    ordered = sorted(samples)
    if len(ordered) < 3:
        return 0.0
    low, high = ordered[len(ordered) // 4], ordered[(3 * len(ordered)) // 4]
    return (high - low) / statistics.median(ordered)


def analyze_startup(results):
    """Per fixture: median time from process start to the first validated
    response for both lanes, and whether it changed. Each pass swaps the CPU
    halves; a change counts only when every pass moves the same way by more
    than STARTUP_FLOOR (twice that with one pass) and the overall change
    exceeds the probes' spread."""
    phase = results.get("startup") or {}
    passes = phase.get("passes") or ([phase] if phase else [])
    found = {}
    for fixture in FIXTURE_NAMES:
        samples = {"head": [], "base": []}
        pass_changes = []
        for one in passes:
            medians = {}
            for lane in ("head", "base"):
                cell = (one.get(lane) or {}).get("startup", {}).get(fixture, {}).get("lane")
                if cell and cell.get("first_response_ms"):
                    samples[lane] += cell["first_response_ms"]
                    medians[lane] = statistics.median(cell["first_response_ms"])
            if len(medians) == 2:
                pass_changes.append(medians["head"] / medians["base"] - 1)
        if not pass_changes:
            continue
        head, base = statistics.median(samples["head"]), statistics.median(samples["base"])
        change = head / base - 1
        noise = max(spread(samples["head"]), spread(samples["base"]))
        floor = STARTUP_FLOOR if len(pass_changes) > 1 else 2 * STARTUP_FLOOR
        if all(c < -floor for c in pass_changes) and abs(change) > noise:
            first_verdict = "better"
        elif all(c > floor for c in pass_changes) and abs(change) > noise:
            first_verdict = "worse"
        else:
            first_verdict = "same"
        found[fixture] = {"first_base": base, "first_head": head, "first_change": change,
                          "first_noise": noise, "first_pass_changes": pass_changes,
                          "first_verdict": first_verdict}
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


def load_runtimes(results_dir):
    """{runtime: {"results": ..., "meta": ..., "rows": ...}} for every runtime
    that left a results.json; missing ones map to None."""
    found = {}
    for name in RUNTIMES:
        # A directory per runtime, named after it or after its artifact.
        directory = Path(results_dir) / name
        if not directory.exists():
            directory = Path(results_dir) / f"framework-bench-{name}"
        results_path = directory / "results.json"
        if not results_path.exists():
            found[name] = None
            continue
        results = json.loads(results_path.read_text())
        meta_path = directory / "base-meta.json"
        meta = json.loads(meta_path.read_text()) if meta_path.exists() else {}
        found[name] = {"results": results, "meta": meta, "rows": analyze(results)}
    return found


MARKS = {"better": " 🟢", "worse": " 🔴", "same": ""}


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


def runtime_notes(data, args):
    results, meta = data["results"], data["meta"]
    notes = list(results.get("notes", []))
    if meta and not meta.get("exact", True):
        notes.append(f"`{args.base_ref}` at `{short(meta.get('requested_sha'))}` has no finished build yet, "
                     f"so the base is the newest finished one, `{short(meta.get('sha'))}`.")
    if meta.get("runner") == "head-same-napi":
        notes.append("The base build has no N-API runner artifact; it ran on the PR's runner, which is "
                     "built from the same pinned N-API revision.")
    elif meta.get("runner") == "head-different-napi":
        notes.append("⚠️ The base build has no N-API runner artifact and pins a different N-API revision; "
                     "it ran on the PR's runner, so N-API changes are not part of this comparison.")
    if not results.get("cpu_pinning"):
        notes.append("CPU pinning was not available; both lanes shared all CPUs.")
    completed = sum(1 for r in results["rounds"] if "results" in r)
    notes.append(f"{completed} throughput round(s), {results.get('startup_runs', 0)} first-reply probes per "
                 f"server and lane, {results['elapsed_s']:.0f} s in all.")
    return notes


def runtime_table(name, data, args):
    """The per-server table of one runtime, with its notes."""
    runtime = RUNTIMES[name]
    reference = runtime.get("reference")
    meta, rows = data["meta"], data["rows"]
    repo_url = f"https://github.com/{args.repo}"
    if reference:
        lines = ["Both lanes run the same Node.js binary, so their difference is the noise of this "
                 "run on this runner.", ""]
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
                per_round = ", ".join(f"{c:+.1%}" for c in r["round_changes"])
                throughput = (f"{r['base']:,.0f} | {r['head']:,.0f} | {r['change']:+.1%} <sub>({per_round})</sub> | "
                              + ("reference" if reference else VERDICT_TEXT[r["verdict"]])
                              + f" | {r['p99_base']:.2f} → {r['p99_head']:.2f} ms")
            else:
                throughput = "– | – | – | no result | –"
            if "first_head" in r:
                passes = ", ".join(f"{c:+.0%}" for c in r.get("first_pass_changes", []))
                startup = (f"{r['first_base']:.0f} → {r['first_head']:.0f} ms ({r['first_change']:+.0%}"
                           + (f" <sub>({passes})</sub>" if passes else "")
                           + f", spread {r['first_noise']:.0%}) | "
                           + ("reference" if reference else VERDICT_TEXT[r["first_verdict"]]))
            else:
                startup = "– | no result"
            lines.append(f"| {FIXTURE_NAMES[fixture]} | {throughput} | {startup} |")
        lines.append("")
    lines += [f"- {note}" for note in runtime_notes(data, args)] + [""]
    return lines


def findings(runtimes):
    """Significant changes across the Edge.js runtimes, as headline lines."""
    found = {"worse": [], "better": [], "slower": [], "faster": []}
    measured = []
    for name, data in runtimes.items():
        if not data or not data["rows"] or RUNTIMES[name].get("reference"):
            continue
        measured.append(name)
        for fixture, row in data["rows"].items():
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


def method_text(results):
    return ("Each runtime has its own job. It runs the PR build and the base build at the same time on one "
            "runner, each pinned to half of the CPUs (server and load generator together). After a warm-up "
            f"that compiles both builds, each server is started {results.get('startup_runs', 0)} times, in two "
            "passes that swap the CPU halves (each after one unmeasured start), to time process start to the "
            "first validated response; a change counts only when both passes move the same way by more than "
            f"{STARTUP_FLOOR:.0%} and it exceeds the probes' interquartile spread. Then each "
            "throughput round starts a fresh server per fixture, warms it up for "
            f"{results['warmup_s']} s with 32 connections and times {results['runs']} load runs of up to "
            f"{results['max_seconds']:g} s with every reply validated; rounds swap the CPU halves. Change is "
            "the geometric mean of the per-round PR/base ratios (per round in brackets); it counts only when "
            f"every round moves the same way by more than {THROUGHPUT_FLOOR:.0%} ({2 * THROUGHPUT_FLOOR:.0%} "
            "with a single round). Shared runners are noisy: confirm "
            "single-digit changes locally with `benchmarks/bench-framework-wasix-hosts.py`.")


def render(args, runtimes, history):
    repo_url = f"https://github.com/{args.repo}"
    head_link = f"[`{short(args.head_sha)}`]({repo_url}/commit/{args.head_sha})"
    run_link = f"[workflow run]({repo_url}/actions/runs/{args.run_id})" if args.run_id else "workflow run"
    base_shas = sorted({d["meta"]["sha"] for d in runtimes.values() if d and d["meta"].get("sha")})
    base_text = ", ".join(f"[`{short(s)}`]({repo_url}/commit/{s})" for s in base_shas) or "?"
    lines = [MARKER, "## Framework benchmarks", ""]
    lines.append(f"PR {head_link} (merged into `{args.base_ref}`, as CI builds it) against `{args.base_ref}` "
                 f"at {base_text}. {run_link}.")
    lines.append("")
    lines += findings(runtimes)
    missing = [RUNTIMES[n]["title"] for n, d in runtimes.items() if not d or not d["rows"]]
    if missing:
        lines.append("⚠️ **No results** for " + ", ".join(missing) + "; see the workflow run.")
    lines.append("")

    header = ["| Runtime | " + " | ".join(FIXTURE_NAMES.values()) + " |",
              "| --- | " + " | ".join("---:" for _ in FIXTURE_NAMES) + " |"]
    for title, cell in (("**Requests per second** (PR, change against the base build):", throughput_cell),
                        ("**Time to first response, ms** (PR, change against the base build):", startup_cell)):
        lines += [title, ""] + header
        for name, data in runtimes.items():
            rows = data["rows"] if data else {}
            cells = [cell(rows.get(f), RUNTIMES[name].get("reference")) for f in FIXTURE_NAMES]
            lines.append(f"| {RUNTIMES[name]['title']} | " + " | ".join(cells) + " |")
        lines.append("")
    lines += ["Each runtime ran on its own runner, so compare within a row, not across rows. Node.js rows "
              "compare a binary with itself and show the noise.", ""]

    for name, data in runtimes.items():
        if data:
            lines += [f"<details><summary><b>{RUNTIMES[name]['title']}</b></summary>", ""]
            lines += runtime_table(name, data, args) + ["</details>", ""]

    sample = next((d["results"] for d in runtimes.values() if d), None)
    if sample:
        lines += ["<details><summary>Method</summary>", "", method_text(sample), "", "</details>", ""]

    entry = {"head": args.head_sha, "run": args.run_id,
             "changes": {name: {f: round(r["change"], 4) for f, r in d["rows"].items() if "change" in r}
                         for name, d in runtimes.items() if d and not RUNTIMES[name].get("reference")},
             "first": {name: {f: round(r["first_change"], 4) for f, r in d["rows"].items() if "first_change" in r}
                       for name, d in runtimes.items() if d and not RUNTIMES[name].get("reference")}}
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
    lines.append(HISTORY_PREFIX + base64.b64encode(json.dumps(history).encode()).decode() + " -->")
    return "\n".join(lines) + "\n"


def cmd_summary(args):
    """Writes one runtime's results to the job summary, so they can be read
    before every benchmark has finished and the comment is posted."""
    runtimes = {name: None for name in RUNTIMES}
    runtimes.update({name: data for name, data in load_runtimes(args.results_dir).items()
                     if name == args.runtime and data})
    data = runtimes[args.runtime]
    title = RUNTIMES[args.runtime]["title"]
    lines = [f"## Framework benchmark: {title}", ""]
    if data:
        if not RUNTIMES[args.runtime].get("reference"):
            lines += findings(runtimes) + [""]
        lines += runtime_table(args.runtime, data, args)
        lines += ["<details><summary>Method</summary>", "", method_text(data["results"]), "", "</details>", ""]
    else:
        lines += ["⚠️ No results; see the step logs above.", ""]
    body = "\n".join(lines) + "\n"
    if os.environ.get("GITHUB_STEP_SUMMARY"):
        with open(os.environ["GITHUB_STEP_SUMMARY"], "a") as summary:
            summary.write(body)
    print(body)


def find_comment(repo, pr):
    comments = json.loads(gh("api", "--paginate", "--slurp", f"repos/{repo}/issues/{pr}/comments"))
    for comment in (c for page in comments for c in page):
        if MARKER in (comment.get("body") or "") and comment["user"]["type"] == "Bot":
            return comment
    return None


def cmd_comment(args):
    runtimes = load_runtimes(args.results_dir)
    existing = find_comment(args.repo, args.pr) if args.pr and args.post else None
    body = render(args, runtimes, read_history(existing and existing["body"]))
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
    fetch = sub.add_parser("fetch-base")
    fetch.add_argument("--runtime", required=True, choices=[n for n, r in RUNTIMES.items() if "artifact" in r])
    fetch.add_argument("--repo", required=True)
    fetch.add_argument("--base-ref", required=True)
    fetch.add_argument("--base-sha", required=True)
    fetch.add_argument("--head-sha", required=True, help="commit whose N-API pin the PR's runner was built from")
    fetch.add_argument("--head-runner", help="runner to use when a V8 WASIX base build has none")
    fetch.add_argument("--out", required=True)
    run = sub.add_parser("run")
    run.add_argument("--runtime", required=True, choices=list(RUNTIMES))
    run.add_argument("--head", required=True, help="directory with dist/ (and runner/), or the node binary")
    run.add_argument("--base", required=True, help="directory with dist/ (and runner/), or the node binary")
    run.add_argument("--out", required=True)
    run.add_argument("--rounds", type=int, default=2)
    run.add_argument("--runs", type=int, default=2)
    run.add_argument("--startup-runs", type=int, default=5, help="first-reply probes per fixture and lane")
    run.add_argument("--warmup", type=int, default=2)
    run.add_argument("--amount-scale", type=float, default=1.0)
    run.add_argument("--max-seconds", type=float, default=2.0, help="time limit of each timed load run")
    run.add_argument("--budget", type=float, default=100, help="seconds; no round starts that would end later")
    run.add_argument("--grace", type=float, default=15, help="seconds past the budget before a round is killed")
    summary = sub.add_parser("summary", help="write one runtime's results to the job summary")
    summary.add_argument("--runtime", required=True, choices=list(RUNTIMES))
    summary.add_argument("--results-dir", required=True, help="holds <runtime>/results.json")
    summary.add_argument("--repo", required=True)
    summary.add_argument("--base-ref", required=True)
    comment = sub.add_parser("comment")
    comment.add_argument("--results-dir", required=True, help="one subdirectory per runtime")
    comment.add_argument("--repo", required=True)
    comment.add_argument("--pr", type=int)
    comment.add_argument("--head-sha", required=True)
    comment.add_argument("--base-ref", required=True)
    comment.add_argument("--run-id")
    comment.add_argument("--out", required=True)
    comment.add_argument("--post", action="store_true")
    args = parser.parse_args()
    {"fetch-base": cmd_fetch_base, "run": cmd_run, "summary": cmd_summary,
     "comment": cmd_comment}[args.command](args)


if __name__ == "__main__":
    main()
