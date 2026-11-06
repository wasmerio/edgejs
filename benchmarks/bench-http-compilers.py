#!/usr/bin/env python3
"""Compare LLVM and Cranelift HTTP throughput using one runner and WASIX guest."""

import argparse
import importlib.util
import json
import math
import os
from pathlib import Path
import platform
import random
import statistics
import subprocess
import time


BACKENDS = ('llvm', 'cranelift')
BOOTSTRAP_SAMPLES = 10_000
BOOTSTRAP_SEED = 20261106
HERE = Path(__file__).resolve().parent


def positive_integer(value):
    number = int(value)
    if number <= 0:
        raise argparse.ArgumentTypeError('must be positive')
    return number


def distribution(values):
    return {'values': values, 'median': statistics.median(values),
            'min': min(values), 'max': max(values)}


def summarize(runs):
    backends = {}
    for backend in BACKENDS:
        selected = [run for run in runs if run['compiler'] == backend]
        if selected:
            backends[backend] = {
                'requests_per_second': distribution(
                    [run['measured']['requests']['average'] for run in selected]),
                'server_cpu_us_per_request': distribution(
                    [run['server_cpu_us_per_request'] for run in selected]),
            }
    pairs = []
    for trial in sorted({run['trial'] for run in runs}):
        paired = {run['compiler']: run for run in runs if run['trial'] == trial}
        if all(backend in paired for backend in BACKENDS):
            rates = {backend: paired[backend]['measured']['requests']['average']
                     for backend in BACKENDS}
            pairs.append({'trial': trial, 'requests_per_second': rates,
                          'llvm_over_cranelift_ratio': rates['llvm'] / rates['cranelift']})
    inference = None
    if pairs:
        log_ratios = [math.log(pair['llvm_over_cranelift_ratio']) for pair in pairs]
        rng = random.Random(BOOTSTRAP_SEED)
        samples = [statistics.median(rng.choices(log_ratios, k=len(log_ratios)))
                   for _ in range(BOOTSTRAP_SAMPLES)]
        quantiles = statistics.quantiles(samples, n=40, method='inclusive')
        estimate = math.exp(statistics.median(log_ratios))
        interval = [math.exp(quantiles[0]), math.exp(quantiles[-1])]
        inference = {
            'method': 'paired percentile bootstrap of median log(LLVM/Cranelift throughput)',
            'pair_count': len(pairs), 'bootstrap_samples': BOOTSTRAP_SAMPLES,
            'random_seed': BOOTSTRAP_SEED, 'llvm_over_cranelift_ratio': estimate,
            'ratio_95_percent_interval': interval, 'change_percent': (estimate - 1) * 100,
            'change_percent_95_percent_interval': [(value - 1) * 100 for value in interval],
            'caveat': 'Single-host short runs; few pairs give uncertain bootstrap coverage. '
                      'Pair resampling assumes independent pairs and does not remove '
                      'thermal drift, shared client/server contention or other systematic bias.',
        }
    return {'backends': backends, 'pairs': pairs, 'paired_inference': inference}


def version(command):
    result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True, timeout=10, check=False)
    return {'command': command, 'exit_code': result.returncode, 'output': result.stdout.strip()}


def save(path, data):
    temporary = path.with_suffix(path.suffix + '.tmp')
    temporary.write_text(json.dumps(data, indent=2, allow_nan=False) + '\n')
    temporary.replace(path)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('runner', 'wasm', 'builtin-dir', 'autocannon', 'output'):
        parser.add_argument('--' + name, type=Path, required=True)
    for name, default in (('rounds', 6), ('warmup', 10), ('duration', 15), ('connections', 32)):
        parser.add_argument('--' + name, type=positive_integer, default=default)
    args = parser.parse_args()
    for name in ('runner', 'wasm', 'builtin_dir', 'autocannon', 'output'):
        setattr(args, name, getattr(args, name).resolve())
    for name in ('runner', 'wasm', 'autocannon'):
        if not getattr(args, name).is_file():
            parser.error(f'--{name} must name a file')
    if not args.builtin_dir.is_dir():
        parser.error('--builtin-dir must name a directory')
    args.workload = HERE / 'workloads/http-server-throughput.js'
    args.output.parent.mkdir(parents=True, exist_ok=True)
    harness_path = HERE / 'bench-http-throughput.py'
    spec = importlib.util.spec_from_file_location('http_throughput', harness_path)
    harness = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(harness)
    data = {
        'date_utc': time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime()),
        'host': platform.platform(), 'status': 'running',
        'method': 'Same runner and guest, fresh server per run, alternating paired compiler order',
        'tool_versions': {'python': platform.python_version(),
                          'node': version(['node', '--version']),
                          'autocannon': version([str(args.autocannon), '--version']),
                          'runner_version_probe': version([str(args.runner), '--version'])},
        'config': {'rounds': args.rounds, 'warmup_seconds': args.warmup,
                   'duration_seconds': args.duration, 'connections': args.connections,
                   'pipelining': 1, 'builtin_dir': str(args.builtin_dir),
                   'compiler_environment_variable': 'WASMER_NAPI_COMPILER',
                   'pair_orders': [list(BACKENDS if trial % 2 else reversed(BACKENDS))
                                   for trial in range(1, args.rounds + 1)],
                   'environment': {key: os.environ[key] for key in
                                   ('WASMER_NAPI_CACHE_DIR', 'LLVM_SYS_231_PREFIX',
                                    'LLVM_SYS_231_STRICT_VERSIONING') if key in os.environ}},
        'artifacts': {name: {'path': str(path), 'sha256': harness.sha256(path)} for name, path in
                      [('runner', args.runner), ('guest', args.wasm), ('workload', args.workload),
                       ('harness', harness_path), ('comparison', Path(__file__).resolve())]},
        'runs': [],
    }
    save(args.output, data)
    try:
        for trial in range(1, args.rounds + 1):
            for compiler in (BACKENDS if trial % 2 else reversed(BACKENDS)):
                result = harness.run(args, compiler, args.wasm, trial, compiler=compiler)
                rate = result['measured']['requests']['average']
                cpu = result['server_cpu_us_per_request']
                if not math.isfinite(rate) or rate <= 0 or not math.isfinite(cpu) or cpu < 0:
                    raise RuntimeError(f'Invalid throughput or CPU measurement: {result}')
                data['runs'].append(result)
                save(args.output, data)
                print(f'{compiler} pair {trial}: {rate:,.2f} req/s; {cpu:.2f} server CPU us/req',
                      flush=True)
        data['status'] = 'complete'
    except BaseException as error:
        data['status'] = 'failed'
        data['failure'] = {'type': type(error).__name__, 'message': str(error)}
        raise
    finally:
        data['summary'] = summarize(data['runs'])
        save(args.output, data)
    print(json.dumps(data['summary'], indent=2), flush=True)


if __name__ == '__main__':
    main()
