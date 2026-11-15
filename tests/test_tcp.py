"""Functional TCP checks only; no benchmark results or performance thresholds."""
import contextlib
import json
from pathlib import Path
import selectors
import socket
import subprocess
import tempfile
import unittest
import time

ROOT = Path(__file__).resolve().parents[1]


@contextlib.contextmanager
def server(limit, workers=3, trace=None, idle_ms=500, lifetime_ms=3000, admission='reject', stall_ms=200):
    command = [str(ROOT/'build/tcp_echo'), '0', str(limit), str(workers), str(idle_ms), str(lifetime_ms)]
    if trace is not None:
        command.append(str(trace))
    command += ['--admission='+admission, '--stall-ms='+str(stall_ms)]
    process = subprocess.Popen(command, cwd=ROOT, stdout=subprocess.PIPE,
                               stderr=subprocess.PIPE, text=True)
    try:
        with selectors.DefaultSelector() as selector:
            selector.register(process.stdout, selectors.EVENT_READ)
            if not selector.select(10):
                raise RuntimeError('Server did not start')
        ready = json.loads(process.stdout.readline())
        assert ready['event'] == 'listening'
        yield process, ('127.0.0.1', ready['port'])
    finally:
        if process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
        process.stdout.close()
        process.stderr.close()


def echo(address, payload):
    with socket.create_connection(address, timeout=5) as connection:
        connection.sendall(payload[:3])
        connection.sendall(payload[3:])
        connection.shutdown(socket.SHUT_WR)
        chunks = []
        while True:
            block = connection.recv(4096)
            if not block:
                return b''.join(chunks)
            chunks.append(block)


