#!/usr/bin/env python3
"""检查当前请求头接收步骤；使用临时端口，不要求服务器提供 HTTP 响应。"""
import argparse
from datetime import datetime, timezone, timedelta
import hashlib
import json
import os
from pathlib import Path
import re
import socket
import struct
import subprocess
import tempfile
import time
import traceback

PROJECT = Path(__file__).resolve().parents[1]
ROOT = None
BINARY = None
SHIM = None
PORT = None


def wait_until(predicate, timeout=4):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        if predicate():
            return
        time.sleep(0.01)
    raise AssertionError('等待测试条件超时')


def header(path='/test', length=None):
    prefix = f'GET {path} HTTP/1.1\r\nHost: localhost\r\nX-Test: '.encode()
    if length is None:
        return prefix + b'ok\r\n\r\n'
    assert length >= len(prefix) + 4
    return prefix + b'A' * (length - len(prefix) - 4) + b'\r\n\r\n'


class Server:
    def __init__(self, name, injected=False):
        self.directory = ROOT / name
        self.directory.mkdir()
        self.injected = injected
        self.process = None

    def log(self):
        path = self.directory / 'server.log'
        return path.read_text() if path.exists() else ''

    def output(self):
        return (self.directory / 'stdout.bin').read_bytes()[self.output_start:]

    def wait_log(self, message, count=1, timeout=4):
        wait_until(lambda: self.log().count(message) >= count, timeout)

    def __enter__(self):
        self.stdout = (self.directory / 'stdout.bin').open('wb')
        self.stderr = (self.directory / 'stderr.txt').open('wb')
        env = os.environ.copy()
        env.pop('LD_PRELOAD', None)
        if self.injected:
            env.update(LD_PRELOAD=str(SHIM), CPP_TCP_TEST_EINTR='1',
                       CPP_TCP_TEST_TRACE=str(self.directory / 'events.txt'))
        self.process = subprocess.Popen([str(BINARY)], cwd=self.directory,
                                        stdout=self.stdout, stderr=self.stderr, env=env)
        try:
            wait_until(lambda: '服务器开始监听' in self.log() or self.process.poll() is not None)
            assert self.process.poll() is None, self.log()
            assert '服务器开始监听' in self.log(), self.log()
            self.output_start = (self.directory / 'stdout.bin').stat().st_size
            return self
        except BaseException:
            self.__exit__(None, None, None)
            raise

    def __exit__(self, *_):
        if self.process is not None and self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait(timeout=3)
        self.stdout.close()
        self.stderr.close()


def connect():
    client = socket.create_connection(('127.0.0.1', PORT), timeout=3)
    client.settimeout(3)
    return client


def closed(client, allow_reset=False):
    try:
        assert client.recv(1) == b'', '当前步骤不应发送响应数据'
    except ConnectionResetError:
        if not allow_reset:
            raise


def incomplete(server, client):
    server.wait_log('开始处理客户端')
    client.settimeout(0.15)
    try:
        client.recv(1)
    except socket.timeout:
        pass
    else:
        raise AssertionError('结束标记尚未收齐，服务器已经关闭或发送数据')
    finally:
        client.settimeout(3)
    assert server.output() == b'', '结束标记尚未收齐就打印了请求'
    assert '结束处理客户端' not in server.log()


def check_output(server, payload, count=1):
    server.wait_log('结束处理客户端', count)
    assert server.output() == payload + b'\n', repr(server.output()[:150])


def basic():
    payload = header('/basic')
    with Server('basic') as server, connect() as client:
        client.sendall(payload)
        closed(client)
        check_output(server, payload)
    return '完整请求头逐字节打印，处理后关闭连接'


def split_line():
    payload = header('/split-line')
    with Server('split_line') as server, connect() as client:
        client.sendall(payload[:8])
        incomplete(server, client)
        client.sendall(payload[8:])
        closed(client)
        check_output(server, payload)
    return '请求行分两次发送；第一段不打印、不关闭，补齐后完整打印'


def split_terminator():
    payload = header('/split-end')
    for split in (1, 2, 3):
        with Server(f'split_terminator_{split}') as server, connect() as client:
            client.sendall(payload[:-4 + split])
            incomplete(server, client)
            client.sendall(payload[-4 + split:])
            closed(client)
            check_output(server, payload)
    return 'CRLFCRLF 的三个内部切分位置全部通过，未提前打印或关闭'


def bytewise():
    payload = header('/bytewise')
    with Server('bytewise') as server, connect() as client:
        for value in payload[:-1]:
            client.sendall(bytes([value]))
            time.sleep(0.002)
        incomplete(server, client)
        client.sendall(payload[-1:])
        closed(client)
        check_output(server, payload)
    return '逐字节发送，最后一个结束标记字节到达后才打印；未假设 send 与 recv 一一对应'


