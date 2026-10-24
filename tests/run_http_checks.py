#!/usr/bin/env python3
"""Linux/WSL：基础阻塞式 HTTP 阶段验收，默认包括真实 30 秒收发超时。

只启动、终止自己的进程；通过测试专用 bind 映射使用临时回环端口。
生产源码、4/64 线程池配置及 30 秒超时均不替换。仅使用 Python 标准库。
"""
import argparse
from concurrent.futures import ThreadPoolExecutor, as_completed
from datetime import datetime, timedelta, timezone
from email.utils import format_datetime, parsedate_to_datetime
import errno
import hashlib
import json
import os
from pathlib import Path
import re
import select
import socket
import struct
import subprocess
import tempfile
import time
import traceback

PROJECT = Path(__file__).resolve().parents[1]
GROUPS = ('regression', 'helpers', 'lifecycle', 'concurrency', 'faults',
          'receive-timeout', 'send-timeout')
PREFIX = b'GET /hello HTTP/1.1\r\nHost: localhost\r\nX-Hold: '
SUFFIX = b'released\r\n\r\n'


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def wait_until(predicate, description, timeout=8):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if predicate():
            return
        time.sleep(.01)
    raise AssertionError('Timed out waiting for ' + description)


def source_hashes():
    paths = [PROJECT/'CMakeLists.txt', *sorted((PROJECT/'src').glob('*.cpp')),
             *sorted((PROJECT/'include').glob('*.h')),
             PROJECT/'tests/run_http_checks.py', PROJECT/'tests/http_cases.json',
             PROJECT/'tests/http_connection_checks.cpp', PROJECT/'tests/socket_test_shim.cpp']
    return {str(p.relative_to(PROJECT)): hashlib.sha256(p.read_bytes()).hexdigest() for p in paths}


def request(path='/health'):
    return ('GET ' + path + ' HTTP/1.1\r\nHost: localhost\r\n\r\n').encode('ascii')


def read_all(client):
    data = bytearray()
    while True:
        chunk = client.recv(8192)
        if not chunk:
            return bytes(data)
        data.extend(chunk)


def parse_response(data, started):
    head, separator, body = data.partition(b'\r\n\r\n')
    require(separator, 'Incomplete HTTP response: ' + repr(data))
    lines = head.decode('ascii').split('\r\n')
    headers = {}
    for line in lines[1:]:
        key, sep, value = line.partition(':')
        require(sep, 'Malformed response header: ' + repr(line))
        headers.setdefault(key.lower(), []).append(value.strip())
    dates = headers.pop('date', [])
    require(len(dates) == 1, 'Expected exactly one Date')
    require(re.fullmatch(r'(Mon|Tue|Wed|Thu|Fri|Sat|Sun), [0-9]{2} '
                        r'(Jan|Feb|Mar|Apr|May|Jun|Jul|Aug|Sep|Oct|Nov|Dec) '
                        r'[0-9]{4} [0-9]{2}:[0-9]{2}:[0-9]{2} GMT', dates[0]), 'Date format')
    date = parsedate_to_datetime(dates[0])
    require(date.utcoffset() == timedelta(0), 'Date must be UTC')
    require(started - 2 <= date.timestamp() <= time.time() + 2, 'Date is stale')
    require(format_datetime(date, usegmt=True) == dates[0], 'Date weekday or format mismatch')
    return dict(status_line=lines[0], headers=headers, body_hex=body.hex())


def expected_response(status=200, body=b'OK\n', version='HTTP/1.1', head=False):
    reason = {200: 'OK', 400: 'Bad Request', 408: 'Request Timeout'}[status]
    headers = {'content-type': ['text/plain; charset=utf-8'], 'connection': ['close']}
    if not head:
        headers['content-length'] = [str(len(body))]
    return dict(status_line=f'{version} {status} {reason}', headers=headers,
                body_hex=(b'' if head else body).hex())


