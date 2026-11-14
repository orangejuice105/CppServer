#!/usr/bin/env python3
"""Repeatable local short-connection benchmark; not a production capacity claim."""
import argparse
import asyncio
from collections import Counter
from datetime import datetime, timezone
import hashlib
import json
import math
import os
from pathlib import Path
import platform
import re
import resource
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time
import traceback
from types import SimpleNamespace

sys.dont_write_bytecode = True
from http_test_support import require, wait_until
from epoll_hardening_checks import open_held_clients
from run_epoll_checks import Server, source_hashes

PROJECT = Path(__file__).resolve().parents[1]


def command(args, root, name):
    result = subprocess.run([str(a) for a in args], cwd=PROJECT, capture_output=True, timeout=120)
    (root/(name+'-stdout.txt')).write_bytes(result.stdout)
    (root/(name+'-stderr.txt')).write_bytes(result.stderr)
    require(result.returncode == 0 and not result.stderr,
            name + ': ' + result.stdout.decode(errors='replace') + result.stderr.decode(errors='replace'))


def process_sample(pid):
    text = Path(f'/proc/{pid}/stat').read_text()
    fields = text[text.rfind(')')+2:].split()
    return dict(cpu_seconds=(int(fields[11])+int(fields[12]))/os.sysconf('SC_CLK_TCK'),
                rss_bytes=int(fields[21])*os.sysconf('SC_PAGE_SIZE'),
                fds=len(list(Path(f'/proc/{pid}/fd').iterdir())))


class Sampler:
    def __init__(self, pid):
        self.pid, self.stop_event, self.samples = pid, threading.Event(), []
        self.thread = threading.Thread(target=self.run)

    def run(self):
        while not self.stop_event.is_set():
            try:
                self.samples.append(process_sample(self.pid))
            except (FileNotFoundError, ProcessLookupError):
                break
            self.stop_event.wait(.05)

    def __enter__(self):
        self.thread.start()
        return self

    def __exit__(self, *_):
        self.stop_event.set()
        self.thread.join(timeout=2)


def percentiles(values):
    ordered = sorted(values)
    if not ordered:
        return None
    return {name: round(ordered[max(0, math.ceil(len(ordered)*p)-1)]*1000, 3)
            for name, p in [('p50', .5), ('p95', .95), ('p99', .99)]}


async def one_request(port, path):
    writer = None
    try:
        reader, writer = await asyncio.open_connection('127.0.0.1', port, family=socket.AF_INET, limit=8192)
        raw = f'GET {path} HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n'.encode()
        writer.write(raw)
        await writer.drain()
        head = await reader.readuntil(b'\r\n\r\n')
        lines = head[:-4].split(b'\r\n')
        if lines[0] != b'HTTP/1.1 200 OK':
            raise ValueError('http_status:' + repr(lines[0]))
        headers = {}
        for line in lines[1:]:
            key, sep, value = line.partition(b':')
            if not sep:
                raise ValueError('malformed_header')
            headers.setdefault(key.lower(), []).append(value.strip())
        body = b'OK\n' if path == '/health' else b'Hello,HTTP!\n'
        if (headers.get(b'content-length') != [str(len(body)).encode()] or
                headers.get(b'connection') != [b'close'] or len(headers.get(b'date', [])) != 1 or
                headers.get(b'content-type') != [b'text/plain; charset=utf-8']):
            raise ValueError('http_headers')
        if await reader.readexactly(len(body)) != body or await reader.read(1) != b'':
            raise ValueError('http_body_or_trailing_bytes')
    finally:
        if writer is not None:
            writer.close()
            try:
                await asyncio.wait_for(writer.wait_closed(), .5)
            except (OSError, asyncio.TimeoutError):
                pass


async def generate_load(port, path, concurrency, duration):
    started = time.monotonic()
    end = started + duration
    successful, failed = [], []
    errors = Counter()
    examples = []

    async def worker():
        while time.monotonic() < end:
            began = time.monotonic()
            try:
                await asyncio.wait_for(one_request(port, path), timeout=3)
                successful.append(time.monotonic()-began)
            except Exception as error:
                failed.append(time.monotonic()-began)
                category = ('timeout' if isinstance(error, asyncio.TimeoutError) else
                            'connection_reset' if isinstance(error, ConnectionResetError) else
                            'incomplete_response' if isinstance(error, asyncio.IncompleteReadError) else
                            'http_mismatch' if isinstance(error, ValueError) else type(error).__name__)
                errors[category] += 1
                if len(examples) < 5:
                    examples.append(repr(error))

    await asyncio.gather(*(worker() for _ in range(concurrency)))
    wall = time.monotonic()-started
    return dict(attempts=len(successful)+len(failed), successful_requests=len(successful),
                errors=dict(errors), error_examples=examples, measured_seconds=round(wall, 6),
                successful_requests_per_second=round(len(successful)/wall, 2),
                successful_latency_ms=percentiles(successful), failed_latency_ms=percentiles(failed))


