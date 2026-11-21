#!/usr/bin/env python3
"""Pull request framework benchmark: PR build against its base branch.

Used by the `framework-bench` job in .github/workflows/test-and-build.yml.

  fetch-base  Download the base branch's `edge-wasix` and N-API runner
              artifacts from its CI run, falling back to the newest base
              build that has them.
  run         Benchmark both builds at once on one runner, each pinned to its
              own half of the CPUs, swapping halves between rounds, within a
              time budget.
  comment     Compare the two, write the analysis as Markdown, and create or
              update the pull request's benchmark comment.

Running both builds at the same time on one machine keeps the comparison
fair: GitHub runners differ by more than the changes being measured, and
each round compares the two builds under the same conditions.
"""

import argparse
import base64
import json
import math
import os
from pathlib import Path
import shutil
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
WORKFLOW = "test-and-build.yml"
DIST_ARTIFACT = "edge-wasix"
RUNNER_ARTIFACT = "napi-wasmer-linux-amd64"
FIXTURE_NAMES = {"http": "node:http", "express": "Express", "hono": "Hono", "next": "Next.js"}
# A change smaller than this, or than the disagreement between rounds, is
# reported as no significant change.
THROUGHPUT_FLOOR = 0.03
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
    return run["event"] == "push" and run["head_branch"] == branch and run["path"].endswith(WORKFLOW)


def download(repo, run_id, name, dest):
    dest.mkdir(parents=True, exist_ok=True)
    gh("run", "download", str(run_id), "-R", repo, "-n", name, "-D", str(dest))


def submodule_pin(repo, ref):
    return gh_json(f"repos/{repo}/contents/napi?ref={ref}")["sha"]


def cmd_fetch_base(args):
    """Downloads the base build into args.out and records where it came from."""
    out = Path(args.out)
    candidates = [a for a in artifacts_named(args.repo, DIST_ARTIFACT)
                  if a["workflow_run"]["head_branch"] == args.base_ref]
    exact = [a for a in candidates if a["workflow_run"]["head_sha"] == args.base_sha]
    chosen = None
    for artifact in exact + candidates:  # the artifacts API lists newest first
        if run_is_base_build(args.repo, artifact["workflow_run"]["id"], args.base_ref):
            chosen = artifact
            break
    if chosen is None:
        sys.exit(f"no {DIST_ARTIFACT} artifact found for {args.base_ref}")
    run_id, sha = chosen["workflow_run"]["id"], chosen["workflow_run"]["head_sha"]
    log(f"base build: run {run_id}, commit {sha}")
    download(args.repo, run_id, DIST_ARTIFACT, out / "artifact")
    with zipfile.ZipFile(out / "artifact" / f"{DIST_ARTIFACT}.zip") as archive:
        archive.extractall(out / "dist")

    meta = {"sha": sha, "run_id": run_id, "exact": sha == args.base_sha, "requested_sha": args.base_sha}
    try:
        download(args.repo, run_id, RUNNER_ARTIFACT, out / "runner")
        meta["runner"] = "base"
    except subprocess.CalledProcessError:
        # Builds from before the runner was uploaded. The runner is built from
        # the pinned N-API revision, so the PR's runner is the same program
        # when both pin the same revision.
        base_pin, head_pin = submodule_pin(args.repo, sha), submodule_pin(args.repo, args.head_sha)
        meta["runner"] = "head-same-napi" if base_pin == head_pin else "head-different-napi"
        meta["base_napi"], meta["head_napi"] = base_pin, head_pin
        (out / "runner").mkdir(parents=True, exist_ok=True)
        shutil.copy2(Path(args.head_runner), out / "runner" / "napi_wasmer")
    (out / "runner" / "napi_wasmer").chmod(0o755)
    (out / "meta.json").write_text(json.dumps(meta, indent=2) + "\n")
    print(json.dumps(meta))


# --- run --------------------------------------------------------------------


def lane_config(build_dir, state_dir, warmup):
    dist = Path(build_dir).resolve()
    return {
        "state": str(Path(state_dir).resolve()),
        "warmup": warmup,
        "lanes": {"lane": {
            "runner": str(dist / "runner" / "napi_wasmer"),
            "guest": str(dist / "dist" / "bin" / "edge"),
            "builtins": str(dist / "dist" / "node-lib"),
            "mounts": {str(dist / "dist" / "icu"): "/usr/share/edge-icu"},
        }},
    }


