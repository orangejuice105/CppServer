"""Real capacity, request-log privacy, and graceful-shutdown acceptance checks."""
import copy
from pathlib import Path
import re
import select
import signal
import socket
import subprocess
import time

from http_test_support import (
    PREFIX, SUFFIX, expected_response, parse_response, read_all, request,
    require, wait_until,
)


def epoll_watch_count(server):
    for fd in Path(f'/proc/{server.process.pid}/fd').iterdir():
        try:
            if str(fd.readlink()) == 'anon_inode:[eventpoll]':
                info = Path(f'/proc/{server.process.pid}/fdinfo/{fd.name}').read_text()
                return len(re.findall(r'^tfd:', info, re.MULTILINE))
        except FileNotFoundError:
            continue
    return None


def stopped(server, code=0, timeout=8):
    actual = server.process.wait(timeout=timeout)
    require(actual == code, f'Expected exit {code}, got {actual}: {server.log()}')
    return actual


def finish_request(client):
    started = time.time()
    client.sendall(SUFFIX)
    response = parse_response(read_all(client), started)
    require(response == expected_response(body=b'Hello,HTTP!\n'), 'Shutdown interrupted response')
    client.close()


def open_held_clients(server, count, baseline):
    clients = []
    for _ in range(count):
        client = server.connect()
        client.sendall(PREFIX)
        clients.append(client)
        # Setup pacing only: avoid SYN retransmission against the production
        # backlog of 5, so early clients do not expire while filling the map.
        if len(clients) % 4 == 0 or len(clients) == count:
            wait_until(lambda: server.fds() == baseline + len(clients), 'held clients accepted')
    return clients


def check_capacity(suite, Server):
    limit, rejected = 1024, 101
    with Server(suite, 'capacity') as server:
        baseline = server.fds()
        held = open_held_clients(server, limit, baseline)
        wait_until(lambda: server.fds() == baseline + limit, '1024 managed clients')
        require(epoll_watch_count(server) == limit + 2, 'Wrong client registration count')
        accepted_at_limit = server.count('接收客户端连接,fd=')
        endings = []
        for _ in range(rejected):
            with server.connect() as client:
                try:
                    require(client.recv(1) == b'', 'Over-capacity client received data')
                    endings.append('eof')
                except ConnectionResetError:
                    endings.append('reset')
        require(server.fds() == baseline + limit, 'Over-capacity connection leaked a descriptor')
        require(server.count('接收客户端连接,fd=') == accepted_at_limit,
                'Over-capacity client entered the managed connection path')
        server.wait_log('rejected = 100,')
        counts = [int(n) for n in re.findall(r'rejected\s*=\s*(\d+)', server.log())]
        require(counts == [1, 100], 'Capacity warning throttling mismatch: ' + repr(counts))

        held.pop().close()
        wait_until(lambda: server.fds() == baseline + limit - 1, 'one capacity slot released')
        server.exchange()
        wait_until(lambda: server.fds() == baseline + limit - 1, 'replacement request cleaned up')
        for client in held:
            client.close()
        server.idle(baseline)
        server.exchange()
        server.idle(baseline)
        return dict(limit=limit, rejected_connections=rejected, rejection_endings=endings,
                    warning_counts=counts, replacement_request_passed=True,
                    baseline_fds=baseline, final_fds=server.fds())


def check_request_logging(suite, Server):
    # Synthetic markers only; never send or save real credentials in the test.
    markers = ['TEST_QUERY_SECRET_9a72', 'TEST_AUTH_SECRET_9a72',
               'TEST_COOKIE_SECRET_9a72', 'TEST_HEADER_SECRET_9a72']
    with Server(suite, 'request-logging') as server:
        raw = (f'GET /hello?token={markers[0]} HTTP/1.1\r\nHost: localhost\r\n'
               f'Authorization: Bearer {markers[1]}\r\nCookie: session={markers[2]}\r\n'
               f'X-Test: {markers[3]}\r\n\r\n').encode()
        server.exchange(raw, expected_response(body=b'Hello,HTTP!\n'))
        server.wait_log('方法=GET,路径=/hello,版本=HTTP/1.1,fd=')
        invalid = raw.replace(b'X-Test:', b'Bad Header:')
        server.exchange(invalid, expected_response(400, b'Bad Request\n'))
        server.process.send_signal(signal.SIGTERM)
        stopped(server)
        text = server.log() + (server.directory/'stdout.bin').read_text(errors='replace')
        require(all(marker not in text for marker in markers), 'Sensitive marker leaked into server output')
        require('请求头:' not in text, 'Full request-header logging unexpectedly enabled')
        require('HTTP请求格式错误,fd=' in text, 'Malformed-request diagnostic missing')
        return dict(valid_and_malformed_requests=True, synthetic_secret_markers=len(markers),
                    query_authorization_cookie_and_header_values_absent=True,
                    method_path_version_fd_present=True, logs_flushed_on_exit=True)