def client_cpu():
    stats = resource.getrusage(resource.RUSAGE_SELF)
    return stats.ru_utime+stats.ru_stime


def run_case(suite, path, concurrency, duration, warmup, repeat, held_count=0):
    name = f'{path[1:]}-c{concurrency}-held{held_count}-r{repeat}'
    stats_path, count_path = suite.root/(name+'-logger.json'), suite.root/(name+'-accepted.txt')
    env = {'CPP_EPOLL_TEST_LOGGER_STATS': str(stats_path), 'CPP_EPOLL_TEST_ACCEPT_COUNT': str(count_path)}
    with Server(suite, name, env) as server:
        baseline = server.fds()
        held = open_held_clients(server, held_count, baseline)
        wait_until(lambda: server.fds() == baseline+held_count, 'benchmark held connections')
        warmup_result = asyncio.run(generate_load(server.port, path, concurrency, warmup))
        wait_until(lambda: server.fds() == baseline+held_count, 'warmup connections released')
        before = process_sample(server.process.pid)
        client_before = client_cpu()
        with Sampler(server.process.pid) as sampler:
            measured = asyncio.run(generate_load(server.port, path, concurrency, duration))
        client_after = client_cpu()
        after = process_sample(server.process.pid)
        wait_until(lambda: server.fds() == baseline+held_count, 'measured connections released')
        for client in held:
            client.close()
        server.idle(baseline)
        server.exchange()
        server.idle(baseline)
        server.process.send_signal(signal.SIGTERM)
        require(server.process.wait(timeout=8) == 0, 'Benchmark server did not exit normally')
        stats = json.loads(stats_path.read_text())
        accepted = int(count_path.read_text())
        logs = ''.join(p.read_text(errors='replace') for p in server.directory.glob('server.log*'))
        managed = logs.count('接收客户端连接,fd=')
        warning_counts = [int(n) for n in re.findall(r'rejected\s*=\s*(\d+)', logs)]
        # Exact only when all accepted-client logs were retained and none were dropped.
        log_bytes = sum(p.stat().st_size for p in server.directory.glob('server.log*'))
        exact_rejections = accepted-managed if stats['dropped_count'] == 0 and log_bytes < 30*1024*1024 else None
        require('[ERROR]' not in logs and '优雅退出等待超时' not in logs, 'Unexpected server failure during benchmark')
        overload = held_count == 1024
        if overload:
            require(measured['successful_requests'] == 0 and warning_counts,
                    'Full capacity did not reject requests')
            require(not measured['errors'].get('http_mismatch'), 'Overload produced malformed HTTP')
        else:
            require(measured['successful_requests'] > 0 and not measured['errors'],
                    'Normal benchmark had request failures: ' + repr(measured))
            require(not warning_counts, 'Normal load reached capacity unexpectedly')
        wall = measured['measured_seconds']
        samples = sampler.samples + [before, after]
        return dict(name=name, path=path, concurrency=concurrency, held_connections=held_count,
                    repeat=repeat, expected_overload=overload, warmup=warmup_result, measurement=measured,
                    server_cpu_percent_one_core=round((after['cpu_seconds']-before['cpu_seconds'])/wall*100, 2),
                    client_cpu_percent_one_core=round((client_after-client_before)/wall*100, 2),
                    peak_rss_bytes=max(s['rss_bytes'] for s in samples), peak_fds=max(s['fds'] for s in samples),
                    baseline_fds=baseline, recovered_fds=baseline, logger_stats=stats,
                    accepted_connections_total_including_warmup_and_probe=accepted,
                    capacity_rejections_total_including_warmup=exact_rejections,
                    capacity_rejections_log_lower_bound=max(warning_counts, default=0),
                    log_bytes_retained=log_bytes, log_rotation_retention_limit_bytes=40*1024*1024,
                    passed=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--duration', type=float, default=3)
    parser.add_argument('--warmup', type=float, default=.5)
    parser.add_argument('--repeats', type=int, default=2)
    parser.add_argument('--concurrency', type=int, nargs='+', default=[1, 16, 64])
    parser.add_argument('--report', type=Path)
    args = parser.parse_args()
    if args.duration <= 0 or args.warmup <= 0 or args.repeats < 1 or any(c < 1 or c > 512 for c in args.concurrency):
        parser.error('positive duration/warmup/repeats and concurrency 1..512 required')
    root = Path(tempfile.mkdtemp(prefix='cppserver-epoll-benchmark-'))
    build = root/'build'
    suite = SimpleNamespace(root=root, build=build, binary=build/'epoll_observed_server',
                            shim=root/'socket_test_shim.so', processes=[], server_ports=[])
    report = dict(started_at=datetime.now(timezone.utc).isoformat(), artifacts=str(root), results=[], fatal=None,
                  source_before=source_hashes(), configuration=vars(args).copy(),
                  environment=dict(platform=platform.platform(), cpu_count=os.cpu_count(),
                                   cpu_affinity=sorted(os.sched_getaffinity(0)),
                                   nofile_soft_hard=list(resource.getrlimit(resource.RLIMIT_NOFILE)),
                                   clock_ticks=os.sysconf('SC_CLK_TCK'),
                                   proc_cpuinfo=Path('/proc/cpuinfo').read_text(),
                                   proc_meminfo=Path('/proc/meminfo').read_text()),
                  git_head=subprocess.check_output(['git','rev-parse','HEAD'], cwd=PROJECT, text=True).strip(),
                  git_status=subprocess.check_output(['git','status','--short'], cwd=PROJECT, text=True),
                  build_configuration='Release, production main and event loop; logger-counter snapshot after return',
                  logging=dict(min_level='INFO', queue_capacity=10000, batch_size=100,
                               max_file_bytes=10*1024*1024, max_backup_files=3, output='asynchronous file'),
                  latency_scope='Client: before connect through full response, server FIN and local connection cleanup. '
                                'New TCP connection for every request; completed attempts divided by actual elapsed time including final in-flight requests.',
                  limitations=['Client and server share this machine and loopback; Python asyncio may limit offered load.',
                               'The server keeps its production backlog of 5 and one-request-per-connection policy.',
                               'Test-only port redirection, an enabled atomic accept counter, and an after-loop logger snapshot; no request, clock, or response substitution.',
                               'Short repeated measurements establish a local baseline, not a maximum capacity or long-running production result.',
                               'Capacity rejection counts include warmup and the post-run health probe; successful QPS counts measured 200 responses only.',
                               'Resource peaks sampled every 50 ms may miss shorter peaks.'])
    report['configuration']['report'] = str(args.report) if args.report else None
    print('ARTIFACTS', root, flush=True)
    try:
        command(['cmake','-S',PROJECT,'-B',build,'-DCMAKE_BUILD_TYPE=Release',
                 '-DCPPSERVER_BUILD_EPOLL_TEST_TOOLS=ON'], root, 'configure')
        command(['cmake','--build',build,'--target','main','epoll_observed_server','-j','4'], root, 'build')
        command(['g++','-std=c++17','-Wall','-Wextra','-Wpedantic','-shared','-fPIC','-pthread',
                 PROJECT/'tests/socket_test_shim.cpp','-ldl','-o',suite.shim], root, 'shim')
        binaries = [build/'main', suite.binary, suite.shim]
        report['binary_sha256'] = {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in binaries}
        scenarios = [(path,c,0) for path in ('/health','/hello') for c in args.concurrency]
        scenarios += [('/health',64,256), ('/health',16,1024)]
        for path, concurrency, held in scenarios:
            for repeat in range(1, args.repeats+1):
                print(f'RUN {path} concurrency={concurrency} held={held} repeat={repeat}', flush=True)
                item = run_case(suite, path, concurrency, args.duration, args.warmup, repeat, held)
                report['results'].append(item)
                print(f'PASS {item["name"]}: {item["measurement"]["successful_requests_per_second"]} successful requests/s', flush=True)
        report['binaries_unchanged'] = report['binary_sha256'] == {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in binaries}
    except Exception:
        report['fatal'] = traceback.format_exc()
        print(report['fatal'], flush=True)
    finally:
        for process in suite.processes:
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=3)
        report['source_after'] = source_hashes()
        report['source_unchanged'] = report['source_before'] == report['source_after']
        report['all_owned_servers_stopped'] = all(p.poll() is not None for p in suite.processes)
        report['server_ports'] = suite.server_ports
        report['finished_at'] = datetime.now(timezone.utc).isoformat()
        report['passed'] = (not report['fatal'] and report['source_unchanged'] and
                            report.get('binaries_unchanged', False) and report['all_owned_servers_stopped'])
        output = json.dumps(report, ensure_ascii=False, indent=2)+'\n'
        (root/'report.json').write_text(output)
        if args.report:
            args.report.parent.mkdir(parents=True, exist_ok=True)
            args.report.write_text(output)
        print('REPORT', args.report or root/'report.json', flush=True)
    return 0 if report['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
