"""Real application control flow with a CPU-only test backend and fake daemons."""
import mmap
import fcntl
import os
from pathlib import Path
import shutil
import socket
import struct
import subprocess
import tempfile
import threading
import time
import unittest
import uuid

ROOT = Path(__file__).resolve().parent
HALF = 4 << 20
CONTROL = 4096
WORD = struct.Struct('<Q')


class MailboxApplicationTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not shutil.which('c++') or not shutil.which('cc'):
            raise unittest.SkipTest('offline test needs C/C++ compilers')
        cls.build = tempfile.TemporaryDirectory(prefix='gpu-mailbox-test-')
        cls.binary = Path(cls.build.name) / 'mailbox-app'
        cls.daemon = Path(cls.build.name) / 'mailbox-daemon'
        obj = Path(cls.build.name) / 'helper.o'
        subprocess.run(['cc', '-std=c11', '-O2', '-c', str(ROOT / '../../rpc/libmcdma_rpc.c'),
                        '-o', str(obj)], check=True, capture_output=True, text=True)
        subprocess.run(['c++', '-std=c++17', '-O2', '-Wall', '-Wextra', '-Werror',
                        '-I' + str(ROOT / '../../rpc'), str(ROOT / 'mailbox_test_backend.cpp'),
                        str(obj), '-o', str(cls.binary)], check=True, capture_output=True, text=True)
        subprocess.run(['c++', '-std=c++17', '-O2', '-Wall', '-Wextra', '-Werror',
                        str(ROOT / 'mailbox_test_daemon.cpp'), '-o', str(cls.daemon)],
                       check=True, capture_output=True, text=True)

    @classmethod
    def tearDownClass(cls):
        cls.build.cleanup()

    def exercise(self, fault=None):
        with tempfile.TemporaryDirectory(prefix='mbx-', dir='/tmp') as temporary:
            directory = Path(temporary)
            link = 'test-' + uuid.uuid4().hex[:12]
            lock = Path('/tmp/mcdma-llama.' + link + '.client.lock')
            maps, processes, logs = [], [], []
            control = None
            claimed_lock = None
            listener = socket.socket(socket.AF_UNIX)
            sock = directory / 'service.sock'
            listener.bind(str(sock)); listener.listen(1); listener.settimeout(3)
            stop = threading.Event()
            failures = []
            for name in ('client', 'service'):
                path = directory / name
                with path.open('wb') as file:
                    file.truncate(2 * HALF)
                with path.open('r+b') as file:
                    mapping = mmap.mmap(file.fileno(), 2 * HALF)
                WORD.pack_into(mapping, 256, HALF); WORD.pack_into(mapping, 264, HALF)
                maps.append(mapping)
            client, service = maps
            WORD.pack_into(client, 64, 1); WORD.pack_into(client, 72, 1)

            def relay():
                last_request = last_reply = 0
                try:
                    while not stop.is_set():
                        request, = WORD.unpack_from(client, 0)
                        sequence, length = request >> 32, request & 0xffffffff
                        if sequence and sequence != last_request:
                            if fault == 'generation' and sequence == 2:
                                WORD.pack_into(client, 72, 2)
                                return
                            if fault == 'service-loss' and sequence == 2:
                                control.shutdown(socket.SHUT_RDWR)
                                return
                            service[CONTROL:CONTROL + length] = client[CONTROL:CONTROL + length]
                            if fault == 'corruption' and sequence == 2:
                                service[CONTROL] ^= 1
                            WORD.pack_into(service, 0, request)
                            last_request = sequence
                        reply, = WORD.unpack_from(service, HALF + 128)
                        sequence, length = reply >> 32, reply & 0xffffffff
                        if sequence and sequence != last_reply:
                            # Mirrors the daemon's ready-before-send path.
                            WORD.pack_into(service, HALF, reply)
                            client[HALF + CONTROL:HALF + CONTROL + length] = service[HALF + CONTROL:HALF + CONTROL + length]
                            WORD.pack_into(client, HALF + 64, reply)
                            last_reply = sequence
                            if fault == 'final-disconnect' and sequence == 13:
                                control.shutdown(socket.SHUT_RDWR)
                                return
                        time.sleep(.0001)
                except BaseException as error:
                    failures.append(error)

            if fault == 'busy-client':
                claimed_lock = lock.open('a+b')
                fcntl.flock(claimed_lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            worker = threading.Thread(target=relay, daemon=True)
            try:
                for role in ('service', 'client'):
                    log = (directory / (role + '.log')).open('w+')
                    logs.append(log)
                    command = [str(self.binary), role, '--link', link, '--mailbox', str(directory / role),
                               '--socket', str(sock), '--timeout', '1']
                    processes.append(subprocess.Popen(command, stdout=log, stderr=log))
                    if role == 'service':
                        control, _ = listener.accept(); control.settimeout(2)
                        self.assertEqual(control.recv(64), b'MODE poll\n')
                        control.sendall(b'OK\n')
                        deadline = time.monotonic() + 3
                        while True:
                            log.seek(0)
                            if 'GPU_MAILBOX_READY' in log.read():
                                break
                            self.assertIsNone(processes[0].poll(), 'service exited before ready')
                            self.assertLess(time.monotonic(), deadline, 'service did not become ready')
                            time.sleep(.005)
                        worker.start()
                codes = [process.wait(timeout=4) for process in processes]
                texts = []
                for log in logs:
                    log.seek(0); texts.append(log.read())
                self.assertEqual(failures, [])
                if fault in (None, 'final-disconnect'):
                    self.assertEqual(codes, [0, 0], texts)
                    for text in texts:
                        self.assertEqual(text.count('GPU_MAILBOX_CALL '), 12)
                        self.assertIn('final_control_ack=1 cleanup=ok', text)
                else:
                    self.assertNotEqual(codes, [0, 0], texts)
                    self.assertNotIn('GPU_MAILBOX_COMPLETE', texts[0] + texts[1])
                return texts
            finally:
                stop.set()
                if worker.is_alive(): worker.join(2)
                for process in processes:
                    if process.poll() is None:
                        process.kill(); process.wait()
                if control is not None: control.close()
                listener.close()
                for mapping in maps: mapping.close()
                for log in logs: log.close()
                if claimed_lock is not None: claimed_lock.close()
                lock.unlink(missing_ok=True)

    def test_twelve_payload_calls_and_final_ack_preserve_guards(self):
        self.exercise()

    def test_corruption_fails_without_successful_cleanup_claim(self):
        self.assertIn('verification failed', ''.join(self.exercise('corruption')))

    def test_generation_change_fails_without_resuming(self):
        self.assertIn('generation changed', ''.join(self.exercise('generation')))

    def test_final_ready_word_survives_immediate_peer_disconnect(self):
        self.exercise('final-disconnect')

    def owned(self, mode='normal', crash_parent=False):
        with tempfile.TemporaryDirectory(prefix='mbx-owned-', dir='/tmp') as temporary:
            directory = Path(temporary)
            link = 'test-' + uuid.uuid4().hex[:12]
            log_path = directory / 'log'
            socket_path = str(directory / 'daemon.sock')
            env = dict(os.environ, MCDMA_TEST_DAEMON_MODE=mode,
                       MCDMA_TEST_EXPECT_SOCKET=socket_path, MCDMA_RPCD_SOCKET='replaced-by-owned-client')
            command = [str(self.binary), 'client', '--link', link, '--daemon', str(self.daemon),
                       '--connect', link + ',example.invalid,1,rdma0,0,1024,4,4',
                       '--socket', socket_path, '--timeout', '1']
            with log_path.open('w') as log:
                process = subprocess.Popen(command, stdout=log, stderr=log, env=env)
                try:
                    if crash_parent:
                        deadline = time.monotonic() + 3
                        while 'OFFLINE_DAEMON_STARTED' not in log_path.read_text():
                            self.assertIsNone(process.poll())
                            self.assertLess(time.monotonic(), deadline)
                            time.sleep(.005)
                        process.kill()  # Simulated GPU application crash, never the daemon.
                    code = process.wait(timeout=4)
                    deadline = time.monotonic() + 3
                    while 'OFFLINE_DAEMON_CLEAN_EXIT' not in log_path.read_text():
                        self.assertLess(time.monotonic(), deadline, log_path.read_text())
                        time.sleep(.005)
                    text = log_path.read_text()
                    if mode == 'normal' and not crash_parent:
                        self.assertEqual(code, 0, text)
                        self.assertIn('GPU_MAILBOX_DAEMON_EXIT clean=1', text)
                        self.assertIn('OFFLINE_OWNED_RELEASE after_child_cleanup=1', text)
                        self.assertLess(text.index('OFFLINE_DAEMON_CLEAN_EXIT'), text.index('OFFLINE_OWNED_RELEASE'))
                    else:
                        self.assertNotEqual(code, 0, text)
                        self.assertNotIn('OFFLINE_OWNED_RELEASE', text)
                        self.assertNotIn('GPU_MAILBOX_COMPLETE', text)
                    return text
                finally:
                    if process.poll() is None:
                        process.kill(); process.wait()
                    Path('/tmp/mcdma-llama.' + link + '.client.lock').unlink(missing_ok=True)

    def test_owned_child_exits_before_parent_storage_release(self):
        self.owned()

    def test_unclean_child_exit_preserves_gpu_storage_until_process_exit(self):
        self.assertIn('exit_status=7', self.owned('nonzero-exit'))

    def test_owned_link_readiness_timeout_stops_child_before_returning(self):
        started = time.monotonic()
        text = self.owned('never-up')
        self.assertIn('link readiness timed out', text)
        self.assertIn('GPU_MAILBOX_DAEMON_EXIT clean=1', text)
        self.assertLess(time.monotonic() - started, 3)

    def test_parent_crash_closes_lease_without_daemon_kill(self):
        self.owned('never-up', crash_parent=True)

    def test_existing_client_owner_is_not_displaced(self):
        self.assertIn('exclusive client lock', ''.join(self.exercise('busy-client')))

    def test_service_loss_fails_both_endpoints(self):
        self.assertIn('ended service registration', ''.join(self.exercise('service-loss')))


if __name__ == '__main__':
    unittest.main()