def cpu_halves():
    cpus = sorted(os.sched_getaffinity(0)) if hasattr(os, "sched_getaffinity") else []
    if len(cpus) < 4 or shutil.which("taskset") is None:
        return None
    half = len(cpus) // 2
    return ",".join(map(str, cpus[:half])), ",".join(map(str, cpus[half:]))


def harness(config, output, args, startup_runs, cpus):
    command = [sys.executable, str(HARNESS), "--config", str(config), "--amount-scale", str(args.amount_scale),
               "run", "--rounds", "1", "--runs", str(args.runs), "--startup-runs", str(startup_runs),
               "--idle", "0", "--attempts", "1", "--spread", "1", "--output", str(output)]
    if cpus:
        command = ["taskset", "-c", cpus, *command]
    return command


def cmd_run(args):
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    configs = {}
    for lane, build in (("head", args.head), ("base", args.base)):
        configs[lane] = out / f"{lane}.config.json"
        configs[lane].write_text(json.dumps(lane_config(build, out / "state" / lane, args.warmup), indent=2))
    halves = cpu_halves()
    started = time.monotonic()
    rounds, notes = [], []
    last_round_s = 0.0
    for index in range(args.rounds):
        final = index == args.rounds - 1
        elapsed = time.monotonic() - started
        # The next round takes about as long as the last one (the first also
        # compiled the guest, so this errs high); the final one adds the
        # first-reply probes.
        if rounds and elapsed + last_round_s + (args.startup_allowance if final else 0) > args.budget:
            notes.append(f"stopped after {len(rounds)} of {args.rounds} rounds to stay within "
                         f"{args.budget:.0f} s")
            break
        round_started = time.monotonic()
        # Swap the CPU halves every round, so neither build keeps the better half.
        pins = {"head": halves[index % 2], "base": halves[1 - index % 2]} if halves else {}
        startup = args.startup_runs if final else 0
        processes, outputs = {}, {}
        for lane in ("head", "base"):
            outputs[lane] = out / f"{lane}-round{index + 1}.json"
            log_path = out / f"{lane}-round{index + 1}.log"
            processes[lane] = subprocess.Popen(harness(configs[lane], outputs[lane], args, startup, pins.get(lane)),
                                               stdout=open(log_path, "w"), stderr=subprocess.STDOUT)
        failed = {}
        for lane, process in processes.items():
            remaining = max(1.0, args.budget + args.grace - (time.monotonic() - started))
            try:
                process.wait(timeout=remaining)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
            if process.returncode != 0:
                failed[lane] = (out / f"{lane}-round{index + 1}.log").read_text(errors="replace")[-2000:]
        if failed:
            for lane, tail in failed.items():
                log(f"{lane} round {index + 1} failed:\n{tail}")
            notes.append("round {} failed for {}".format(index + 1, " and ".join(failed)))
            rounds.append({"index": index + 1, "failed": failed})
            break
        rounds.append({"index": index + 1, "pins": pins,
                       "results": {lane: json.loads(outputs[lane].read_text()) for lane in outputs}})
        last_round_s = time.monotonic() - round_started
        log(f"round {index + 1} took {last_round_s:.0f} s")
    summary = {"rounds": rounds, "notes": notes, "elapsed_s": time.monotonic() - started,
               "cpu_pinning": bool(halves), "amount_scale": args.amount_scale, "runs": args.runs,
               "warmup_s": args.warmup}
    (out / "results.json").write_text(json.dumps(summary, indent=2) + "\n")
    log(f"benchmark took {summary['elapsed_s']:.0f} s")


# --- comment ----------------------------------------------------------------


def geometric_mean(values):
    return math.exp(statistics.mean(math.log(value) for value in values))


def verdict(change, noise, floor, higher_is_better=True):
    threshold = max(floor, noise)
    if abs(change) < threshold:
        return "≈ no significant change"
    better = (change > 0) == higher_is_better
    return "🟢 improved" if better else "🔴 regressed"


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
        row = {"fixture": fixture, "base": statistics.mean(base_rates), "head": statistics.mean(head_rates),
               "change": change, "noise": noise, "rounds": len(ratios),
               "round_changes": [ratio - 1 for ratio in ratios],
               "verdict": verdict(change, noise, THROUGHPUT_FLOOR),
               "p99_base": statistics.median(base_p99), "p99_head": statistics.median(head_p99)}
        startup = {}
        for lane in ("head", "base"):
            for round_ in rounds:
                cell = round_["results"][lane].get("startup", {}).get(fixture, {}).get("lane")
                if cell:
                    startup[lane] = statistics.median(cell["first_response_ms"])
        if len(startup) == 2:
            startup_change = startup["head"] / startup["base"] - 1
            row.update(first_base=startup["base"], first_head=startup["head"], first_change=startup_change,
                       first_verdict=verdict(startup_change, 0.0, STARTUP_FLOOR, higher_is_better=False))
        rows[fixture] = row
    return rows


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


