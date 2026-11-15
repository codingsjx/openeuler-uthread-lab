"""Functional TCP checks only; no benchmark results or performance thresholds."""
import contextlib
import json
from pathlib import Path
import selectors
import socket
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


@contextlib.contextmanager
def server(limit, workers=3, trace=None):
    command = [str(ROOT/'build/tcp_echo'), '0', str(limit), str(workers), '500', '3000']
    if trace is not None:
        command.append(str(trace))
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
        return json.loads(output.strip())

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


if __name__ == '__main__':
    unittest.main()