class Server:
    def __init__(self, suite, name, overrides=None):
        self.suite = suite
        self.directory = suite.root/name
        self.directory.mkdir()
        with socket.socket() as reservation:
            reservation.bind(('127.0.0.1', 0))
            self.port = reservation.getsockname()[1]
        self.env = os.environ.copy()
        for key in list(self.env):
            if key == 'LD_PRELOAD' or key.startswith(('CPP_TCP_TEST_', 'CPP_HTTP_TEST_')):
                self.env.pop(key)
        self.env.update(LD_PRELOAD=str(suite.shim), CPP_HTTP_TEST_PORT=str(self.port),
                        CPP_TCP_TEST_TRACE=str(self.directory/'shim-events.txt'))
        self.env.update(overrides or {})
        self.process = None
        self.clients = []
        suite.server_ports.append(dict(group=name, loopback_port=self.port))

    def log(self):
        path = self.directory/'server.log'
        return path.read_text(errors='replace') if path.exists() else ''

    def count(self, message):
        return self.log().count(message)

    def wait_log(self, message, count=1):
        wait_until(lambda: self.count(message) >= count, message)

    def fds(self):
        return len(os.listdir(f'/proc/{self.process.pid}/fd'))

    def connect(self, timeout=5):
        client = socket.create_connection(('127.0.0.1', self.port), timeout=timeout)
        client.settimeout(timeout)
        self.clients.append(client)
        return client

    def exchange(self, raw=None, expected=None):
        started = time.time()
        with self.connect() as client:
            client.sendall(request() if raw is None else raw)
            actual = parse_response(read_all(client), started)
        wanted = expected_response() if expected is None else expected
        require(actual == wanted, 'Response mismatch: ' + repr(actual))
        return actual

    def idle(self, baseline=None, timeout=8):
        wait_until(lambda: self.count('开始处理客户端,fd=') == self.count('结束处理客户端,fd=')
                   and (baseline is None or self.fds() == baseline), 'connection cleanup', timeout)

    def hold(self, count):
        before = self.count('开始处理客户端,fd=')
        clients = []
        for i in range(count):
            client = self.connect()
            client.sendall(PREFIX)
            clients.append(client)
            self.wait_log('开始处理客户端,fd=', before+i+1)
        return clients

    def release(self, client):
        started = time.time()
        client.sendall(SUFFIX)
        actual = parse_response(read_all(client), started)
        client.close()
        require(actual == expected_response(body=b'Hello,HTTP!\n'), repr(actual))

    def __enter__(self):
        self.output = (self.directory/'stdout.bin').open('wb')
        self.errors = (self.directory/'stderr.txt').open('wb')
        self.process = subprocess.Popen([str(self.suite.binary)], cwd=self.directory,
                                        env=self.env, stdout=self.output, stderr=self.errors)
        self.suite.processes.append(self.process)
        try:
            wait_until(lambda: '服务器开始监听' in self.log() or self.process.poll() is not None,
                       'server startup')
            require(self.process.poll() is None, 'Startup failed: ' + self.log())
            return self
        except BaseException:
            self.__exit__(None, None, None)
            raise

    def __exit__(self, exc_type, *_):
        for client in self.clients:
            client.close()
        if self.process and self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait(timeout=3)
        self.output.close()
        self.errors.close()
        stderr = (self.directory/'stderr.txt').read_text(errors='replace')
        if exc_type is None:
            require(not stderr, 'Server stderr / sanitizer diagnostic: ' + stderr)