def render(args, results, rows, base_meta, history):
    repo_url = f"https://github.com/{args.repo}"
    head_link = f"[`{short(args.head_sha)}`]({repo_url}/commit/{args.head_sha})"
    base_link = f"[`{short(base_meta['sha'])}`]({repo_url}/commit/{base_meta['sha']})"
    run_link = f"[workflow run]({repo_url}/actions/runs/{args.run_id})" if args.run_id else "workflow run"
    completed = sum(1 for r in results["rounds"] if "results" in r)
    lines = [MARKER, "## Framework benchmark (WASIX, V8 N-API runner)", ""]
    lines.append(f"PR {head_link} (merged into `{args.base_ref}`, as CI builds it) against "
                 f"`{args.base_ref}` at {base_link}. {run_link}.")
    lines.append("")

    if rows:
        improved = [FIXTURE_NAMES[f] for f, r in rows.items() if r["verdict"].startswith("🟢")]
        regressed = [FIXTURE_NAMES[f] for f, r in rows.items() if r["verdict"].startswith("🔴")]
        slower_start = [FIXTURE_NAMES[f] for f, r in rows.items() if r.get("first_verdict", "").startswith("🔴")]
        if regressed:
            headline = "🔴 **Throughput regressed** for " + ", ".join(regressed) + "."
        elif improved:
            headline = "🟢 **Throughput improved** for " + ", ".join(improved) + "."
        else:
            headline = "**No significant throughput change.**"
        if regressed and improved:
            headline += " Improved for " + ", ".join(improved) + "."
        if slower_start:
            headline += " First reply is slower for " + ", ".join(slower_start) + "."
        lines += [headline, ""]
        lines += ["| Server | Base req/s | PR req/s | Change | Verdict | p99 base → PR | First reply base → PR |",
                  "| --- | ---: | ---: | ---: | --- | --- | --- |"]
        for fixture, r in rows.items():
            per_round = ", ".join(f"{c:+.1%}" for c in r["round_changes"])
            first = (f"{r['first_base']:.0f} → {r['first_head']:.0f} ms ({r['first_change']:+.0%})"
                     if "first_base" in r else "n/a")
            lines.append(f"| {FIXTURE_NAMES[fixture]} | {r['base']:,.0f} | {r['head']:,.0f} | "
                         f"**{r['change']:+.1%}** <sub>({per_round})</sub> | {r['verdict']} | "
                         f"{r['p99_base']:.2f} → {r['p99_head']:.2f} ms | {first} |")
        lines.append("")
    else:
        lines += ["⚠️ **The benchmark did not produce results.** See the workflow run.", ""]

    notes = list(results.get("notes", []))
    if not base_meta.get("exact"):
        notes.append(f"`{args.base_ref}` at `{short(base_meta.get('requested_sha'))}` has no finished build "
                     f"yet, so the base is the newest finished one, `{short(base_meta['sha'])}`.")
    if base_meta.get("runner") == "head-same-napi":
        notes.append("The base build has no N-API runner artifact; it ran on the PR's runner, which is built "
                     "from the same pinned N-API revision.")
    elif base_meta.get("runner") == "head-different-napi":
        notes.append("⚠️ The base build has no N-API runner artifact and pins a different N-API revision; it "
                     "ran on the PR's runner, so N-API changes are not part of this comparison.")
    if not results.get("cpu_pinning"):
        notes.append("CPU pinning was not available; both builds shared all CPUs.")
    if notes:
        lines += ["**Notes**", ""] + [f"- {note}" for note in notes] + [""]

    lines += ["<details><summary>Method</summary>", "",
              "Both builds run at the same time on one GitHub runner"
              + (", each pinned to half of its CPUs (server and load generator together), swapping halves "
                 "between rounds" if results.get("cpu_pinning") else "")
              + f"; {completed} round(s) completed in {results['elapsed_s']:.0f} s. "
              "Each round starts a fresh server per fixture, "
              f"warms it up for {results['warmup_s']} s with 32 connections, then times "
              f"{results['runs']} fixed-count load run(s) ({results['amount_scale']:.0%} of the request "
              "counts in `benchmarks/README.md`) with every reply validated. Change is the geometric mean "
              "of the per-round PR/base ratios; per-round changes are in brackets. A change is reported "
              f"only when it exceeds both {THROUGHPUT_FLOOR:.0%} and half the gap between rounds. "
              f"First reply is process start to the first validated response, median of the probes "
              f"({STARTUP_FLOOR:.0%} threshold). Shared runners are noisy: treat single-digit changes as "
              "a hint and confirm locally with `benchmarks/bench-framework-wasix-hosts.py`.", "",
              "</details>", ""]

    entry = {"head": args.head_sha, "base": base_meta["sha"], "run": args.run_id,
             "changes": {f: round(r["change"], 4) for f, r in rows.items()}}
    history = [h for h in history if h.get("head") != args.head_sha][-9:] + [entry]
    if len(history) > 1:
        lines += ["<details><summary>Earlier runs on this PR</summary>", "",
                  "| PR commit | Base | " + " | ".join(FIXTURE_NAMES.values()) + " |",
                  "| --- | --- | " + " | ".join("---:" for _ in FIXTURE_NAMES) + " |"]
        for h in reversed(history):
            cells = [f"{h['changes'][f]:+.1%}" if f in h["changes"] else "" for f in FIXTURE_NAMES]
            lines.append(f"| `{short(h['head'])}` | `{short(h['base'])}` | " + " | ".join(cells) + " |")
        lines += ["", "</details>", ""]
    lines.append(f"<sub>Updated for {head_link}.</sub>")
    lines.append(HISTORY_PREFIX + base64.b64encode(json.dumps(history).encode()).decode() + " -->")
    return "\n".join(lines) + "\n"