def check_graceful_shutdown(suite, Server):
    cases = []
    for stop_signal in (signal.SIGINT, signal.SIGTERM):
        with Server(suite, 'stop-idle-' + stop_signal.name) as server:
            baseline = server.fds()
            server.exchange()
            server.idle(baseline)
            began = time.monotonic()
            server.process.send_signal(stop_signal)
            stopped(server, timeout=3)
            require(f'signal={int(stop_signal)}' in server.log(), 'Signal reason not flushed')
            require('[ERROR]' not in server.log(), 'Idle shutdown reported an error')
            cases.append(dict(name='idle-' + stop_signal.name, exit_code=0,
                              seconds=time.monotonic()-began))

    with Server(suite, 'stop-finish-existing') as server:
        clients = [server.connect(), server.connect()]
        for client in clients:
            client.sendall(PREFIX)
        server.wait_log('接收客户端连接,fd=', 2)
        wait_until(lambda: epoll_watch_count(server) == 4, 'two clients registered')
        server.process.send_signal(signal.SIGTERM)
        server.wait_log('收到服务器退出请求')
        wait_until(lambda: epoll_watch_count(server) == 3, 'listener removed from epoll')
        require(server.process.poll() is None, 'Shutdown discarded existing clients')
        require(not select.select(clients, [], [], .1)[0], 'Partial request closed prematurely')
        late = server.connect()
        late.sendall(request())
        require(not select.select([late], [], [], .15)[0], 'New request processed while stopping')
        require(server.count('接收客户端连接,fd=') == 2 and epoll_watch_count(server) == 3,
                'New client accepted after listener removal')
        finish_request(clients[0])
        wait_until(lambda: epoll_watch_count(server) == 2, 'first client released')
        require(server.process.poll() is None, 'Shutdown ignored the second client')
        server.process.send_signal(signal.SIGINT)
        server.wait_log('收到服务器退出请求', 2)
        finish_request(clients[1])
        stopped(server)
        require('[ERROR]' not in server.log() and '优雅退出等待超时' not in server.log(),
                'Normal completion was treated as forced shutdown')
        cases.append(dict(name='finish-existing-and-repeat', completed_responses=2,
                          late_client_processed=False, repeated_signal_passed=True, exit_code=0))

    with Server(suite, 'stop-peer-write-open') as server:
        client = server.connect()
        started = time.time()
        client.sendall(request())
        require(parse_response(read_all(client), started) == expected_response(), 'Drain response')
        # Leave the client sending direction open after receiving the server FIN.
        began = time.monotonic()
        server.process.send_signal(signal.SIGTERM)
        stopped(server, timeout=3)
        require('优雅退出等待超时' not in server.log(), 'Quiet drain used the shutdown deadline')
        cases.append(dict(name='peer-write-direction-open', seconds=time.monotonic()-began, exit_code=0))

    for repeat in (False, True):
        with Server(suite, 'stop-deadline-' + str(repeat)) as server:
            client = server.connect()
            client.sendall(PREFIX)
            server.wait_log('接收客户端连接,fd=')
            began = time.monotonic()
            server.process.send_signal(signal.SIGTERM)
            server.wait_log('收到服务器退出请求')
            if repeat:
                time.sleep(2)
                require(server.process.poll() is None, 'Exited before the grace period')
                server.process.send_signal(signal.SIGINT)
                server.wait_log('收到服务器退出请求', 2)
            stopped(server)
            elapsed = time.monotonic()-began
            require(4.8 <= elapsed < 6.5, 'Shutdown deadline reset or wrong: ' + str(elapsed))
            client.settimeout(2)
            require(read_all(client) == b'', 'Incomplete request unexpectedly received a response')
            require('优雅退出等待超时，关闭剩余连接,remaining=1' in server.log(), 'Missing forced-close count')
            require('[ERROR]' not in server.log(), 'Grace expiry was treated as a server failure')
            cases.append(dict(name='receive-deadline-repeat' if repeat else 'receive-deadline',
                              seconds=elapsed, repeated_after_seconds=2 if repeat else None, exit_code=0))

    fixture_suite = copy.copy(suite)
    fixture_suite.binary = suite.build/'epoll_response_fixture'
    trace = suite.root/'stop-send-trace.txt'
    with Server(fixture_suite, 'stop-sending', {'CPP_SEND_FIXTURE_TRACE': str(trace)}) as server:
        client = socket.socket()
        server.clients.append(client)
        client.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096)
        client.settimeout(4)
        client.connect(('127.0.0.1', server.port))
        client.sendall(b'GET /hello HTTP/1.1\r\nHost: localhost\r\nX-Test-Large: 1\r\n\r\n')
        def rows():
            return [line.split() for line in trace.read_text().splitlines()] if trace.exists() else []
        wait_until(lambda: any(row[0] == 'SEND' for row in rows()), 'real TCP partial send')
        total = next(int(row[3]) for row in rows() if row[0] == 'FIXTURE')
        def stalled():
            sent = [row for row in rows() if row[0] == 'SEND']
            return (0 < sum(int(row[3]) for row in sent) < total and
                    time.monotonic_ns()-int(sent[-1][2]) >= 250_000_000)
        # sendResponse yields after any positive send. Kernel backpressure can stop
        # EPOLLOUT without a subsequent send() ever returning EAGAIN.
        wait_until(stalled, 'partial response with no further send progress')
        accepted_before_stop = sum(int(row[3]) for row in rows() if row[0] == 'SEND')
        began = time.monotonic()
        server.process.send_signal(signal.SIGTERM)
        stopped(server)
        elapsed = time.monotonic()-began
        require(4.8 <= elapsed < 6.5, 'Stalled send prevented shutdown')
        received = read_all(client)
        require(received.startswith(b'HTTP/1.1 200 OK\r\n') and 0 < len(received) < total,
                'Fixture did not remain partially sent')
        require('优雅退出等待超时，关闭剩余连接,remaining=1' in server.log(), 'Missing stalled-send shutdown log')
        cases.append(dict(name='real-backpressured-send', seconds=elapsed, exit_code=0,
                          response_bytes=total, accepted_before_stop=accepted_before_stop,
                          received_bytes=len(received), complete_response=False,
                          scope='Test-only 8 MiB response; production IO, clocks and shutdown policy'))

    fault_modes = [('eintr', 0, None), ('again', 0, None),
                   ('error', 1, '读取退出信号失败'), ('short', 1, '退出信号读取长度异常')]
    for mode, code, error_log in fault_modes:
        with Server(suite, 'stop-read-' + mode, {'CPP_EPOLL_TEST_SIGNAL_READ': mode}) as server:
            server.process.send_signal(signal.SIGTERM)
            stopped(server, code, timeout=3)
            require('SIGNAL_READ_' in server.trace(), 'Signal read fault not injected')
            if error_log:
                require(error_log in server.log(), 'Missing signal-read failure diagnostic')
            else:
                require('收到服务器退出请求' in server.log(), 'Read retry lost the pending signal')
            cases.append(dict(name='injected-signal-read-' + mode, exit_code=code, injected=True))

    with Server(suite, 'stop-listener-del-fail', {'CPP_EPOLL_TEST_LISTENER_DEL_FAIL': '1'}) as server:
        client = server.connect()
        client.sendall(PREFIX)
        server.wait_log('接收客户端连接,fd=')
        server.process.send_signal(signal.SIGTERM)
        stopped(server, 1, timeout=3)
        require('LISTENER_DEL_FAIL' in server.trace(), 'Listener DEL fault not exercised')
        require('取消监听socket的epoll登记失败' in server.log(), 'Missing listener DEL failure log')
        require(read_all(client) == b'', 'Fatal stop path left a response')
        cases.append(dict(name='injected-listener-del-failure', injected=True, exit_code=1))

    startup = Server(suite, 'stop-signal-add-fail', {'CPP_EPOLL_TEST_SIGNAL_ADD_FAIL': '1'})
    process = subprocess.Popen([str(suite.binary)], cwd=startup.directory, env=startup.env,
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    suite.processes.append(process)
    try:
        output, errors = process.communicate(timeout=5)
        (startup.directory/'stdout.bin').write_bytes(output)
        (startup.directory/'stderr.txt').write_bytes(errors)
        require(process.returncode == 1 and not errors, 'Signal ADD failure exit mismatch')
        require('SIGNAL_ADD_FAIL' in startup.trace(), 'Signal ADD fault not exercised')
        require('登记退出信号fd到epoll失败' in startup.log(), 'Missing signal ADD diagnostic')
    finally:
        if process.poll() is None:
            process.kill()
            process.wait(timeout=3)
    cases.append(dict(name='injected-signal-add-failure', injected=True, exit_code=1))
    return dict(actual_signals=True, grace_seconds=5, cases=cases,
                scope='Real SIGINT/SIGTERM, unmodified clocks and bounded shutdown. '
                      'Syscall faults and the enlarged-response sending fixture are explicitly labeled. '
                      'A TCP handshake can still complete against the underegistered listener; '
                      'new clients must not be accepted or processed.')
