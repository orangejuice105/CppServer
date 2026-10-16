#!/usr/bin/env python3
"""Linux TCP Echo 阶段检查。运行前需释放 127.0.0.1:8080。"""
import argparse
from concurrent.futures import ThreadPoolExecutor
from datetime import datetime, timedelta, timezone
import errno
import hashlib
import json
import os
from pathlib import Path
import re
import socket
import struct
import subprocess
import tempfile
import threading
import time
import traceback

PROJECT = Path(__file__).resolve().parents[1]
BINARY = PROJECT / 'build/main'
ROOT = None
SHIM = None
MULTI = None


def wait_until(predicate, timeout=5, description='condition'):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        if predicate():
            return
        time.sleep(0.02)
    raise AssertionError(f'Timed out waiting for {description}')


class Server:
    def __init__(self, name, binary=BINARY, overrides=None):
        self.directory = ROOT / name
        self.directory.mkdir()
        self.binary = binary
        self.env = os.environ.copy()
        self.env.pop('LD_PRELOAD', None)
        self.env.update(overrides or {})
        self.process = None

    def log(self):
        path = self.directory / 'server.log'
        return path.read_text() if path.exists() else ''

    def wait_log(self, text, count=1, timeout=5):
        wait_until(lambda: self.log().count(text) >= count, timeout, text)

    def __enter__(self):
        self.output = (self.directory / 'stdout.bin').open('wb')
        self.errors = (self.directory / 'stderr.txt').open('wb')
        self.process = subprocess.Popen([str(self.binary)], cwd=self.directory,
                                        env=self.env, stdout=self.output, stderr=self.errors)
        try:
            wait_until(lambda: '服务器开始监听' in self.log() or self.process.poll() is not None,
                       description='server startup')
            assert self.process.poll() is None, self.log()
            assert '服务器开始监听' in self.log(), self.log()
            return self
        except BaseException:
            self.__exit__(None, None, None)
            raise

    def __exit__(self, *_):
        if self.process and self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait(timeout=3)
        self.output.close()
        self.errors.close()


def connect():
    client = socket.create_connection(('127.0.0.1', 8080), timeout=4)
    client.settimeout(4)
    return client


def receive_exact(client, length):
    result = bytearray()
    while len(result) < length:
        chunk = client.recv(length - len(result))
        assert chunk, f'Unexpected EOF after {len(result)}/{length} bytes'
        result.extend(chunk)
    return bytes(result)


def echo(client, payload):
    client.sendall(payload)
    assert receive_exact(client, len(payload)) == payload, 'Echo bytes differ'


def basic():
    with Server('basic') as server, connect() as client:
        for payload in (b'hello\n', '你好，TCP\n'.encode(), b'\x00\xff\x01\x00'):
            echo(client, payload)
        for piece in (b'a', b'bc', b'defg'):
            client.sendall(piece)
        assert receive_exact(client, 7) == b'abcdefg'
        assert server.process.poll() is None
    return '同一连接连续收发、UTF-8、二进制和分段发送，逐字节相同'


def large():
    payload = bytes(range(256)) * 1024
    with Server('large'), connect() as client:
        echo(client, payload)
    return f'{len(payload)} 字节（256 KiB），超过 1024 字节缓冲区，完整一致'


def half_close():
    with Server('half_close') as server, connect() as client:
        payload = b'finish-writing-but-still-read'
        client.sendall(payload)
        client.shutdown(socket.SHUT_WR)
        assert receive_exact(client, len(payload)) == payload
        assert client.recv(1) == b''
        server.wait_log('结束处理客户端')
    return '客户端结束发送后仍收到完整回显，随后读到 EOF'


def overload():
    with Server('overload') as server:
        with connect() as a:
            echo(a, b'active-A')
            with connect() as b:
                b.sendall(b'queued-B')
                server.wait_log('接收客户端连接', count=2)
                assert server.log().count('开始处理客户端') == 1
                with connect() as c:
                    try:
                        assert c.recv(1) == b'', 'Rejected client unexpectedly received data'
                    except ConnectionResetError:
                        pass
                server.wait_log('ThreadPool task queue is full')
                echo(a, b'A-still-works')
                a.shutdown(socket.SHUT_WR)
                assert a.recv(1) == b''
                assert receive_exact(b, len(b'queued-B')) == b'queued-B'
        server.wait_log('结束处理客户端', count=2)
        assert server.log().count('开始处理客户端') == 2
    return 'A 执行、B 排队、C 被关闭；A 结束后 B 被处理，拒绝连接未进入工作线程'