def find_comment(repo, pr):
    comments = json.loads(gh("api", "--paginate", "--slurp", f"repos/{repo}/issues/{pr}/comments"))
    for comment in (c for page in comments for c in page):
        if MARKER in (comment.get("body") or "") and comment["user"]["type"] == "Bot":
            return comment
    return None


def cmd_comment(args):
    results = json.loads(Path(args.results).read_text())
    base_meta = json.loads(Path(args.base_meta).read_text())
    rows = analyze(results)
    existing = find_comment(args.repo, args.pr) if args.pr else None
    body = render(args, results, rows, base_meta, read_history(existing and existing["body"]))
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
    fetch.add_argument("--repo", required=True)
    fetch.add_argument("--base-ref", required=True)
    fetch.add_argument("--base-sha", required=True)
    fetch.add_argument("--head-sha", required=True)
    fetch.add_argument("--head-runner", required=True, help="runner to use when the base build has none")
    fetch.add_argument("--out", required=True)
    run = sub.add_parser("run")
    run.add_argument("--head", required=True, help="directory with dist/ and runner/")
    run.add_argument("--base", required=True, help="directory with dist/ and runner/")
    run.add_argument("--out", required=True)
    run.add_argument("--rounds", type=int, default=2)
    run.add_argument("--runs", type=int, default=2)
    run.add_argument("--startup-runs", type=int, default=3)
    run.add_argument("--warmup", type=int, default=2)
    run.add_argument("--amount-scale", type=float, default=0.1)
    run.add_argument("--budget", type=float, default=100, help="seconds; no round starts that would end later")
    run.add_argument("--grace", type=float, default=15, help="seconds past the budget before a round is killed")
    run.add_argument("--startup-allowance", type=float, default=15, help="seconds the first-reply probes add")
    comment = sub.add_parser("comment")
    comment.add_argument("--results", required=True)
    comment.add_argument("--base-meta", required=True)
    comment.add_argument("--repo", required=True)
    comment.add_argument("--pr", type=int)
    comment.add_argument("--head-sha", required=True)
    comment.add_argument("--base-ref", required=True)
    comment.add_argument("--run-id")
    comment.add_argument("--out", required=True)
    comment.add_argument("--post", action="store_true")
    args = parser.parse_args()
    {"fetch-base": cmd_fetch_base, "run": cmd_run, "comment": cmd_comment}[args.command](args)


if __name__ == "__main__":
    main()