def multi_buffer():
    payload = header('/large-header', 6000)
    with Server('multi_buffer') as server, connect() as client:
        client.sendall(payload)
        closed(client)
        check_output(server, payload)
    return '6000 字节请求头跨越多个 1024 字节接收缓冲区，完整一致'


def exact_limit():
    payload = header('/exact-limit', 8192)
    with Server('exact_limit') as server, connect() as client:
        client.sendall(payload)
        closed(client)
        check_output(server, payload)
        assert '[WARN]' not in server.log(), server.log()
    return '包含结束标记共 8192 字节，正常打印，没有超长警告'


def incomplete_limit():
    with Server('incomplete_limit') as server, connect() as client:
        start = time.monotonic()
        client.sendall(b'A' * 8192)
        closed(client)
        elapsed = time.monotonic() - start
        server.wait_log('累积数据达到上限')
        server.wait_log('结束处理客户端')
        assert server.output() == b''
    return f'8192 字节且无结束标记，{elapsed:.3f} 秒关闭并记录 WARN，没有等待接收超时'


def oversized():
    payload = header('/over-limit', 8193)
    with Server('oversized') as server, connect() as client:
        # 保持第一部分小于上限，第二部分一次补齐结束标记。
        client.sendall(payload[:8191])
        incomplete(server, client)
        client.sendall(payload[8191:])
        closed(client)
        server.wait_log('请求头长度超出上限')
        server.wait_log('结束处理客户端')
        assert server.output() == b''
    return '结束标记补齐后总长 8193 字节，触发完整请求头超长分支并关闭'


def trailing_data():
    payload = header('/with-extra')
    with Server('trailing_data') as server, connect() as client:
        client.sendall(payload + b'EXTRA_BYTES_NOT_PART_OF_HEADER')
        closed(client, allow_reset=True)
        check_output(server, payload)
    return '请求头后附带额外字节，只打印到第一个结束标记，未把额外字节计入头部'


def half_close():
    payload = header('/half-close')
    with Server('half_close') as server, connect() as client:
        client.sendall(payload)
        client.shutdown(socket.SHUT_WR)
        closed(client)
        check_output(server, payload)
    return '完整请求后客户端关闭发送方向，服务端仍完整打印并关闭'


def early_eof():
    partial = b'GET /never-complete HTTP/1.1\r\nHost: localhost\r\n'
    followup = header('/after-early-eof')
    with Server('early_eof') as server:
        with connect() as client:
            client.sendall(partial)
            incomplete(server, client)
            client.shutdown(socket.SHUT_WR)
            closed(client)
        server.wait_log('结束处理客户端')
        with connect() as client:
            client.sendall(followup)
            closed(client)
        server.wait_log('结束处理客户端', 2)
        assert partial not in server.output()
        assert server.output().endswith(followup + b'\n')
        assert '[ERROR]' not in server.log(), server.log()
    return '不完整请求提前 EOF，未打印成完整请求，连接释放且后续请求正常'


def reset():
    followup = header('/after-reset')
    with Server('reset') as server:
        client = connect()
        try:
            client.sendall(b'GET /reset HTTP/1.1\r\n')
            incomplete(server, client)
            client.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack('ii', 1, 0))
        finally:
            client.close()
        server.wait_log('接收失败')
        server.wait_log('结束处理客户端')
        with connect() as client:
            client.sendall(followup)
            closed(client)
        check_output(server, followup, 2)
    return '请求未完成时客户端 RST，记录接收失败，后续连接正常打印'


def reconnect():
    with Server('reconnect') as server:
        descriptors = Path(f'/proc/{server.process.pid}/fd')
        before = len(list(descriptors.iterdir()))
        expected = b''
        for index in range(40):
            payload = header(f'/repeat-{index}')
            with connect() as client:
                client.sendall(payload)
                closed(client)
            expected += payload + b'\n'
            server.wait_log('结束处理客户端', index + 1)
        wait_until(lambda: len(list(descriptors.iterdir())) == before)
        assert server.output() == expected
    return f'连续 40 次连接内容准确，正常路径文件描述符数回到基线 {before}'


def overload():
    first, second = header('/active'), header('/queued')
    with Server('overload') as server, connect() as a:
        a.sendall(first[:-4])
        incomplete(server, a)
        with connect() as b:
            b.sendall(second)
            server.wait_log('接收客户端连接', 2)
            with connect() as c:
                closed(c, allow_reset=True)
            server.wait_log('ThreadPool task queue is full')
            assert server.output() == b''
            assert server.log().count('开始处理客户端') == 1
            a.sendall(first[-4:])
            closed(a)
            closed(b)
        server.wait_log('结束处理客户端', 2)
        assert server.output() == first + b'\n' + second + b'\n'
        assert server.log().count('开始处理客户端') == 2
    return 'A 等待补齐请求头，B 排队，C 被拒绝；补齐 A 后按顺序打印 A、B'