def reconnect():
    with Server('reconnect') as server:
        before = len(list(Path(f'/proc/{server.process.pid}/fd').iterdir()))
        for index in range(40):
            with connect() as client:
                echo(client, f'connection-{index}'.encode())
            server.wait_log('结束处理客户端', count=index + 1)
        wait_until(lambda: len(list(Path(f'/proc/{server.process.pid}/fd').iterdir())) == before,
                   description='connection descriptors released')
        assert 'ThreadPool task queue is full' not in server.log()
    return f'连续连接 40 次，正常路径文件描述符数回到基线 {before}'


def reset():
    with Server('reset') as server:
        client = connect()
        echo(client, b'before-reset')
        client.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack('ii', 1, 0))
        client.close()
        server.wait_log('[ERROR]')
        assert '接收失败' in server.log() or '发送失败' in server.log()
        with connect() as following:
            echo(following, b'after-reset')
        assert server.process.poll() is None
    return '客户端发送 RST，服务器记录错误并继续服务后续连接'


def bind_conflict():
    with Server('bind_first') as server:
        second_dir = ROOT / 'bind_second'
        second_dir.mkdir()
        other = subprocess.run([str(BINARY)], cwd=second_dir, capture_output=True, timeout=5)
        (second_dir / 'stdout.bin').write_bytes(other.stdout)
        (second_dir / 'stderr.txt').write_bytes(other.stderr)
        log = (second_dir / 'server.log').read_text()
        assert other.returncode == 1
        assert '[ERROR]' in log and 'bind失败' in log and f'errno={errno.EADDRINUSE},' in log
        assert '服务器开始监听' not in log
        with connect() as client:
            echo(client, b'original-server-alive')
        assert server.process.poll() is None
    return '第二进程返回 1，记录地址占用；原进程继续回显，各自日志独立'


def multi():
    with Server('multi', binary=MULTI) as server:
        clients = []
        try:
            for index in range(4):
                client = connect()
                clients.append(client)
                echo(client, f'initial-{index}'.encode())
            with ThreadPoolExecutor(max_workers=4) as executor:
                futures = [executor.submit(echo, client, bytes([index]) * 2048)
                           for index, client in enumerate(clients)]
                for result in futures:
                    result.result(timeout=5)
            server.wait_log('开始处理客户端', count=4)
            ids = {re.search(r'\[thread:([^]]+)\]', line).group(1)
                   for line in server.log().splitlines() if '开始处理客户端' in line}
            assert len(ids) == 4, ids
        finally:
            for client in clients:
                client.close()
    return '临时 (4,4) 配置副本：4 个连接均保持打开，4 个工作线程完成并发回显；主程序未改'


def injected():
    trace = ROOT / 'injected-events.txt'
    overrides = {'LD_PRELOAD': str(SHIM), 'CPP_TCP_TEST_EINTR': '1',
                 'CPP_TCP_TEST_CHUNK': '17', 'CPP_TCP_TEST_TRACE': str(trace)}
    with Server('injected', overrides=overrides) as server, connect() as client:
        echo(client, bytes(range(251)) * 20)
        events = trace.read_text()
        assert events.count('ACCEPT_EINTR') == 1, events
        assert events.count('RECV_EINTR') == 1, events
        assert events.count('SEND_EINTR') == 2, events
        assert 'PARTIAL_SEND' in events
        assert '[ERROR]' not in server.log(), server.log()
    return '注入 accept/recv 的 EINTR 各 1 次、send 的 EINTR 2 次；实际分批发送最多 17 字节，5020 字节无丢失重复。未模拟真实信号投递'


def receive_timeout():
    with Server('receive_timeout') as server, connect() as client:
        client.settimeout(38)
        server.wait_log('开始处理客户端')
        start = time.monotonic()
        assert client.recv(1) == b''
        elapsed = time.monotonic() - start
        assert 27 <= elapsed <= 38, elapsed
        server.wait_log('客户端接收超时')
        assert '[WARN]' in server.log()
        with connect() as next_client:
            echo(next_client, b'after-receive-timeout')
    return f'原配置 30 秒接收超时，实测 {elapsed:.2f} 秒；WARN 落盘且后续连接正常'