class TCPTests(unittest.TestCase):
    def finish(self, process):
        output, errors = process.communicate(timeout=10)
        self.assertEqual(process.returncode, 0, errors)
        stopped = json.loads(output.strip())
        self.assertEqual(stopped['accepted'], stopped['admitted'] + stopped['rejected'])
        self.assertEqual(stopped['admitted'], sum(stopped[key] for key in
                         ('completed', 'timeouts', 'peer_errors', 'shutdown_closed', 'reclaimed')))
        self.assertEqual(stopped['active_remaining'], 0)
        return stopped

    def test_capacity_rejects_then_recovers(self):
        with server(3, workers=1, idle_ms=5000, lifetime_ms=20000) as (process, address):
            with socket.create_connection(address, timeout=5) as held:
                held.sendall(b'H')
                self.assertEqual(held.recv(1), b'H')  # confirms the slot is active
                with socket.create_connection(address, timeout=5) as overflow:
                    try:
                        closed = overflow.recv(1)
                    except ConnectionResetError:
                        closed = b''
                    self.assertEqual(closed, b'')
                held.shutdown(socket.SHUT_WR)
                self.assertEqual(held.recv(1), b'')  # server has released its slot
            self.assertEqual(echo(address, b'reused'), b'reused')
            stopped = self.finish(process)
            self.assertEqual(stopped['admitted'], 2)
            self.assertEqual(stopped['capacity_rejected'], 1)
            self.assertEqual(stopped['peak_active'], 1)

    def test_sequential_slot_reuse_and_recent_trace(self):
        with tempfile.TemporaryDirectory() as temporary:
            trace = Path(temporary)/'recent.csv'
            with server(64, workers=1, trace=trace) as (process, address):
                for index in range(64):
                    payload = ('request-'+str(index)).encode()
                    self.assertEqual(echo(address, payload), payload)
                stopped = self.finish(process)
                self.assertEqual(stopped['admitted'], 64)
                self.assertEqual(stopped['rejected'], 0)
                self.assertEqual(stopped['completed'], 64)
                self.assertEqual(stopped['peak_active'], 1)
            self.assertIn('overwritten', trace.read_text())

    def test_fragmented_binary_echo_and_idle_cleanup(self):
        with server(3) as (process, address), socket.create_connection(address, timeout=5) as idle:
            self.assertEqual(echo(address, b'hello'), b'hello')
            payload = bytes(range(256)) * 17
            self.assertEqual(echo(address, payload), payload)
            self.assertEqual(idle.recv(1), b'')
            stopped = self.finish(process)
            self.assertEqual(stopped['accepted'], 3)
            self.assertEqual(stopped['completed'], 2)
            self.assertEqual(stopped['timeouts'], 1)
            self.assertEqual(stopped['peer_errors'], 0)
            self.assertLessEqual(stopped['peak_active'], 3)

    def test_slow_receiver_waits_without_losing_fast_echo(self):
        with tempfile.TemporaryDirectory() as temporary:
            trace = Path(temporary)/'trace.csv'
            with server(2, 2, trace) as (process, address), socket.socket() as slow:
                slow.settimeout(5)
                slow.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1024)
                slow.connect(address)
                slow.sendall(b'X' * (512 * 1024))
                self.assertEqual(echo(address, b'fast'), b'fast')
                stopped = self.finish(process)
                self.assertEqual(stopped['accepted'], 2)
                self.assertEqual(stopped['completed'], 1)
                self.assertEqual(stopped['timeouts'], 1)
                self.assertLessEqual(stopped['peak_active'], 2)
            self.assertIn('write_eagain', trace.read_text())

    def test_shutdown_while_accept_waits(self):
        with server(0) as (process, _):
            process.terminate()
            self.assertEqual(self.finish(process)['accepted'], 0)

    def test_reclaim_silent_peer_for_ready_request(self):
        with server(2, 1, idle_ms=5000, lifetime_ms=20000,
                    admission='reclaim-stalled', stall_ms=1) as (process, address):
            with socket.create_connection(address, timeout=5) as silent:
                time.sleep(0.1)  # create a deliberately stale, never-started session
                self.assertEqual(echo(address, b'replacement'), b'replacement')
                self.assertEqual(silent.recv(1), b'')
            stopped = self.finish(process)
            self.assertEqual(stopped['reclaimed'], 1)
            self.assertEqual(stopped['completed'], 1)
            self.assertEqual(stopped['rejected'], 0)

    def test_reclaim_preserves_established_idle_session(self):
        with server(2, 1, idle_ms=5000, lifetime_ms=20000,
                    admission='reclaim-stalled', stall_ms=1) as (process, address):
            with socket.create_connection(address, timeout=5) as held:
                held.sendall(b'H')
                self.assertEqual(held.recv(1), b'H')
                time.sleep(0.02)
                with socket.create_connection(address, timeout=5) as newcomer:
                    try:
                        newcomer.sendall(b'N')
                        self.assertEqual(newcomer.recv(1), b'')
                    except ConnectionResetError:
                        pass
                held.sendall(b'R')
                self.assertEqual(held.recv(1), b'R')
                held.shutdown(socket.SHUT_WR)
                self.assertEqual(held.recv(1), b'')
            stopped = self.finish(process)
            self.assertEqual(stopped['reclaimed'], 0)
            self.assertEqual(stopped['capacity_rejected'], 1)

    def test_reclaim_backpressured_writer(self):
        with server(2, 1, idle_ms=5000, lifetime_ms=20000,
                    admission='reclaim-stalled', stall_ms=10) as (process, address), socket.socket() as slow:
            slow.settimeout(5)
            slow.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1024)
            slow.connect(address)
            slow.sendall(b'X' * (512 * 1024))
            time.sleep(0.1)  # intentionally do not drain the receive window
            self.assertEqual(echo(address, b'fast'), b'fast')
            stopped = self.finish(process)
            self.assertEqual(stopped['reclaimed'], 1)
            self.assertEqual(stopped['completed'], 1)

    def test_shutdown_cancels_long_wait(self):
        with server(0, 1, idle_ms=60000, lifetime_ms=120000) as (process, address):
            with socket.create_connection(address, timeout=5) as held:
                held.sendall(b'H')
                self.assertEqual(held.recv(1), b'H')
                process.terminate()
                stopped = self.finish(process)
            self.assertEqual(stopped['shutdown_closed'], 1)
            self.assertEqual(stopped['timeouts'], 0)


if __name__ == '__main__':
    unittest.main()