class Suite:
    def __init__(self, root):
        self.root = root
        self.build = root/'build'
        self.binary = self.build/'main'
        self.probe = self.build/'http_connection_checks'
        self.shim = root/'socket_test_shim.so'
        self.processes = []
        self.server_ports = []

    def command(self, args, name, directory=None, timeout=120):
        result = subprocess.run(args, cwd=directory or PROJECT, capture_output=True, timeout=timeout)
        (self.root/(name+'-stdout.txt')).write_bytes(result.stdout)
        (self.root/(name+'-stderr.txt')).write_bytes(result.stderr)
        require(result.returncode == 0, f'{name} exited {result.returncode}: '
                + result.stdout.decode(errors='replace') + result.stderr.decode(errors='replace'))
        return result

    def prepare(self):
        main = (PROJECT/'src/main.cpp').read_text()
        connection = (PROJECT/'src/tcp_connection.cpp').read_text()
        require(re.search(r'ThreadPool\s+pool\s*\(\s*4\s*,\s*64\s*\)', main), 'Expected pool(4,64)')
        require(re.search(r'timeout\.tv_sec\s*=\s*30\s*;', connection), 'Expected 30-second timeout')
        self.command(['cmake', '-S', str(PROJECT), '-B', str(self.build),
                      '-DCPPSERVER_BUILD_TEST_TOOLS=ON',
                      '-DCMAKE_CXX_FLAGS=-D_GLIBCXX_ASSERTIONS -fsanitize=undefined -fno-sanitize-recover=all'], 'configure')
        built = self.command(['cmake', '--build', str(self.build), '-j', '4'], 'build')
        require(not built.stderr, 'Compiler diagnostics: ' + built.stderr.decode(errors='replace'))
        shim = self.command(['g++', '-std=c++17', '-Wall', '-Wextra', '-Wpedantic',
                             '-shared', '-fPIC', '-pthread', str(PROJECT/'tests/socket_test_shim.cpp'),
                             '-ldl', '-o', str(self.shim)], 'shim')
        require(not shim.stderr, 'Shim compiler diagnostics')

    def regression(self):
        cases = json.loads((PROJECT/'tests/http_cases.json').read_text())['cases']
        results = []
        with Server(self, 'regression', {'TZ': 'America/New_York'}) as server:
            for index, case in enumerate(cases):
                started = time.time()
                before = server.log()
                ended = server.count('结束处理客户端,fd=')
                raw = bytes.fromhex(case['request_hex'])
                result = dict(name=case['name'], passed=False)
                try:
                    with server.connect() as client:
                        if case.get('split'):
                            cut = case.get('split_at', len(raw)-1)
                            client.sendall(raw[:cut])
                            require(not select.select([client], [], [], .15)[0], 'Unexpected early response')
                            client.sendall(raw[cut:])
                        elif raw:
                            client.sendall(raw)
                        if case.get('half_close'):
                            client.shutdown(socket.SHUT_WR)
                        if case.get('delay_read_seconds'):
                            time.sleep(case['delay_read_seconds'])
                        data = read_all(client)  # A reset is a failure; retain every response byte.
                    (server.directory/f'{index:03d}.request').write_bytes(raw)
                    (server.directory/f'{index:03d}.response').write_bytes(data)
                    if case.get('expect_empty'):
                        require(not data, 'Expected a close without an HTTP response')
                    else:
                        actual = parse_response(data, started)
                        expected = dict(case['expected_response'])
                        expected['headers'] = {k:v for k,v in expected['headers'].items() if k != 'date'}
                        require(actual == expected, repr(actual))
                        result['response'] = actual
                    server.wait_log('结束处理客户端,fd=', ended+1)
                    for text in case.get('logs', []):
                        require(text in server.log()[len(before):], 'Missing log: ' + text)
                    result.update(passed=True, seconds=round(time.time()-started, 6))
                except Exception:
                    result['error'] = traceback.format_exc()
                    raise
                finally:
                    results.append(result)
                    (self.root/'http-cases.json').write_text(json.dumps(results, ensure_ascii=False, indent=2)+'\n')
            server.idle()
        return dict(passed_cases=len(results), total_cases=len(cases), cases=results,
                    date_validation='Every response: canonical English IMF-fixdate, correct UTC clock; server TZ America/New_York')

    def helpers(self):
        directory = self.root/'helpers'
        directory.mkdir()
        result = self.command([str(self.probe)], 'helpers', directory)
        require(not result.stderr, 'Helper stderr')
        rows = [line.split('\t') for line in result.stdout.decode().splitlines()]
        require(len(rows) == 25 and all(row[1] == '1' for row in rows), repr(rows))
        log = (directory/'helper.log').read_text()
        require('errno=5' in log and 'errno=92' in log, 'Missing injected cleanup failure errno logs')
        return dict(passed_cases=len(rows), cases=rows,
                    scope='16 target-prefix checks plus 9 close-helper checks using actual production functions. Specified cleanup failures/EINTR are linker-injected; time/byte bounds use real socketpair I/O.')

    def lifecycle(self):
        with Server(self, 'lifecycle') as server:
            server.exchange(); server.idle()
            baseline = server.fds()
            before = server.count('结束处理客户端,fd=')
            for i in range(40):
                server.exchange(request('/health?n='+str(i)))
            server.wait_log('结束处理客户端,fd=', before+40)
            server.idle(baseline)
            half_before = server.count('结束处理客户端,fd=')
            started = time.time()
            with server.connect() as client:
                client.sendall(request('/hello'))
                client.shutdown(socket.SHUT_WR)
                require(parse_response(read_all(client), started) == expected_response(body=b'Hello,HTTP!\n'), 'Half-close response')
            server.wait_log('结束处理客户端,fd=', half_before+1)
            # Reset a connection after the worker started, guaranteeing that it exercises recv/error cleanup.
            client = server.hold(1)[0]
            ended = server.count('结束处理客户端,fd=')
            client.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack('ii', 1, 0))
            client.close()
            server.wait_log('结束处理客户端,fd=', ended+1)
            server.wait_log('接收失败')
            require(f'errno={errno.ECONNRESET}' in server.log(), 'Missing reset errno')
            server.idle(baseline)
            other_dir = self.root/'bind-conflict'
            other_dir.mkdir()
            other = subprocess.run([str(self.binary)], cwd=other_dir, env=server.env,
                                   capture_output=True, timeout=8)
            (other_dir/'stdout.bin').write_bytes(other.stdout)
            (other_dir/'stderr.txt').write_bytes(other.stderr)
            log = (other_dir/'server.log').read_text()
            require(other.returncode == 1 and not other.stderr, 'Second server should exit 1')
            require('bind失败' in log and f'errno={errno.EADDRINUSE},' in log and '[ERROR]' in log,
                    'Bind failure not persisted: ' + log)
            require('服务器开始监听' not in log, 'Second server unexpectedly listening')
            server.exchange(); server.idle(baseline)
            return dict(repeated_connections=40, half_close=True, reset_logged=True,
                        bind_conflict_exit=other.returncode, baseline_fds=baseline, final_fds=server.fds())

    def concurrency(self):
        evidence = []
        with Server(self, 'concurrency') as server:
            server.exchange(); server.idle()
            baseline = server.fds()
            offset = len(server.log())
            held = server.hold(3)
            began = time.monotonic()
            server.exchange(request('/hello'), expected_response(body=b'Hello,HTTP!\n'))
            elapsed = time.monotonic()-began
            require(elapsed < 2, 'Fourth client blocked')
            require(not select.select(held, [], [], .15)[0], 'Held client responded')
            ids = set(re.findall(r'\[thread:([^]]+)\].*开始处理客户端,fd=', server.log()[offset:]))
            require(len(ids) == 4, 'Expected four distinct workers: ' + repr(ids))
            evidence.append(dict(name='three-held-fourth-responds', seconds=elapsed, worker_ids=sorted(ids)))
            for client in held:
                server.release(client)
            server.idle(baseline)

            held = server.hold(4)
            count = server.count('开始处理客户端,fd=')
            accepted = server.count('接收客户端连接,fd=')
            queued = server.connect(); queued.sendall(request())
            server.wait_log('接收客户端连接,fd=', accepted+1)
            require(not select.select([queued], [], [], .25)[0], 'Fifth client did not queue')
            require(server.count('开始处理客户端,fd=') == count, 'Fifth client started early')
            server.release(held.pop(0))
            require(parse_response(read_all(queued), time.time()-3) == expected_response(), 'Queued fifth response')
            queued.close()
            for client in held:
                server.release(client)
            server.idle(baseline)
            evidence.append(dict(name='fifth-queues-until-worker-released', passed=True))

            held = server.hold(4)
            count = server.count('开始处理客户端,fd=')
            accepted = server.count('接收客户端连接,fd=')
            rejected = server.count('ThreadPool task queue is full')
            queued = []
            for i in range(64):
                client = server.connect(); client.sendall(request('/health?slot='+str(i)))
                queued.append(client)
                server.wait_log('接收客户端连接,fd=', accepted+i+1)
            require(not select.select(queued, [], [], .2)[0], 'Queued request responded early')
            require(server.count('开始处理客户端,fd=') == count, 'Unexpected extra active worker')
            require(server.count('ThreadPool task queue is full') == rejected, 'Rejected within 64 slots')
            wait_until(lambda: server.fds() == baseline+68, '68 admitted connection descriptors')
            with server.connect() as extra:
                server.wait_log('ThreadPool task queue is full', rejected+1)
                require(extra.recv(1) == b'', 'Overload connection not closed')
            wait_until(lambda: server.fds() == baseline+68, 'rejected descriptor released')
            for client in held:
                server.release(client)
            for client in queued:
                require(parse_response(read_all(client), time.time()-10) == expected_response(), 'Queued response')
                client.close()
            server.idle(baseline, timeout=12)
            require(server.count('ThreadPool task queue is full') == rejected+1, 'Unexpected rejection count')
            evidence.append(dict(name='four-active-64-queued-next-rejected', admitted=68, rejected=1,
                                 all_admitted_responses_correct=True, recovered_fds=server.fds()))

            before = server.count('结束处理客户端,fd=')
            began = time.monotonic()
            def one(index):
                path, body = ('/hello', b'Hello,HTTP!\n') if index % 2 else ('/health', b'OK\n')
                server.exchange(request(path+'?n='+str(index)), expected_response(body=body))
            with ThreadPoolExecutor(max_workers=16) as executor:
                list(executor.map(one, range(200)))
            server.wait_log('结束处理客户端,fd=', before+200)
            server.idle(baseline)
            require(server.count('ThreadPool task queue is full') == rejected+1, 'Burst rejected unexpectedly')
            evidence.append(dict(name='200-requests-16-concurrent-clients', seconds=time.monotonic()-began,
                                 requests=200, concurrency=16, recovered_fds=server.fds()))
            server.exchange(); server.idle(baseline)
        return dict(scenarios=evidence, baseline_fds=baseline, final_fds=baseline)

    def faults(self):
        with Server(self, 'faults', {'CPP_TCP_TEST_EINTR': '1', 'CPP_TCP_TEST_CHUNK': '17'}) as server:
            server.exchange(request('/hello'), expected_response(body=b'Hello,HTTP!\n'))
            server.idle()
            events = (server.directory/'shim-events.txt').read_text()
            require(events.count('ACCEPT_EINTR\n') == 1, events)
            require(events.count('RECV_EINTR\n') == 1, events)
            require(events.count('SEND_EINTR\n') == 2, events)
            require(events.count('PARTIAL_SEND\n') > 0, events)
            require('[ERROR]' not in server.log(), server.log())
            server.exchange(); server.idle()
            return dict(accept_eintr=1, recv_eintr=1, send_eintr=2, max_send_bytes=17,
                        partial_send_calls=events.count('PARTIAL_SEND\n'),
                        scope='Injected syscall returns, not real signal delivery; response bytes checked without omissions/duplication')

    def receive_timeout(self):
        examples = [
            ('empty', b'', None),
            ('get', PREFIX, expected_response(408, b'Request Timeout\n')),
            ('head', b'HEAD /hello HTTP/1.1\r\nHost: localhost\r\n', expected_response(408, b'Request Timeout\n', head=True)),
            ('http10', b'GET /hello HTTP/1.0\r\n', expected_response(408, b'Request Timeout\n', version='HTTP/1.0')),
        ]
        results = []
        with Server(self, 'receive-timeout') as server:
            baseline = server.fds()
            clients = []
            for i, (name, raw, expected) in enumerate(examples):
                client = server.connect(timeout=40)
                began = time.monotonic(); started = time.time()
                if raw:
                    client.sendall(raw)
                clients.append((client, name, expected, began, started))
                server.wait_log('开始处理客户端,fd=', i+1)
            def finish(item):
                client, name, expected, began, started = item
                data = read_all(client)
                elapsed = time.monotonic()-began
                client.close()
                require(29 <= elapsed < 40, f'{name}: timeout elapsed {elapsed}')
                if expected is None:
                    require(not data, 'Empty connection should close without an HTTP response')
                else:
                    require(parse_response(data, started) == expected, f'{name}: wrong timeout response')
                return dict(name=name, seconds=elapsed, response_hex=data.hex())
            with ThreadPoolExecutor(max_workers=4) as executor:
                results = list(executor.map(finish, clients))
            server.wait_log('客户端接收超时', 4)
            server.idle(baseline)
            require(server.count('状态码=408') == 3, 'Expected three 408 response logs')
            server.exchange(); server.idle(baseline)
        return dict(socket_timeout_seconds=30, cases=results, recovery=True)

    def send_timeout(self):
        directory = self.root/'send-timeout'
        directory.mkdir()
        result = self.command([str(self.probe), '--send-timeout'], 'send-timeout', directory, timeout=45)
        require(not result.stderr, 'Send-timeout stderr')
        row = result.stdout.decode().strip().split('\t')
        require(row[0] == 'real-send-timeout' and row[1] == '1', repr(row))
        require('客户端发送超时' in (directory/'helper.log').read_text(), 'Missing real send timeout log')
        return dict(seconds=float(row[2]), prefilled_bytes=int(row[3]), socket_timeout_seconds=30,
                    scope='Calls the actual production sendAll on a real AF_UNIX SOCK_STREAM socketpair whose kernel send buffer is full. Peer never reads. Actual blocking timeout; no injected EAGAIN and no enlarged HTTP route. This directly tests sendAll, not a large-response HTTP endpoint.')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--only', choices=GROUPS, action='append', help='Run selected groups; may repeat')
    parser.add_argument('--skip-timeouts', action='store_true', help='Quick run; explicitly excludes full acceptance')
    parser.add_argument('--report', type=Path, help='Also save report here; default is the artifact directory')
    args = parser.parse_args()
    groups = list(dict.fromkeys(args.only or GROUPS))
    if args.skip_timeouts:
        groups = [g for g in groups if g not in ('receive-timeout', 'send-timeout')]
    if not groups:
        parser.error('No groups selected')
    root = Path(tempfile.mkdtemp(prefix='cppserver-http-checks-'))
    suite = Suite(root)
    report = dict(started_at=datetime.now(timezone.utc).isoformat(), artifacts=str(root),
                  source_before=source_hashes(), selected_groups=groups, results=[], fatal=None,
                  production_configuration={'worker_threads': 4, 'queue_capacity': 64,
                                            'receive_timeout_seconds': 30, 'send_timeout_seconds': 30},
                  isolation='CMake builds unmodified production sources in the artifact directory. LD_PRELOAD bind mapping redirects only 127.0.0.1:8080 in test child processes to temporary loopback ports. Normal timeout values unchanged. Only processes started by this suite are terminated.')
    def run_group(group):
        print(f'RUN {group}', flush=True)
        began = time.monotonic()
        result = dict(name=group, passed=False)
        try:
            result['details'] = getattr(suite, group.replace('-', '_'))()
            result['passed'] = True
        except Exception:
            result['error'] = traceback.format_exc()
        result['seconds'] = round(time.monotonic()-began, 6)
        report['results'].append(result)
        print(f'{"PASS" if result["passed"] else "FAIL"} {group} ({result["seconds"]:.2f}s)', flush=True)
        if not result['passed']:
            print(result['error'], flush=True)
    print('ARTIFACTS', root, flush=True)
    try:
        suite.prepare()
        report['cmake_build'] = 'fresh build passed; assertions and undefined-behavior sanitizer enabled'
        report['binary_sha256'] = hashlib.sha256(suite.binary.read_bytes()).hexdigest()
        for group in groups:
            if group not in ('receive-timeout', 'send-timeout'):
                run_group(group)
        timeouts = [g for g in groups if g in ('receive-timeout', 'send-timeout')]
        with ThreadPoolExecutor(max_workers=2) as executor:
            for completed in as_completed([executor.submit(run_group, g) for g in timeouts]):
                completed.result()
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
                    process.kill(); process.wait(timeout=3)
        report['all_own_servers_stopped'] = all(p.poll() is not None for p in suite.processes)
        report['server_ports'] = suite.server_ports
        report['source_after'] = source_hashes()
        report['source_unchanged'] = report['source_before'] == report['source_after']
        report['finished_at'] = datetime.now(timezone.utc).isoformat()
        report['passed_groups'] = sum(r['passed'] for r in report['results'])
        report['full_acceptance'] = (set(groups) == set(GROUPS) and not report['fatal']
                                     and report['passed_groups'] == len(GROUPS)
                                     and report['source_unchanged'] and report['all_own_servers_stopped'])
        text = json.dumps(report, ensure_ascii=False, indent=2)+'\n'
        (root/'report.json').write_text(text)
        if args.report:
            args.report.parent.mkdir(parents=True, exist_ok=True)
            args.report.write_text(text)
        print('REPORT', args.report or root/'report.json', flush=True)
        print(f'{report["passed_groups"]}/{len(groups)} groups passed; full_acceptance={report["full_acceptance"]}', flush=True)
    return 0 if (not report['fatal'] and report['passed_groups'] == len(groups)
                 and report['source_unchanged'] and report['all_own_servers_stopped']) else 1


if __name__ == '__main__':
    raise SystemExit(main())
