"""Protocol, admission and ownership regression checks; no performance claims."""
import contextlib
import json
import selectors
import socket
import struct
import subprocess
import threading
import time
import unittest
import os
import shutil
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def keep_trace(path, policy):
    destination = os.environ.get('TRACE_TEST_ARTIFACT_DIR')
    if destination:
        folder = Path(destination)
        folder.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(path, folder/('request-'+policy+'.csv'))


@contextlib.contextmanager
def server(**options):
    configuration = {'port': 0, 'limit': 1, 'capacity': 2, 'probes': 2,
                     'header-ms': 3000, 'body-ms': 3000, 'response-ms': 3000,
                     'idle-ms': 3000, 'lifetime-ms': 20000, 'stall-ms': 10}
    configuration.update({key.replace('_', '-'): value for key, value in options.items()})
    command = [str(ROOT/'build/request_server'), *['--'+k+'='+str(v) for k, v in configuration.items()]]
    process = subprocess.Popen(command, cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    try:
        with selectors.DefaultSelector() as selector:
            selector.register(process.stdout, selectors.EVENT_READ)
            if not selector.select(10):
                raise RuntimeError('Request server did not start')
        line = process.stdout.readline()
        if not line:
            raise RuntimeError(process.stderr.read())
        ready = json.loads(line)
        yield process, ('127.0.0.1', ready['port']), ready
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


def exact(connection, size):
    data = b''
    while len(data) < size:
        chunk = connection.recv(size - len(data))
        if not chunk:
            raise EOFError('Incomplete response')
        data += chunk
    return data


def request(connection, payload):
    connection.sendall(struct.pack('!I', len(payload)) + payload)
    size, = struct.unpack('!I', exact(connection, 4))
    if size != len(payload):
        raise AssertionError('Response length mismatch')
    return exact(connection, size)


def closed(connection):
    try:
        return connection.recv(1) == b''
    except ConnectionResetError:
        return True


def finish_client(connection):
    connection.shutdown(socket.SHUT_WR)
    if not closed(connection):
        raise AssertionError('Unexpected bytes after response')


class RequestTests(unittest.TestCase):
    def finish(self, process, ready):
        output, errors = process.communicate(timeout=10)
        self.assertEqual(process.returncode, 0, errors)
        result = json.loads(output.strip())
        self.assertEqual(result['accepted'], result['opened'] + result['direct_rejected'])
        self.assertEqual(result['opened'], result['closed'])
        self.assertEqual(result['closed'], sum(result[key] for key in
                         ('clean_closed', 'timeout_closed', 'protocol_closed', 'error_closed',
                          'reclaimed', 'shutdown_closed', 'probe_rejected')))
        for key in ('workers_remaining', 'probes_remaining', 'reservations_remaining', 'owned_fds_remaining'):
            self.assertEqual(result[key], 0, key)
        self.assertLessEqual(result['peak_workers'], ready['capacity'])
        self.assertLessEqual(result['peak_probes'], ready['probes'])
        self.assertLessEqual(result['peak_tasks'], ready['task_budget'])
        self.assertLessEqual(result['peak_fds'], ready['fd_budget'])
        return result

    def test_binary_frames_and_persistent_requests(self):
        with server() as (process, address, ready), socket.create_connection(address, timeout=5) as connection:
            for payload in (b'hello', bytes(range(256))*16):
                self.assertEqual(request(connection, payload), bytes(x ^ 0x5a for x in payload))
            finish_client(connection)
            result = self.finish(process, ready)
            self.assertEqual(result['requests_completed'], 2)
            self.assertEqual(result['clean_closed'], 1)

    def test_invalid_length_and_truncated_body(self):
        for wire in (struct.pack('!I', 4097), struct.pack('!I', 0), struct.pack('!I', 5)+b'x'):
            with self.subTest(wire=wire), server() as (process, address, ready):
                with socket.create_connection(address, timeout=5) as connection:
                    connection.sendall(wire)
                    connection.shutdown(socket.SHUT_WR)
                    self.assertTrue(closed(connection))
                result = self.finish(process, ready)
                self.assertEqual(result['protocol_closed'], 1)
                self.assertEqual(result['requests_completed'], 0)

    def test_body_deadline_is_not_refreshed_by_trickle(self):
        with server(body_ms=250, lifetime_ms=3000) as (process, address, ready):
            with socket.create_connection(address, timeout=5) as connection:
                connection.sendall(struct.pack('!I', 4096))
                stop = threading.Event()
                def trickle():
                    while not stop.is_set():
                        try:
                            connection.sendall(b'x')
                        except OSError:
                            return
                        stop.wait(0.05)
                thread = threading.Thread(target=trickle, daemon=True)
                thread.start()
                try:
                    self.assertTrue(closed(connection))
                finally:
                    stop.set()
                    thread.join(timeout=5)
            result = self.finish(process, ready)
            self.assertEqual(result['body_timeouts'], 1)
            self.assertEqual(result['lifetime_timeouts'], 0)

    def test_partial_header_three_policies(self):
        for admission in ('reject', 'byte-stalled', 'phase-aware'):
            with self.subTest(admission=admission), tempfile.TemporaryDirectory() as temporary:
                trace = Path(temporary)/'request.csv'
                with server(limit=2, capacity=1, admission=admission, trace=trace) as (process, address, ready):
                    with socket.create_connection(address, timeout=5) as partial:
                        partial.sendall(b'\0')
                        time.sleep(0.08)
                        with socket.create_connection(address, timeout=5) as newcomer:
                            if admission == 'phase-aware':
                                self.assertEqual(request(newcomer, b'valid'), bytes(x ^ 0x5a for x in b'valid'))
                                finish_client(newcomer)
                                self.assertTrue(closed(partial))
                            else:
                                try:
                                    newcomer.sendall(struct.pack('!I', 5)+b'valid')
                                except (ConnectionResetError, BrokenPipeError):
                                    pass
                                self.assertTrue(closed(newcomer))
                                partial.shutdown(socket.SHUT_WR)
                                self.assertTrue(closed(partial))
                    result = self.finish(process, ready)
                    self.assertEqual(result['reclaimed'], int(admission == 'phase-aware'))
                    self.assertEqual(result['requests_completed'], int(admission == 'phase-aware'))
                keep_trace(trace, admission)

    def test_silent_probe_does_not_hold_up_ready_probe(self):
        with server(limit=3, capacity=1, probes=2, header_ms=2000) as (process, address, ready):
            with socket.create_connection(address, timeout=5) as partial:
                partial.sendall(b'\0'); time.sleep(0.08)
                with socket.create_connection(address, timeout=5) as silent:
                    with socket.create_connection(address, timeout=5) as fast:
                        self.assertEqual(request(fast, b'A'), b'\x1b')
                        finish_client(fast)
                    # The earlier silent probe remains usable after the later
                    # request completes: the acceptor did not wait on that probe.
                    self.assertEqual(request(silent, b'B'), b'\x18')
                    finish_client(silent)
                self.assertTrue(closed(partial))
            result = self.finish(process, ready)
            self.assertEqual(result['requests_completed'], 2)
            self.assertEqual(result['promoted'], 2)
            self.assertEqual(result['timeout_closed'], 0)

    def test_established_idle_session_is_not_reclaimed(self):
        with server(limit=2, capacity=1) as (process, address, ready):
            with socket.create_connection(address, timeout=5) as established:
                self.assertEqual(request(established, b'H'), b'\x12')
                time.sleep(0.04)
                with socket.create_connection(address, timeout=5) as newcomer:
                    newcomer.sendall(struct.pack('!I', 1)+b'N')
                    self.assertTrue(closed(newcomer))
                self.assertEqual(request(established, b'R'), b'\x08')
                finish_client(established)
            result = self.finish(process, ready)
            self.assertEqual(result['reclaimed'], 0)
            self.assertEqual(result['probe_rejected'], 1)
            self.assertEqual(result['requests_completed'], 2)

    def test_malformed_probe_cannot_evict_partial_request(self):
        with server(limit=2, capacity=1) as (process, address, ready):
            with socket.create_connection(address, timeout=5) as partial:
                partial.sendall(b'\0'); time.sleep(0.04)
                with socket.create_connection(address, timeout=5) as malformed:
                    malformed.sendall(struct.pack('!I', 4097))
                    self.assertTrue(closed(malformed))
                partial.sendall(b'\0\0\x01H')
                self.assertEqual(exact(partial, 5), struct.pack('!I', 1)+b'\x12')
                finish_client(partial)
            result = self.finish(process, ready)
            self.assertEqual(result['reclaimed'], 0)
            self.assertEqual(result['protocol_closed'], 1)
            self.assertEqual(result['requests_completed'], 1)

    def test_concurrent_handoffs_preserve_capacity_reservations(self):
        with server(limit=4, capacity=2, probes=2) as (process, address, ready):
            with socket.create_connection(address, timeout=5) as first, socket.create_connection(address, timeout=5) as second:
                first.sendall(b'\0'); second.sendall(b'\0'); time.sleep(0.08)
                errors = []
                barrier = threading.Barrier(2, timeout=5)
                def replace():
                    try:
                        with socket.create_connection(address, timeout=5) as connection:
                            if request(connection, b'X') != b'\x02':
                                raise AssertionError('Bad replacement response')
                            barrier.wait()  # keep the first replacement occupying its slot
                            finish_client(connection)
                    except Exception as error:
                        errors.append(error)
                        barrier.abort()
                threads = [threading.Thread(target=replace) for _ in range(2)]
                for thread in threads: thread.start()
                for thread in threads: thread.join(timeout=10)
                self.assertFalse(any(thread.is_alive() for thread in threads))
                self.assertFalse(errors, errors)
                self.assertTrue(closed(first) and closed(second))
            result = self.finish(process, ready)
            self.assertEqual(result['reclaimed'], 2)
            self.assertEqual(result['requests_completed'], 2)

    def test_probe_budget_rejects_excess_without_reclaim(self):
        with server(limit=4, capacity=1, probes=1) as (process, address, ready):
            with socket.create_connection(address, timeout=5) as partial:
                partial.sendall(b'\0'); time.sleep(0.03)
                with socket.create_connection(address, timeout=5) as silent:
                    time.sleep(0.03)
                    for _ in range(2):
                        with socket.create_connection(address, timeout=5) as excess:
                            self.assertTrue(closed(excess))
                    silent.shutdown(socket.SHUT_WR); self.assertTrue(closed(silent))
                partial.shutdown(socket.SHUT_WR); self.assertTrue(closed(partial))
            result = self.finish(process, ready)
            self.assertEqual(result['direct_rejected'], 2)
            self.assertEqual(result['reclaimed'], 0)

    def test_single_initialization_fd_and_slot_reuse(self):
        with server(limit=65) as (process, address, ready):
            descriptor_directory = Path('/proc')/str(process.pid)/'fd'
            baseline = len(list(descriptor_directory.iterdir()))
            for index in range(64):
                with socket.create_connection(address, timeout=5) as connection:
                    self.assertEqual(request(connection, b'X'), b'\x02')
                    finish_client(connection)
                if index % 16 == 15:
                    self.assertEqual(len(list(descriptor_directory.iterdir())), baseline)
            with socket.create_connection(address, timeout=5) as connection:
                self.assertEqual(request(connection, b'Y'), b'\x03'); finish_client(connection)
            result = self.finish(process, ready)
            self.assertEqual(result['requests_completed'], 65)

    def test_stop_cleans_workers_and_probes(self):
        with server(limit=0, capacity=1, header_ms=60000, lifetime_ms=120000) as (process, address, ready):
            with socket.create_connection(address, timeout=5) as first, socket.create_connection(address, timeout=5) as second:
                first.sendall(b'\0'); second.sendall(b'\0'); time.sleep(0.05)
                process.terminate()
                result = self.finish(process, ready)
                self.assertEqual(result['timeout_closed'], 0)
                self.assertEqual(result['shutdown_closed'], result['opened'])

    def test_task_budget_configuration_is_checked(self):
        result = subprocess.run([str(ROOT/'build/request_server'), '--capacity=31', '--probes=1'], capture_output=True, text=True, timeout=5)
        self.assertEqual(result.returncode, 2)
        self.assertIn('<= 31', result.stderr)


if __name__ == '__main__':
    unittest.main()
