"""Accept recovery over real TCP: explicit faults and one isolated real fd limit."""
import errno
import copy
import os
from pathlib import Path
import resource
import signal
import subprocess
import time

from http_test_support import (PREFIX, SUFFIX, expected_response, parse_response,
                               read_all, request, require, wait_until)

PAUSE = '接收新连接资源不足，暂停接收,监听fd='
RESUME = '恢复监听登记，重新尝试接收新连接,监听fd='
ACCEPT = '接收客户端连接,fd='


def check_accept_recovery(suite, Server):
    results = []
    parent_limit = resource.getrlimit(resource.RLIMIT_NOFILE)

    def overrides(name, error=errno.EMFILE, repeats=1, **extra):
        return dict(CPP_EPOLL_TEST_ACCEPT_ERROR=str(error),
                    CPP_EPOLL_TEST_ACCEPT_REPEATS=str(repeats),
                    CPP_EPOLL_TEST_ACCEPT_TRACE=str(suite.root/name/'accept-events.txt'), **extra)

    def events(server, kind):
        path = server.directory/'accept-events.txt'
        if not path.exists():
            return []
        rows = [line.split() for line in path.read_text().splitlines()]
        return [float(row[0]) for row in rows if len(row) == 3 and row[1] == kind]

    def start_pair(server):
        baseline = server.fds()
        started = time.time()
        held = server.connect()
        held.sendall(PREFIX)
        server.wait_log(ACCEPT)
        pending = server.connect()
        pending.sendall(request())
        return baseline, started, held, pending

    def finish_held(held, started):
        held.sendall(SUFFIX)
        require(parse_response(read_all(held), started) ==
                expected_response(body=b'Hello,HTTP!\n'), 'Existing client did not survive accept failure')
        held.close()

    def finish_pending(pending, started):
        require(parse_response(read_all(pending), started) == expected_response(),
                'Pending client did not receive a complete response')
        pending.close()

    def healthy_stop(server, baseline):
        server.idle(baseline)
        server.exchange()
        server.idle(baseline)
        server.process.send_signal(signal.SIGTERM)
        require(server.process.wait(timeout=3) == 0, 'Normal shutdown exit')
        require('[ERROR]' not in server.log(), 'Unexpected server error: '+server.log())

    class LimitedServer(Server):
        def __enter__(self):
            self.output = (self.directory/'stdout.bin').open('wb')
            self.errors = (self.directory/'stderr.txt').open('wb')

            def limit_files():
                resource.setrlimit(resource.RLIMIT_NOFILE, (64, 64))

            # Only this child gets a lower limit; the runner and user shell are unchanged.
            self.process = subprocess.Popen([str(self.suite.binary)], cwd=self.directory,
                                            env=self.env, stdout=self.output, stderr=self.errors,
                                            preexec_fn=limit_files)
            self.suite.processes.append(self.process)
            try:
                wait_until(lambda: '服务器开始监听' in self.log() or self.process.poll() is not None,
                           'limited server startup')
                require(self.process.poll() is None, 'Limited server startup failed')
                return self
            except BaseException:
                self.__exit__(None, None, None)
                raise

    with Server(suite, 'accept-normal-control') as server:
        healthy_stop(server, server.fds())
        results.append(dict(name='normal-control', injected=False, response=200, exit_code=0))

    network_errors = [('ECONNABORTED', errno.ECONNABORTED), ('ENETDOWN', errno.ENETDOWN),
                      ('EPROTO', errno.EPROTO), ('ENOPROTOOPT', errno.ENOPROTOOPT),
                      ('EHOSTDOWN', errno.EHOSTDOWN), ('ENONET', errno.ENONET),
                      ('EHOSTUNREACH', errno.EHOSTUNREACH), ('EOPNOTSUPP', errno.EOPNOTSUPP),
                      ('ENETUNREACH', errno.ENETUNREACH)]
    for label, error in network_errors:
        name = 'accept-network-'+label
        with Server(suite, name, overrides(name, error)) as server:
            baseline, started, held, pending = start_pair(server)
            finish_pending(pending, started)
            finish_held(held, started)
            warning = '接收新连接遇到可恢复网络错误,监听fd='
            server.wait_log(warning)
            lines = [line for line in server.log().splitlines() if warning in line]
            require(len(events(server, 'accept_error')) == 1 and len(lines) == 1 and
                    '[WARN]' in lines[0] and f',errno={error}' in lines[0],
                    'Network fault was not exercised or logged correctly')
            require(PAUSE not in server.log(), 'Network error incorrectly entered resource backoff')
            healthy_stop(server, baseline)
            results.append(dict(name=label, injected=True, errno=error,
                                existing_response=200, pending_response=200, exit_code=0))

    for error, repeats in ((errno.EMFILE, 1), (errno.ENFILE, 1), (errno.EMFILE, 3)):
        name = 'accept-'+errno.errorcode[error]+'-repeat-'+str(repeats)
        with Server(suite, name, overrides(name, error, repeats)) as server:
            baseline, started, held, pending = start_pair(server)
            server.wait_log(PAUSE)
            require(server.process.poll() is None, 'Server terminated on resource error')
            finish_held(held, started)
            held_finished = time.monotonic()
            finish_pending(pending, started)
            server.wait_log(RESUME, repeats)
            injected = events(server, 'accept_error')
            resumed = events(server, 'listener_add')
            removed = events(server, 'listener_del')
            require(len(injected) == len(resumed) == len(removed) == repeats,
                    'Unexpected pause/resume/injection counts')
            require(all(b-a >= .98 for a, b in zip(injected, resumed)),
                    'Recovery happened before the retry interval')
            require(all(b-a >= .98 for a, b in zip(injected, injected[1:])),
                    'Resource retries were not rate limited')
            require(held_finished < resumed[0], 'Existing client was not tested during pause')
            healthy_stop(server, baseline)
            results.append(dict(name=name, injected=True, errno=error, injections=repeats,
                                existing_response_during_pause=200, pending_response_after_resume=200,
                                fd_count_restored=True, exit_code=0,
                                retry_delays_seconds=[round(b-a, 6) for a, b in zip(injected, resumed)]))

    for stop_signal in (signal.SIGINT, signal.SIGTERM):
        name = 'accept-paused-stop-'+stop_signal.name
        with Server(suite, name, overrides(name)) as server:
            baseline, started, held, pending = start_pair(server)
            server.wait_log(PAUSE)
            server.process.send_signal(stop_signal)
            server.wait_log('收到服务器退出请求,signal=')
            # Cross the retry deadline while an existing request still needs processing.
            time.sleep(1.25)
            require(server.process.poll() is None, 'Did not wait for existing client')
            require(not events(server, 'listener_add') and RESUME not in server.log(),
                    'Listener was resumed during graceful shutdown')
            finish_held(held, started)
            require(server.process.wait(timeout=3) == 0, 'Paused shutdown exit')
            require(len(events(server, 'listener_del')) == 1,
                    'Listener deletion was repeated during paused shutdown')
            require(server.count(ACCEPT) == 1 and '[ERROR]' not in server.log(),
                    'Accepted pending client or logged a shutdown error')
            results.append(dict(name=name, injected=True, real_signal=stop_signal.name,
                                existing_response=200, resumed_during_shutdown=False,
                                listener_deleted_once=True, exit_code=0))

    for mode in ('pause-del-failure', 'resume-add-failure'):
        name = 'accept-'+mode
        extra = dict(CPP_EPOLL_TEST_LISTENER_DEL_FAIL='1') if mode.startswith('pause') else \
                dict(CPP_EPOLL_TEST_LISTENER_RESUME_FAIL='1')
        with Server(suite, name, overrides(name, **extra)) as server:
            start_pair(server)
            require(server.process.wait(timeout=4) == 1, 'Fault did not return exit 1')
            wanted = '暂停接收新连接失败' if mode.startswith('pause') else \
                     '恢复监听socket的epoll登记失败'
            require(wanted in server.log() and f'errno={errno.EIO}' in server.log(),
                    'Failed epoll operation was not logged')
            require(RESUME not in server.log(), 'Falsely reported successful recovery')
            results.append(dict(name=name, injected=True, errno=errno.EIO, exit_code=1,
                                success_not_reported=True))

    real_suite = copy.copy(suite)
    real_suite.binary = suite.fd_binary
    with LimitedServer(real_suite, 'accept-real-nofile-64') as server:
        baseline = server.fds()
        started = time.time()
        held = []
        pending = None
        for _ in range(80):
            client = server.connect()
            client.sendall(PREFIX)
            expected = len(held)+1
            wait_until(lambda: server.count(ACCEPT) >= expected or PAUSE in server.log()
                       or server.process.poll() is not None, 'accept or real fd exhaustion')
            require(server.process.poll() is None, 'Real fd exhaustion killed server')
            if server.count(ACCEPT) >= expected:
                held.append(client)
                # The next accept can hit EMFILE even before another client is queued.
                if PAUSE in server.log():
                    pending = server.connect()
                    pending.sendall(PREFIX)
                    break
            else:
                pending = client
                break
        require(pending is not None and 0 < len(held) < 1024, 'No real EMFILE boundary')
        require(f'errno={errno.EMFILE}' in server.log(), 'Wrong real exhaustion errno')
        require(server.fds() == 64, 'Child descriptor limit was not actually reached')
        ticks_per_second = os.sysconf('SC_CLK_TCK')

        def cpu_ticks():
            # Fields after comm start at field 3; utime/stime are fields 14 and 15.
            fields = Path(f'/proc/{server.process.pid}/stat').read_text().rsplit(')', 1)[1].split()
            return int(fields[11]) + int(fields[12])

        before_cpu = cpu_ticks()
        before_wait = time.monotonic()
        # Leave all descriptors in use through another real retry.
        server.wait_log(PAUSE, 2)
        retry_elapsed = time.monotonic()-before_wait
        cpu_seconds = (cpu_ticks()-before_cpu)/ticks_per_second
        require(server.process.poll() is None, 'Server failed on repeated real EMFILE')
        accepted_before_release = len(held)
        finish_held(held.pop(0), started)
        wait_until(lambda: server.fds() < 64, 'fd released by existing client')
        pending.sendall(SUFFIX)
        require(parse_response(read_all(pending), started) ==
                expected_response(body=b'Hello,HTTP!\n'), 'Real EMFILE recovery response')
        pending.close()
        server.wait_log(RESUME, 2)
        for client in held:
            client.close()
        healthy_stop(server, baseline)
        results.append(dict(name='real-nofile-64', injected=False, child_nofile_limit=64,
                            build='Release with assertions; cold HTTP formatting, no UBSan runtime',
                            accepted_before_release=accepted_before_release,
                            existing_response_at_limit=200, pending_response_after_release=200,
                            retry_observation_seconds=round(retry_elapsed, 6),
                            cpu_seconds_during_full_retry=cpu_seconds,
                            fd_count_restored=True, exit_code=0))

    require(resource.getrlimit(resource.RLIMIT_NOFILE) == parent_limit, 'Runner limits changed')
    require(len(results) == 18, 'An accept recovery scenario was skipped')
    return dict(passed_cases=len(results), cases=results, parent_limits_unchanged=True,
                scope='Production main, temporary loopback ports and real responses. Nine network errors and resource/epoll faults explicitly injected under UBSan; one separate assertions/Release child really exhausts RLIMIT_NOFILE=64 without a preceding HTTP warmup. All other process limits unchanged.')