def send_timeout():
    trace = ROOT / 'send-timeout-events.txt'
    overrides = {'LD_PRELOAD': str(SHIM), 'CPP_TCP_TEST_SMALL_SNDBUF': '1',
                 'CPP_TCP_TEST_TRACE': str(trace)}
    with Server('send_timeout', overrides=overrides) as server:
        client = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        client.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096)
        client.settimeout(45)
        client.connect(('127.0.0.1', 8080))
        def produce():
            try:
                client.sendall(b'x' * (4 * 1024 * 1024))
            except OSError:
                pass
        producer = threading.Thread(target=produce, daemon=True)
        producer.start()
        start = time.monotonic()
        try:
            server.wait_log('客户端发送超时', timeout=40)
            elapsed = time.monotonic() - start
            server.wait_log('结束处理客户端')
        finally:
            client.close()
            producer.join(timeout=3)
        assert not producer.is_alive(), 'Test sender did not stop'
        events = trace.read_text()
        assert 'SMALL_SNDBUF' in events and 'SHIM_FAILURE' not in events, events
        with connect() as next_client:
            echo(next_client, b'after-send-timeout')
    return f'缩小测试连接缓冲区，客户端持续发送但不读响应；约 {elapsed:.2f} 秒记录真实发送超时，后续连接正常'


CASES = {'basic': basic, 'large': large, 'half_close': half_close, 'overload': overload,
         'reconnect': reconnect, 'reset': reset, 'bind_conflict': bind_conflict,
         'multi': multi, 'injected': injected, 'receive_timeout': receive_timeout,
         'send_timeout': send_timeout}


def main():
    global ROOT, SHIM, MULTI
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--case', action='append', choices=CASES)
    args = parser.parse_args()
    selected = args.case or list(CASES)
    source = (PROJECT / 'src/main.cpp').read_text()
    assert re.search(r'ThreadPool\s+pool\(\s*1\s*,\s*1\s*\)', source), 'Expected pool(1,1) test configuration'
    assert re.search(r'timeout\.tv_sec\s*=\s*30\s*;', source), 'Expected 30 second timeout'
    ROOT = Path(tempfile.mkdtemp(prefix='cppserver-tcp-checks-'))
    print(f'Artifacts: {ROOT}', flush=True)
    # 只探测端口；不终止用户已有服务。
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as probe:
            probe.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            probe.bind(('127.0.0.1', 8080))
    except OSError as error:
        if error.errno == errno.EADDRINUSE:
            raise SystemExit('8080 已被占用，请先在自己的服务器终端按 Ctrl+C 停止，再运行检查。')
        if error.errno in (errno.EPERM, errno.EACCES):
            raise SystemExit('当前环境禁止创建或绑定 socket，需要允许本机回环网络测试后重试。')
        raise
    if any(case in selected for case in ('injected', 'send_timeout')):
        SHIM = ROOT / 'socket_test_shim.so'
        subprocess.run(['g++', '-std=c++17', '-shared', '-fPIC', '-Wall', '-Wextra',
                        str(PROJECT / 'tests/socket_test_shim.cpp'), '-ldl', '-o', str(SHIM)],
                       check=True, timeout=30)
    if 'multi' in selected:
        variant, replacements = re.subn(r'ThreadPool\s+pool\(\s*1\s*,\s*1\s*\)',
                                        'ThreadPool pool(4,4)', source)
        assert replacements == 1
        variant_path = ROOT / 'multi_main.cpp'
        variant_path.write_text(variant)
        MULTI = ROOT / 'multi_main'
        subprocess.run(['g++', '-std=c++17', '-pthread', '-I', str(PROJECT / 'include'),
                        str(variant_path), str(PROJECT / 'src/thread_pool.cpp'),
                        str(PROJECT / 'src/logger.cpp'), '-o', str(MULTI)],
                       check=True, timeout=30)
    results = []
    for name in selected:
        print(f'RUN {name}', flush=True)
        started = time.monotonic()
        try:
            evidence = CASES[name]()
            result = {'case': name, 'passed': True, 'evidence': evidence}
        except Exception as error:
            result = {'case': name, 'passed': False, 'error': str(error)}
            traceback.print_exc()
        result['seconds'] = round(time.monotonic() - started, 3)
        results.append(result)
        print(f'{"PASS" if result["passed"] else "FAIL"} {name}: '
              f'{result.get("evidence", result.get("error"))}', flush=True)
        report = {'time': datetime.now(timezone(timedelta(hours=8))).isoformat(),
                  'main_sha256': hashlib.sha256((PROJECT / 'src/main.cpp').read_bytes()).hexdigest(),
                  'binary_sha256': hashlib.sha256(BINARY.read_bytes()).hexdigest(),
                  'artifacts': str(ROOT), 'results': results}
        (ROOT / 'report.json').write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n')
    passed = sum(result['passed'] for result in results)
    print(f'{passed}/{len(results)} passed; report: {ROOT / "report.json"}', flush=True)
    return int(passed != len(results))


if __name__ == '__main__':
    raise SystemExit(main())
