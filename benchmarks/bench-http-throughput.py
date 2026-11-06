#!/usr/bin/env python3
"""Compare two WASIX Edge guests on the same N-API host and HTTP workload."""

import argparse
import hashlib
import http.client
import json
import os
from pathlib import Path
import platform
import signal
import socket
import statistics
import subprocess
import time


def sha256(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def stop(process):
    if process.poll() is None:
        os.killpg(process.pid, signal.SIGTERM)
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            os.killpg(process.pid, signal.SIGKILL)
            process.wait(timeout=5)


def wait_ready(process, port, log):
    deadline = time.monotonic() + 120
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RuntimeError(f"Server exited: {log.read_text()}")
        connection = http.client.HTTPConnection('127.0.0.1', port, timeout=1)
        try:
            connection.request('GET', '/')
            response = connection.getresponse()
            body = response.read()
            if response.status != 200 or body != b'hello world\n':
                raise RuntimeError(f"Unexpected HTTP response: {response.status} {body!r}")
            return
        except (ConnectionError, TimeoutError, OSError):
            time.sleep(0.05)
        finally:
            connection.close()
    raise RuntimeError(f"Server readiness timeout: {log.read_text()}")


def load(args, port, duration):
    command = [str(args.autocannon), '--json', '--connections', str(args.connections),
               '--pipelining', '1', '--duration', str(duration), '--timeout', '5',
               '--expectBody', 'hello world\n', f'http://127.0.0.1:{port}/']
    result = subprocess.run(command, capture_output=True, text=True,
                            timeout=duration + 30, check=True)
    data = json.loads(result.stdout)
    if data.get('errors', 0) or data.get('timeouts', 0) or data.get('non2xx', 0) or data.get('mismatches', 0):
        raise RuntimeError(f"HTTP errors: {json.dumps(data)}")
    if data['requests']['total'] == 0:
        raise RuntimeError('No requests completed')
    return data


def run(args, label, wasm, trial):
    with socket.socket() as listener:
        listener.bind(('127.0.0.1', 0))
        port = listener.getsockname()[1]
    log = args.output.parent / f'{args.output.stem}-{label}-{trial}.log'
    command = [str(args.runner), str(wasm), '--builtin-js-dir', str(args.builtin_dir),
               '--app-dir', str(args.workload.parent), '--cwd', '/app',
               '--program-name', 'edge', '--env', f'PORT={port}',
               '--env', 'EDGE_BYTECODE_CACHE=0', '--', '/app/' + args.workload.name]
    with log.open('w') as output:
        process = subprocess.Popen(command, stdout=output, stderr=subprocess.STDOUT,
                                   start_new_session=True)
        try:
            wait_ready(process, port, log)
            warmup = load(args, port, args.warmup)
            measured = load(args, port, args.duration)
            if process.poll() is not None:
                raise RuntimeError(f"Server exited during measurement: {log.read_text()}")
            return {'label': label, 'trial': trial, 'server_command': command,
                    'server_log': str(log), 'warmup': warmup, 'measured': measured}
        finally:
            stop(process)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ['runner', 'published-wasm', 'current-wasm', 'autocannon', 'builtin-dir', 'output']:
        parser.add_argument('--' + name, type=Path, required=True)
    parser.add_argument('--rounds', type=int, default=3)
    parser.add_argument('--duration', type=int, default=10)
    parser.add_argument('--warmup', type=int, default=3)
    parser.add_argument('--connections', type=int, default=32)
    args = parser.parse_args()
    if min(args.rounds, args.duration, args.warmup, args.connections) <= 0:
        parser.error('rounds, duration, warmup and connections must be positive')
    for name in ['runner', 'published_wasm', 'current_wasm', 'autocannon', 'builtin_dir', 'output']:
        setattr(args, name, getattr(args, name).resolve())
    args.workload = Path(__file__).resolve().parent / 'workloads/http-server-throughput.js'
    args.output.parent.mkdir(parents=True, exist_ok=True)
    data = {
        'date_utc': time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime()),
        'host': platform.platform(),
        'autocannon_version': subprocess.check_output([str(args.autocannon), '--version'],
                                                     stderr=subprocess.STDOUT, text=True).strip(),
        'config': {'rounds': args.rounds, 'duration_seconds': args.duration,
                   'warmup_seconds': args.warmup, 'connections': args.connections, 'pipelining': 1},
        'artifacts': {name: {'path': str(path), 'sha256': sha256(path)} for name, path in
                      [('runner', args.runner), ('published', args.published_wasm),
                       ('current', args.current_wasm), ('workload', args.workload)]},
        'runs': [],
    }
    for trial in range(1, args.rounds + 1):
        labels = ['published', 'current'] if trial % 2 else ['current', 'published']
        for label in labels:
            wasm = args.published_wasm if label == 'published' else args.current_wasm
            result = run(args, label, wasm, trial)
            data['runs'].append(result)
            args.output.write_text(json.dumps(data, indent=2) + '\n')
            print(f"{label} trial {trial}: {result['measured']['requests']['average']:,.2f} req/s", flush=True)
    data['summary'] = {}
    for label in ['published', 'current']:
        rates = [run['measured']['requests']['average'] for run in data['runs'] if run['label'] == label]
        data['summary'][label] = {'requests_per_second': rates, 'median': statistics.median(rates),
                                  'min': min(rates), 'max': max(rates)}
    data['summary']['change_percent'] = (
        data['summary']['current']['median'] / data['summary']['published']['median'] - 1) * 100
    args.output.write_text(json.dumps(data, indent=2) + '\n')
    print(json.dumps(data['summary'], indent=2), flush=True)


if __name__ == '__main__':
    main()
