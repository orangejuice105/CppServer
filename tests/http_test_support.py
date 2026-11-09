"""Shared HTTP test transport and response assertions; no server-architecture assumptions."""
from datetime import timedelta
from email.utils import format_datetime, parsedate_to_datetime
import os
import re
import socket
import subprocess
import time

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
    reason = {200: 'OK', 400: 'Bad Request', 404: 'Not Found', 405: 'Method Not Allowed',
              408: 'Request Timeout', 414: 'URI Too Long'}[status]
    headers = {'content-type': ['text/plain; charset=utf-8'], 'connection': ['close']}
    if status == 405:
        headers['allow'] = ['GET']
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
            if key == 'LD_PRELOAD' or key.startswith(('CPP_TCP_TEST_', 'CPP_HTTP_TEST_', 'CPP_EPOLL_TEST_', 'CPP_SEND_FIXTURE_')):
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
