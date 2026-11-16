#!/usr/bin/env python3
"""Linux/WSL: current LT epoll HTTP acceptance; default includes real 30/60-second deadlines."""
import argparse
import copy
from concurrent.futures import ThreadPoolExecutor, as_completed
from datetime import datetime, timezone
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
import threading
import time
import traceback
import sys
sys.dont_write_bytecode = True
# Shared utilities contain transport and response assertions only.
from http_test_support import (Server as TestServer, read_all, parse_response,
                             expected_response, request, require, wait_until, PREFIX, SUFFIX)
from epoll_hardening_checks import check_capacity, check_request_logging, check_graceful_shutdown
from accept_recovery_checks import check_accept_recovery

PROJECT = Path(__file__).resolve().parents[1]
GROUPS = ('regression', 'helpers', 'lifecycle', 'concurrency', 'faults', 'draining',
          'oversize', 'fatal-cleanup', 'capacity', 'request-logging', 'graceful-shutdown',
          'accept-recovery', 'logger-time',
          'receive-timeout', 'request-deadline', 'send-timeout')
TIMEOUTS = ('receive-timeout', 'request-deadline', 'send-timeout')


def source_hashes():
    paths = [PROJECT/'CMakeLists.txt', *sorted((PROJECT/'src').glob('*.cpp')),
             *sorted((PROJECT/'include').glob('*.h')),
             *sorted((PROJECT/'tests').glob('*.cpp')), *sorted((PROJECT/'tests').glob('*.py')),
             PROJECT/'tests/http_cases.json']
    return {str(p.relative_to(PROJECT)): hashlib.sha256(p.read_bytes()).hexdigest() for p in paths}


class Server(TestServer):
    def __init__(self, suite, name, overrides=None):
        super().__init__(suite, name)
        for key in list(self.env):
            if key.startswith(('CPP_EPOLL_TEST_', 'CPP_SEND_FIXTURE_')):
                self.env.pop(key)
        self.env.update(overrides or {})

    def idle(self, baseline, timeout=5):
        wait_until(lambda: self.fds() == baseline, 'epoll client fd release', timeout)
        require(self.process.poll() is None, 'server unexpectedly exited')

    def trace(self):
        p = self.directory/'shim-events.txt'
        return p.read_text() if p.exists() else ''