def injected():
    payload = header('/eintr')
    with Server('injected', injected=True) as server, connect() as client:
        client.sendall(payload)
        closed(client)
        check_output(server, payload)
        events = (server.directory / 'events.txt').read_text()
        assert events.count('ACCEPT_EINTR') == 1, events
        assert events.count('RECV_EINTR') == 1, events
        assert '[ERROR]' not in server.log(), server.log()
    return '注入 accept、recv 的 EINTR 各一次，重试后请求头仍完整；不代表真实信号测试'


def receive_timeout():
    followup = header('/after-timeout')
    with Server('receive_timeout') as server, connect() as client:
        client.sendall(b'GET /partial-timeout HTTP/1.1\r\n')
        incomplete(server, client)
        client.settimeout(38)
        start = time.monotonic()
        closed(client)
        elapsed = time.monotonic() - start
        assert 27 <= elapsed <= 38, elapsed
        server.wait_log('客户端接收超时')
        server.wait_log('结束处理客户端')
        assert server.output() == b''
        with connect() as next_client:
            next_client.sendall(followup)
            closed(next_client)
        check_output(server, followup, 2)
    return f'不完整请求停止发送，真实接收超时约 {elapsed:.2f} 秒，WARN 落盘且后续连接正常'


CASES = {function.__name__: function for function in (
    basic, split_line, split_terminator, bytewise, multi_buffer, exact_limit,
    incomplete_limit, oversized, trailing_data, half_close, early_eof, reset,
    reconnect, overload, injected, receive_timeout)}


def main():
    global ROOT, PORT, BINARY, SHIM
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--case', action='append', choices=CASES)
    args = parser.parse_args()
    selected = args.case or list(CASES)
    # 由内核选择临时端口；不探测或连接用户的 8080 服务。
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as probe:
        probe.bind(('127.0.0.1', 0))
        PORT = probe.getsockname()[1]
    ROOT = Path(tempfile.mkdtemp(prefix='cppserver-header-checks-'))
    BINARY = ROOT / 'header_server'
    SHIM = ROOT / 'socket_test_shim.so'
    print(f'Artifacts: {ROOT}; temporary port: {PORT}', flush=True)
    source = (PROJECT / 'src/tcp_server.cpp').read_text()
    variant, count = re.subn(r'htons\(8080\)', f'htons({PORT})', source)
    assert count == 1, '预期原程序在 htons(8080) 中设置端口'
    variant = variant.replace('127.0.0.1:8080', f'127.0.0.1:{PORT}')
    server_path = ROOT / 'tcp_server.cpp'
    server_path.write_text(variant)
    subprocess.run(['g++', '-std=c++17', '-pthread', '-Wall', '-Wextra', '-Wpedantic',
                    '-I', str(PROJECT / 'include'), str(PROJECT / 'src/main.cpp'),
                    str(server_path), str(PROJECT / 'src/tcp_connection.cpp'),
                    str(PROJECT / 'src/thread_pool.cpp'), str(PROJECT / 'src/logger.cpp'),
                    '-o', str(BINARY)], check=True, timeout=60)
    if 'injected' in selected:
        subprocess.run(['g++', '-std=c++17', '-shared', '-fPIC',
                        str(PROJECT / 'tests/socket_test_shim.cpp'), '-ldl', '-o', str(SHIM)],
                       check=True, timeout=30)
    files = ['src/main.cpp', 'src/tcp_server.cpp', 'src/tcp_connection.cpp',
             'src/thread_pool.cpp', 'src/logger.cpp', 'include/tcp_server.h',
             'include/tcp_connection.h', 'include/thread_pool.h', 'include/logger.h',
             'tests/run_http_header_checks.py', 'tests/socket_test_shim.cpp']
    fingerprints = {name: hashlib.sha256((PROJECT / name).read_bytes()).hexdigest()
                    for name in files}
    report = {'time': datetime.now(timezone(timedelta(hours=8))).isoformat(),
              'scope': 'request-header reception; no HTTP parsing or response yet',
              'temporary_port': PORT, 'artifacts': str(ROOT), 'sources_sha256': fingerprints,
              'binary_sha256': hashlib.sha256(BINARY.read_bytes()).hexdigest(), 'results': []}
    for name in selected:
        print(f'RUN {name}', flush=True)
        start = time.monotonic()
        try:
            evidence = CASES[name]()
            result = {'case': name, 'passed': True, 'evidence': evidence}
        except Exception as error:
            result = {'case': name, 'passed': False, 'error': str(error)}
            traceback.print_exc()
        result['seconds'] = round(time.monotonic() - start, 3)
        report['results'].append(result)
        print(f'{"PASS" if result["passed"] else "FAIL"} {name}: '
              f'{result.get("evidence", result.get("error"))}', flush=True)
        (ROOT / 'report.json').write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n')
    passed = sum(result['passed'] for result in report['results'])
    print(f'{passed}/{len(selected)} passed; report: {ROOT / "report.json"}', flush=True)
    return int(passed != len(selected))


if __name__ == '__main__':
    raise SystemExit(main())