class Suite:
    def __init__(self, root):
        self.root = root
        self.build = root/'build'
        self.binary = self.build/'main'
        self.fd_build = root/'fd-limit-release-build'
        self.fd_binary = self.fd_build/'main'
        self.shim = root/'socket_test_shim.so'
        self.processes = []
        self.server_ports = []

    def command(self, args, name, directory=None, timeout=120):
        result = subprocess.run([str(a) for a in args], cwd=directory or PROJECT,
                                capture_output=True, timeout=timeout)
        (self.root/(name+'-stdout.txt')).write_bytes(result.stdout)
        (self.root/(name+'-stderr.txt')).write_bytes(result.stderr)
        require(result.returncode == 0, f'{name}: exit {result.returncode}\n'
                + result.stdout.decode(errors='replace') + result.stderr.decode(errors='replace'))
        require(not result.stderr, f'{name}: compiler/sanitizer stderr: '+result.stderr.decode(errors='replace'))
        return result.stdout.decode()

    def prepare(self):
        self.command(['cmake', '-S', PROJECT, '-B', self.build,
                      '-DCPPSERVER_BUILD_EPOLL_TEST_TOOLS=ON',
                      '-DCMAKE_CXX_FLAGS=-D_GLIBCXX_ASSERTIONS -fsanitize=undefined -fno-sanitize-recover=all'], 'configure')
        self.command(['cmake', '--build', self.build, '-j', '4'], 'build')
        self.command(['g++', '-std=c++17', '-Wall', '-Wextra', '-Wpedantic', '-shared',
                      '-fPIC', '-pthread', PROJECT/'tests/socket_test_shim.cpp', '-ldl', '-o', self.shim], 'shim')
        # GCC 11's UBSan vptr checker allocates a pipe. True fd exhaustion can
        # therefore make it report a valid standard-library object as invalid.
        # Use a cold-start production Release binary for genuine exhaustion;
        # injected resource faults and the other checks retain UBSan.
        self.command(['cmake', '-S', PROJECT, '-B', self.fd_build,
                      '-DCMAKE_BUILD_TYPE=Release', '-DCMAKE_CXX_FLAGS=-D_GLIBCXX_ASSERTIONS'],
                     'fd-limit-configure')
        self.command(['cmake', '--build', self.fd_build, '--target', 'main', '-j', '4'],
                     'fd-limit-build')

    def capacity(self):
        return check_capacity(self, Server)

    def request_logging(self):
        return check_request_logging(self, Server)

    def graceful_shutdown(self):
        return check_graceful_shutdown(self, Server)

    def accept_recovery(self):
        return check_accept_recovery(self, Server)

    def logger_time(self):
        results = []
        normal = r'\[[0-9]{4}-[0-9]{2}-[0-9]{2} [0-9]{2}:[0-9]{2}:[0-9]{2}\]'
        for mode in ('normal', 'failure', 'recovery'):
            path = self.root/('logger-time-'+mode+'.log')
            output = self.command([self.build/'logger_time_checks', mode, path],
                                  'logger-time-'+mode, timeout=5)
            lines = path.read_text().splitlines()
            require(len(lines) == (2 if mode == 'recovery' else 1), 'Missing/duplicate logger lines')
            if mode == 'normal':
                require(re.match(normal+r' \[INFO\]', lines[0]) and
                        'normal-time-marker' in lines[0], 'Normal log timestamp')
            elif mode == 'failure':
                require(lines[0].startswith('[time-unavailable] [ERROR] ') and
                        'failed-time-body-preserved' in lines[0], 'Fallback log/body was lost')
            else:
                require(lines[0].startswith('[time-unavailable] [WARN] ') and
                        'before-time-recovery' in lines[0], 'Initial fallback log was lost')
                require(re.match(normal+r' \[INFO\]', lines[1]) and
                        'after-time-recovery' in lines[1], 'Logger did not recover after conversion failure')
            results.append(dict(name=mode, output=output.strip(), log_lines=len(lines)))
        return dict(passed_cases=len(results), cases=results,
                    scope='Real logger and queue/destructor flush; only localtime_r failure is injected. Check direct helper results, call counts, persisted bodies and recovery; no server-clock changes.')

    def regression(self):
        cases = json.loads((PROJECT/'tests/http_cases.json').read_text())['cases']
        results = []
        obsolete_logs = ('状态码=', '客户端结束发送时请求头仍不完整')
        with Server(self, 'regression', {'TZ': 'America/New_York'}) as server:
            baseline = server.fds()
            for index, case in enumerate(cases):
                before = len(server.log()); started = time.time()
                raw = bytes.fromhex(case['request_hex'])
                item = dict(name=case['name'], passed=False)
                try:
                    with server.connect() as client:
                        if case.get('split'):
                            cut = case.get('split_at', len(raw)-1)
                            client.sendall(raw[:cut])
                            require(not select.select([client], [], [], .15)[0], 'premature response before final fragment')
                            client.sendall(raw[cut:])
                        elif raw:
                            client.sendall(raw)
                        if case.get('half_close'): client.shutdown(socket.SHUT_WR)
                        if case.get('delay_read_seconds'): time.sleep(case['delay_read_seconds'])
                        data = read_all(client)
                    (server.directory/f'{index:03d}.request').write_bytes(raw)
                    (server.directory/f'{index:03d}.response').write_bytes(data)
                    if case.get('expect_empty'):
                        require(not data, 'expected close without HTTP response')
                    else:
                        actual = parse_response(data, started)
                        wanted = copy.deepcopy(case['expected_response']); wanted['headers'].pop('date', None)
                        require(actual == wanted, 'HTTP mismatch: '+repr(actual))
                    server.idle(baseline)
                    for text in case.get('logs', []):
                        if text.startswith(obsolete_logs): continue
                        current = server.log()[before:]
                        if text == '请求目标长度超出上限':
                            require('请求目标超限' in current or '请求目标已经超限' in current, 'missing target rejection log')
                        else:
                            require(text in current, 'missing current parser log: '+text)
                    item.update(passed=True, seconds=round(time.time()-started, 5))
                except Exception:
                    item['error'] = traceback.format_exc()
                    raise
                finally:
                    results.append(item)
                    (self.root/'http-cases.json').write_text(json.dumps(results, ensure_ascii=False, indent=2))
        return dict(passed_cases=len(results), total_cases=len(cases), cases=results,
                    comparison='All original status/header/body bytes; Date independently validated in UTC with server TZ America/New_York. Obsolete blocking-only EOF/status logs replaced by wire assertions; current parser and target rejection logs retained.')

    def helpers(self):
        output = self.command([self.build/'epoll_connection_checks', self.root/'helpers.log'], 'helpers')
        passed = [line for line in output.splitlines() if line.startswith('PASS ')]
        require(len(passed) == 14 and 'RESULT 14/14' in output, output)
        return dict(passed_cases=len(passed), cases=passed,
                    scope='Production receive/send/drain/event functions, real nonblocking socketpair and epoll; prepared expired timestamps for boundary checks, injected recv/send EINTR. Real long deadlines tested separately.')

    def lifecycle(self):
        with Server(self, 'lifecycle') as server:
            baseline = server.fds()
            for i in range(40): server.exchange(request('/health?n='+str(i)))
            server.idle(baseline)
            with server.connect() as client:
                started = time.time(); client.sendall(request('/hello')); client.shutdown(socket.SHUT_WR)
                require(parse_response(read_all(client), started) == expected_response(body=b'Hello,HTTP!\n'), 'half-close response')
            server.idle(baseline)
            with server.connect() as client:
                client.shutdown(socket.SHUT_WR); require(client.recv(1) == b'', 'empty EOF response')
            server.idle(baseline)
            before = len(server.log()); client = server.connect(); client.sendall(PREFIX)
            wait_until(lambda: server.fds() > baseline, 'reset client accepted')
            client.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack('ii', 1, 0)); client.close()
            server.idle(baseline)
            wait_until(lambda: '客户端出现epoll错误事件' in server.log()[before:] or 'errno=104' in server.log()[before:], 'reset handling evidence')
            directory = self.root/'bind-conflict'; directory.mkdir()
            completed = subprocess.run([str(self.binary)], cwd=directory, env=server.env, capture_output=True, timeout=8)
            log = (directory/'server.log').read_text()
            require(completed.returncode == 1 and not completed.stderr, 'bind conflict exit status')
            require('bind失败' in log and f'errno={errno.EADDRINUSE},' in log and '[ERROR]' in log, 'bind error not persisted')
            server.exchange(); server.idle(baseline)
            return dict(repeated_connections=40, empty_eof=True, half_close=True, real_tcp_reset=True,
                        bind_conflict_logged=True, baseline_fds=baseline, final_fds=server.fds())

    def concurrency(self):
        with Server(self, 'concurrency') as server:
            baseline = server.fds(); held = []
            for _ in range(80):
                client = server.connect(); client.sendall(PREFIX); held.append(client)
            wait_until(lambda: server.fds() == baseline+80, '80 accepted partial requests')
            begin = time.monotonic(); server.exchange(); latency = time.monotonic()-begin
            require(latency < 2 and not select.select(held, [], [], .05)[0], 'idle clients blocked independent request')
            for client in held: client.sendall(SUFFIX)
            for client in held:
                require(parse_response(read_all(client), time.time()-10) == expected_response(body=b'Hello,HTTP!\n'), 'held response mismatch')
                client.close()
            server.idle(baseline)
            def one(index):
                path, body = ('/hello', b'Hello,HTTP!\n') if index%2 else ('/health', b'OK\n')
                server.exchange(request(path+'?n='+str(index)), expected_response(body=body))
            with ThreadPoolExecutor(max_workers=16) as pool: list(pool.map(one, range(200)))
            server.idle(baseline)
            ids = set(re.findall(r'\[thread:([^]]+)\].*(?:接收客户端连接|方法=)', server.log()))
            require(len(ids) == 1, 'expected one network event-loop thread: '+repr(ids))
            return dict(held_connections=80, independent_request_seconds=latency,
                        burst_requests=200, burst_concurrency=16, event_loop_thread_ids=sorted(ids),
                        baseline_fds=baseline, final_fds=server.fds())

    def faults(self):
        scenarios = [
            ('interrupts', {'CPP_TCP_TEST_EINTR':'1', 'CPP_TCP_TEST_CHUNK':'17', 'CPP_EPOLL_TEST_WAIT_EINTR':'1'},
             ['ACCEPT_EINTR','RECV_EINTR','SEND_EINTR','PARTIAL_SEND','EPOLL_WAIT_EINTR'], None),
            ('recv-again', {'CPP_EPOLL_TEST_RECV_EAGAIN':'1'}, ['RECV_EAGAIN'], None),
            ('send-again', {'CPP_EPOLL_TEST_SEND_EAGAIN':'1'}, ['SEND_EAGAIN'], None),
            ('add-fail', {'CPP_EPOLL_TEST_ADD_FAIL':'1'}, ['CLIENT_ADD_FAIL'], '登记客户端到epoll失败'),
            ('mod-fail', {'CPP_EPOLL_TEST_MOD_FAIL':'1'}, ['CLIENT_MOD_FAIL'], '修改客户端epoll关注事件失败'),
            ('del-fail', {'CPP_EPOLL_TEST_DEL_FAIL':'1'}, ['CLIENT_DEL_FAIL'], '取消epoll登记失败'),
            ('shutdown-fail', {'CPP_EPOLL_TEST_SHUTDOWN_FAIL':'1'}, ['SHUTDOWN_FAIL'], '结束发送方向失败'),
        ]
        results = []
        for name, env, events, log in scenarios:
            with Server(self, 'fault-'+name, env) as server:
                baseline = server.fds()
                if name in ('add-fail','mod-fail'):
                    with server.connect() as client:
                        if name == 'mod-fail': client.sendall(request())
                        require(read_all(client) == b'', 'failed initialization should close without response')
                else:
                    server.exchange(request('/hello'), expected_response(body=b'Hello,HTTP!\n'))
                server.idle(baseline)
                for event in events: require(event+'\n' in server.trace(), 'fault was not exercised: '+event)
                if name == 'interrupts':
                    require(server.trace().count('SEND_EINTR\n') == 2, 'send EINTR count')
                    require('[ERROR]' not in server.log(), 'recoverable interruption became fatal')
                if log: require(log in server.log(), 'missing failure log: '+log)
                server.exchange(); server.idle(baseline)
                results.append(dict(name=name, injected=True, events=events, recovered_fds=server.fds()))
        return dict(cases=results, scope='Explicit syscall-return injection; no real signal delivery or resource exhaustion claimed.')

    def draining(self):
        with Server(self, 'draining') as server:
            baseline = server.fds()
            def responded():
                client = server.connect(); started = time.time(); client.sendall(request())
                require(parse_response(read_all(client), started) == expected_response(), 'drain response')
                return client
            client = responded(); begin = time.monotonic()
            # Keep the peer write direction open: no EOF event is generated.
            server.idle(baseline); idle = time.monotonic()-begin
            require(idle < 1, 'quiet drain was not periodically released'); client.close()
            client = responded(); begin = time.monotonic(); sent = 0
            while server.fds() > baseline and time.monotonic()-begin < 2:
                try: client.sendall(b'x'); sent += 1
                except OSError: break
                time.sleep(.025)
            server.idle(baseline); elapsed = time.monotonic()-begin; client.close()
            require(.8 <= elapsed < 1.6 and sent >= 10, 'drain total deadline was extended by trickle')
            before = len(server.log()); client = responded()
            try: client.sendall(b'x'*(200*1024))
            except (BrokenPipeError, ConnectionResetError): pass
            server.idle(baseline); client.close()
            logs = server.log()[before:]
            require('达到64KiB上限' in logs or '达到字节上限' in logs, 'byte budget was not exercised')
            server.exchange(); server.idle(baseline)
            return dict(quiet_drain_release_seconds=idle, total_deadline_seconds=elapsed,
                        trickle_writes=sent, byte_limit_exercised=True, final_fds=server.fds())

    def oversize(self):
        results = []
        with Server(self, 'oversize') as server:
            baseline = server.fds()
            for method in ('GET','HEAD'):
                for length in (4096,4097,8193,16384,1024*1024):
                    observations = []
                    for split in (False,True):
                        raw = method.encode()+b' /'+b'a'*(length-1)+b' HTTP/1.1\r\nHost: localhost\r\n\r\n'
                        wanted = expected_response(414,b'URI Too Long\n',head=method=='HEAD') if length>4096 else (
                            expected_response(405,b'Method Not Allowed\n',head=True) if method=='HEAD' else expected_response(404,b'Not Found\n'))
                        started = time.time(); began = time.monotonic(); client = server.connect(timeout=5)
                        errors = []; data = bytearray(); ending = 'eof'
                        def write():
                            try:
                                if split:
                                    cut = len(method)+1+4096; client.sendall(raw[:cut]); time.sleep(.02); client.sendall(raw[cut:])
                                else: client.sendall(raw)
                            except OSError as error: errors.append(error.errno)
                        writer = threading.Thread(target=write); writer.start()
                        try:
                            while True:
                                try: block = client.recv(8192)
                                except ConnectionResetError: ending = 'reset'; break
                                if not block: break
                                data.extend(block)
                            writer.join(timeout=6); require(not writer.is_alive(), 'oversize sender did not finish')
                            actual = parse_response(bytes(data), started); require(actual == wanted, 'oversize response mismatch')
                            client.close(); server.idle(baseline)
                            require(time.monotonic()-began < 6, 'oversize connection exceeded cleanup budget')
                            observations.append(actual)
                            results.append(dict(method=method,target_bytes=length,split=split,http_complete=True,
                                                termination=ending,send_errors=errors,seconds=time.monotonic()-began))
                        finally:
                            client.close(); writer.join(timeout=6)
                    require(observations[0] == observations[1], 'fragmentation changed rejection status/headers/body')
            server.exchange(); server.idle(baseline)
        return dict(total_cases=len(results), cases=results,
                    policy='Always require a complete HTTP response. Record EOF/RST separately; bounded cleanup can cause RST on excessive remaining input.')

    def fatal_cleanup(self):
        cases = []
        expected = {'wait-fatal':'等待epoll事件失败', 'listener-error':'监听socket出现错误或挂断',
                    'accept-fatal':'接收客户端连接失败', 'delete-failure':'取消epoll登记失败',
                    'pause-del-failure':'暂停接收新连接失败',
                    'resume-add-failure':'恢复监听socket的epoll登记失败',
                    'close-eintr':'关闭客户端描述符失败'}
        for name in ('empty','wait-fatal','listener-error','accept-fatal','delete-failure','close-eintr',
                     'pause-del-failure','resume-add-failure'):
            log = self.root/(name+'.log')
            out = self.command([self.build/'epoll_server_fault_checks',name,log], 'fatal-'+name, timeout=15)
            require('PASS '+name in out, out)
            if name in expected: require(expected[name] in log.read_text(), 'missing fatal cleanup error log')
            cases.append(dict(name=name, output=out.strip()))
        return dict(cases=cases, scope='Real TCP clients, injected fatal paths (accept uses EBADF, not recoverable EMFILE); verify client/listener/epoll fds before the test process exits. Injected close EINTR follows an actual successful close and must not be retried.')

    def receive_timeout(self):
        examples = [('empty', b'', None), ('incomplete-line', b'GET /hel', None),
                    ('get', PREFIX, expected_response(408,b'Request Timeout\n')),
                    ('head', b'HEAD /hello HTTP/1.1\r\nHost: localhost\r\n', expected_response(408,b'Request Timeout\n',head=True)),
                    ('http10', b'GET /hello HTTP/1.0\r\n', expected_response(408,b'Request Timeout\n',version='HTTP/1.0'))]
        with Server(self, 'receive-timeout') as server:
            baseline = server.fds()
            def one(item):
                name,raw,wanted = item; begin = time.monotonic(); started = time.time()
                with server.connect(timeout=40) as client:
                    if raw: client.sendall(raw)
                    data = read_all(client)
                elapsed = time.monotonic()-begin; require(29 <= elapsed < 36, 'wrong receive idle timeout')
                require(not data if wanted is None else parse_response(data,started)==wanted, 'receive timeout response mismatch')
                return dict(name=name, seconds=elapsed, response_hex=data.hex())
            with ThreadPoolExecutor(max_workers=len(examples)) as pool: results = list(pool.map(one,examples))
            server.idle(baseline); require(server.count('接收等待超时') == len(examples), 'receive timeout logs')
            server.exchange(); server.idle(baseline)
            return dict(actual_clock=True, idle_seconds=30, cases=results, final_fds=server.fds())

    def request_deadline(self):
        examples = [('get', PREFIX, False, expected_response(408,b'Request Timeout\n')),
                    ('head', b'HEAD /hello HTTP/1.1\r\nHost: localhost\r\nX-Hold: ', False, expected_response(408,b'Request Timeout\n',head=True)),
                    ('http10', b'GET /hello HTTP/1.0\r\nX-Hold: ', False, expected_response(408,b'Request Timeout\n',version='HTTP/1.0')),
                    ('complete-at-50s', PREFIX, True, expected_response(body=b'Hello,HTTP!\n'))]
        with Server(self,'request-deadline') as server:
            baseline = server.fds()
            def one(item):
                name,raw,finish,wanted = item; begin = time.monotonic(); started = time.time()
                with server.connect(timeout=70) as client:
                    client.sendall(raw); plan = [(20,b'a'),(40,b'b')]+([(50,SUFFIX)] if finish else [])
                    events = []
                    for at,data in plan:
                        delay = begin+at-time.monotonic()
                        if delay>0: time.sleep(delay)
                        client.sendall(data); events.append(dict(seconds=time.monotonic()-begin,bytes=len(data)))
                    data = read_all(client)
                elapsed = time.monotonic()-begin
                require((49 <= elapsed < 56) if finish else (59 <= elapsed < 66), 'wrong fixed request deadline')
                require(parse_response(data,started)==wanted, 'fixed deadline response mismatch')
                return dict(name=name,seconds=elapsed,fragments=events)
            with ThreadPoolExecutor(max_workers=4) as pool: results = list(pool.map(one,examples))
            server.idle(baseline); require(server.count('请求头接收总时限已到')==3, 'total deadline logs')
            require(server.count('接收等待超时')==0, 'trickle hit idle timeout instead of fixed total')
            server.exchange(); server.idle(baseline)
            return dict(actual_clock=True,total_seconds=60,cases=results,
                        starts_at='accepted connection initialization')

    def send_timeout(self):
        results = []
        def direct(mode):
            out = self.command([self.build/'epoll_send_checks',mode,self.root/(mode+'.log')], mode, timeout=60)
            require('PASS '+mode in out, out); return dict(name=mode,output=out.strip())
        def server_check():
            suite = copy.copy(self); suite.binary = self.build/'epoll_response_fixture'
            trace = self.root/'send-fixture-trace.txt'
            with Server(suite,'send-timeout',{'CPP_SEND_FIXTURE_TRACE':str(trace)}) as server:
                baseline = server.fds(); clients = []
                for _ in range(2):
                    client = socket.socket(); server.clients.append(client); clients.append(client)
                    client.setsockopt(socket.SOL_SOCKET,socket.SO_RCVBUF,4096); client.settimeout(5)
                    client.connect(('127.0.0.1',server.port))
                    client.sendall(b'GET /hello HTTP/1.1\r\nHost: localhost\r\nX-Test-Large: 1\r\n\r\n')
                def rows(): return [line.split() for line in trace.read_text().splitlines()] if trace.exists() else []
                wait_until(lambda:sum(row[0]=='FIXTURE' for row in rows())==2,'large response fixtures')
                fds={int(row[1]) for row in rows() if row[0]=='FIXTURE'}
                wait_until(lambda:all(any(row[0]=='SEND' and int(row[1])==fd for row in rows()) for fd in fds),'real partial TCP sends')
                time.sleep(1); mark=time.monotonic(); server.exchange(); latency=time.monotonic()-mark
                require(latency<2,'stalled send blocked healthy connection')
                closures={}; limit=time.monotonic()+37
                while len(closures)<2:
                    require(server.process.poll() is None and time.monotonic()<limit,'stalled send not released')
                    for fd in fds:
                        if fd not in closures and not os.path.lexists(f'/proc/{server.process.pid}/fd/{fd}'):
                            closures[fd]=time.monotonic_ns()
                    time.sleep(.01)
                events=rows(); details=[]
                require(not any(row[0]=='BUFFER_ERROR' for row in events),'buffer setup failure')
                for fd in sorted(fds):
                    sent=[row for row in events if row[0]=='SEND' and int(row[1])==fd]
                    total=next(int(row[3]) for row in events if row[0]=='FIXTURE' and int(row[1])==fd)
                    accepted=sum(int(row[3]) for row in sent)
                    elapsed=(closures[fd]-int(sent[-1][2]))/1e9
                    require(0<accepted<total and 29.75<=elapsed<35,'wrong real scanner send timeout')
                    require(any('tcp_server.cpp:' in line and re.search(r'发送等待超时,fd='+str(fd)+r'$',line)
                                for line in server.log().splitlines()),'missing scanner timeout log')
                    details.append(dict(fd=fd,accepted_bytes=accepted,response_bytes=total,seconds_after_last_send=elapsed))
                server.idle(baseline)
                for client in clients:client.close()
                server.exchange();server.idle(baseline)
                return dict(name='two-real-stalled-TCP-clients',cases=details,health_during_stall_seconds=latency,final_fds=server.fds())
        with ThreadPoolExecutor(max_workers=3) as pool:
            for future in as_completed([pool.submit(direct,'full-buffer-30s'),pool.submit(direct,'progress-at-20s'),pool.submit(server_check)]):
                results.append(future.result())
        return dict(actual_clock=True,idle_seconds=30,cases=results,
                    scope='Real kernel backpressure, no EAGAIN/clock injection. Direct current sendResponse plus current main/event loop with test-only 8 MiB response fixture; production routes unchanged.')


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--only',choices=GROUPS,action='append')
    parser.add_argument('--skip-timeouts',action='store_true',help='Quick checks, never full acceptance')
    parser.add_argument('--report',type=Path)
    args=parser.parse_args(); groups=list(dict.fromkeys(args.only or GROUPS))
    if args.skip_timeouts:groups=[name for name in groups if name not in TIMEOUTS]
    if not groups:parser.error('no groups selected')
    root=Path(tempfile.mkdtemp(prefix='cppserver-epoll-checks-'));suite=Suite(root)
    report=dict(started_at=datetime.now(timezone.utc).isoformat(),artifacts=str(root),selected_groups=groups,
                source_before=source_hashes(),results=[],fatal=None,
                configuration=dict(mode='nonblocking LT epoll',receive_idle_seconds=30,send_idle_seconds=30,request_head_seconds=60,
                                   drain_total_seconds=1,drain_idle_milliseconds=100,drain_max_bytes=65536,
                                   max_connections=1024,shutdown_grace_seconds=5,accept_retry_seconds=1,
                                   max_accept_calls_per_event=32,max_consecutive_accept_errors=32,
                                   logger_time_fallback='time-unavailable'),
                isolation='Fresh assertions/UBSan build for regular and injected-fault checks; cold-start real fd exhaustion uses a separately built assertions/Release main because GCC 11 UBSan vptr checking itself needs pipe descriptors. Only test-child bind is redirected to temporary loopback ports. Original deadlines unchanged; owned processes only. Faults and enlarged response fixtures are recorded per group.')
    def run_group(name):
        print('RUN',name,flush=True); begin=time.monotonic(); item=dict(name=name,passed=False)
        try:item['details']=getattr(suite,name.replace('-','_'))();item['passed']=True
        except Exception:item['error']=traceback.format_exc()
        item['seconds']=round(time.monotonic()-begin,6);report['results'].append(item)
        print(('PASS' if item['passed'] else 'FAIL')+' '+name+f' ({item["seconds"]:.2f}s)',flush=True)
        if not item['passed']:print(item['error'],flush=True)
    print('ARTIFACTS',root,flush=True)
    try:
        suite.prepare();report['fresh_build']='passed: assertions/UBSan tools plus separate assertions/Release main for real fd exhaustion'
        binaries={p.name:p for p in [suite.binary,suite.build/'epoll_connection_checks',suite.build/'epoll_send_checks',suite.build/'epoll_server_fault_checks',suite.build/'epoll_response_fixture',suite.build/'epoll_observed_server',suite.build/'logger_time_checks',suite.shim]}
        binaries['fd-limit-main']=suite.fd_binary
        report['binary_sha256']={name:hashlib.sha256(p.read_bytes()).hexdigest() for name,p in binaries.items()}
        for name in groups:
            if name not in TIMEOUTS:run_group(name)
        with ThreadPoolExecutor(max_workers=3) as pool:
            for future in as_completed([pool.submit(run_group,name) for name in groups if name in TIMEOUTS]):future.result()
        report['binaries_unchanged']=report['binary_sha256']=={name:hashlib.sha256(p.read_bytes()).hexdigest() for name,p in binaries.items()}
    except Exception:report['fatal']=traceback.format_exc();print(report['fatal'],flush=True)
    finally:
        for process in suite.processes:
            if process.poll() is None:
                process.terminate()
                try:process.wait(timeout=3)
                except subprocess.TimeoutExpired:process.kill();process.wait(timeout=3)
        report['all_owned_servers_stopped']=all(p.poll() is not None for p in suite.processes)
        report['server_ports']=suite.server_ports;report['source_after']=source_hashes()
        report['source_unchanged']=report['source_before']==report['source_after']
        report['finished_at']=datetime.now(timezone.utc).isoformat()
        report['passed_groups']=sum(item['passed'] for item in report['results'])
        good=(not report['fatal'] and report['passed_groups']==len(groups) and report['source_unchanged']
              and report.get('binaries_unchanged',False) and report['all_owned_servers_stopped'])
        report['full_acceptance']=good and set(groups)==set(GROUPS)
        text=json.dumps(report,ensure_ascii=False,indent=2)+'\n';(root/'report.json').write_text(text)
        if args.report:args.report.parent.mkdir(parents=True,exist_ok=True);args.report.write_text(text)
        print('REPORT',args.report or root/'report.json',flush=True)
        print(f'{report["passed_groups"]}/{len(groups)} groups passed; full_acceptance={report["full_acceptance"]}',flush=True)
    return 0 if good else 1


if __name__=='__main__':raise SystemExit(main())
