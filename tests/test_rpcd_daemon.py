"""Exercise mcdma-rpcd's failure paths offline: the real daemon sources built against a stub verbs library."""
import mmap
import ctypes
import fcntl
import os
import pathlib
import shutil
import signal
import select
import socket
import struct
import subprocess
import tempfile
import threading
import time
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
MIB = 1 << 20
SOURCES = ['rpc/rpcd_common.c', 'rpc/rpcd_verbs.c', 'rpc/rpcd_listen.c', 'rpc/rpcd_connect.c']


@unittest.skipUnless(shutil.which('cc'), 'C compiler required')
class DaemonTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.work = tempfile.mkdtemp(prefix='rpcd-', dir='/tmp')
        flags = ['cc', '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                 f'-DMCDMA_RPC_BOX_DIR="{cls.work}"', f'-DMCDMA_RPC_LOCK_DIR="{cls.work}"',
                 '-DMCDMA_RPC_HANDSHAKE_S=1', '-DMCDMA_RPC_CONTROL_SESSION_S=1']
        stub = ['tests/rpcd_stub_verbs.c']
        cls.daemon = os.path.join(cls.work, 'rpcd-stub')
        cls.harness = os.path.join(cls.work, 'connect-harness')
        cls.dmabuf_daemon = os.path.join(cls.work, 'rpcd-dmabuf-stub')
        cls.dmabuf_harness = os.path.join(cls.work, 'connect-dmabuf-harness')
        subprocess.run([*flags, 'rpc/mcdma-rpcd.c', *SOURCES, *stub, '-o', cls.daemon], cwd=ROOT, check=True)
        subprocess.run([*flags, 'tests/test_rpcd_connect.c', *SOURCES, *stub, '-o', cls.harness], cwd=ROOT,
                       check=True)
        for source, output in [('rpc/mcdma-rpcd.c', cls.dmabuf_daemon),
                               ('tests/test_rpcd_connect.c', cls.dmabuf_harness)]:
            subprocess.run([*flags, '-DMCDMA_RPC_TEST_DMABUF', source, *SOURCES, *stub, '-o', output],
                           cwd=ROOT, check=True)

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.work, ignore_errors=True)

    def connect_scenario(self, mode):
        done = subprocess.run([self.harness, mode], capture_output=True, text=True, timeout=30)
        self.assertEqual(done.returncode, 0, done.stderr[-2000:])

    def test_a_request_staged_the_moment_the_link_is_up_is_served(self):
        self.connect_scenario('race')

    def test_shutdown_completes_when_the_peer_never_answers_hello(self):
        self.connect_scenario('hang')

    def test_an_oversized_pull_reply_drops_the_link_instead_of_crashing(self):
        self.connect_scenario('pullcrash')

    def test_sigterm_during_a_call_stops_the_daemon_the_orderly_way(self):
        self.connect_scenario('sigterm')

    def test_dmabuf_uses_same_protocol_and_never_initializes_payload_or_falls_back(self):
        done = subprocess.run([self.dmabuf_harness, 'dmabuf'], capture_output=True, text=True, timeout=30,
                              env=dict(os.environ, STUB_FORBID_HOST_REG='1'))
        self.assertEqual(done.returncode, 0, done.stderr[-2000:])
        self.assertIn('STUB_DMABUF_REG offset=0 length=4194304', done.stderr)
        self.assertIn('STUB_DMABUF_REG offset=4194304 length=4194304', done.stderr)
        self.assertEqual(done.stderr.count('STUB_DMABUF_DEREG fd_alive=1 mapping_alive=1'), 2)

    def test_verbs_cleanup_errors_retain_handles_and_fail_closed(self):
        for fault in ('QP_ERROR', 'DESTROY_QP', 'DESTROY_CQ', 'DEREG_MR', 'DEALLOC_PD', 'CLOSE_DEVICE'):
            with self.subTest(fault=fault):
                done = subprocess.run([self.dmabuf_harness, 'dmabuf'], capture_output=True, text=True, timeout=30,
                                      env=dict(os.environ, STUB_FORBID_HOST_REG='1', **{'STUB_FAIL_' + fault: '1'}))
                self.assertEqual(done.returncode, 2, done.stderr[-2000:])
                self.assertIn('verbs cleanup incomplete', done.stderr)
                self.assertNotIn('STUB-VIOLATION', done.stderr)
                self.assertNotIn('every verbs object destroyed', done.stderr)

    def fd_arguments(self, fd, name, sock, parent=None):
        args = [self.dmabuf_daemon, 'connect', '--buffer-fd', str(fd)]
        if parent is not None:
            args += ['--parent-fd', str(parent)]
        args += [f'{name},127.0.0.1,{free_port()},stub0,0,4096,4,4']
        env = dict(os.environ, MCDMA_RPCD_SOCKET=sock, STUB_FORBID_HOST_REG='1')
        return args, env

    def start_fd(self, name, *, parent=None, extra_env=None):
        backing = tempfile.TemporaryFile(dir=self.work)
        self.addCleanup(backing.close)
        backing.truncate(8 * MIB)
        sock = os.path.join(self.work, name + '.sock')
        args, env = self.fd_arguments(backing.fileno(), name, sock, parent)
        env.update(extra_env or {})
        inherited = (backing.fileno(),) + (() if parent is None else (parent,))
        daemon = subprocess.Popen(args, stderr=subprocess.PIPE, text=True, env=env, pass_fds=inherited)
        self.addCleanup(stop, daemon, sock)
        deadline = time.monotonic() + 5
        while command(sock, 'STATUS') is None:
            self.assertIsNone(daemon.poll(), 'FD-mode daemon exited before readiness')
            self.assertLess(time.monotonic(), deadline, 'FD-mode daemon never answered')
            time.sleep(.02)
        return daemon, sock, backing

    def test_parent_lease_eof_stops_daemon_with_orderly_cleanup(self):
        read_fd, write_fd = os.pipe()
        try:
            daemon, _, _ = self.start_fd('lease', parent=read_fd)
            os.close(read_fd); read_fd = -1
            os.close(write_fd); write_fd = -1
            self.assertEqual(daemon.wait(timeout=10), 0)
            diagnostic = daemon.stderr.read()
            self.assertIn('parent lease closed; shutting down', diagnostic)
            self.assertIn('every verbs object destroyed', diagnostic)
            self.assertEqual(diagnostic.count('STUB_DMABUF_DEREG fd_alive=1 mapping_alive=1'), 2)
        finally:
            if read_fd >= 0: os.close(read_fd)
            if write_fd >= 0: os.close(write_fd)

    def test_parent_lease_unexpected_data_fails_after_cleanup(self):
        read_fd, write_fd = os.pipe()
        try:
            original_flags = fcntl.fcntl(read_fd, fcntl.F_GETFL)
            daemon, _, _ = self.start_fd('leasedata', parent=read_fd)
            os.write(write_fd, b'x')
            self.assertEqual(daemon.wait(timeout=10), 2)
            diagnostic = daemon.stderr.read()
            self.assertIn('unexpected data', diagnostic)
            self.assertEqual(diagnostic.count('STUB_DMABUF_DEREG fd_alive=1 mapping_alive=1'), 2)
            self.assertEqual(fcntl.fcntl(read_fd, fcntl.F_GETFL), original_flags)
            self.assertEqual(select.select([read_fd], [], [], 0)[0], [read_fd], 'daemon must never consume lease bytes')
            self.assertEqual(os.read(read_fd, 1), b'x')
        finally:
            os.close(read_fd); os.close(write_fd)

    def lease_during_control(self, name, complete_lines):
        read_fd, write_fd = os.pipe()
        client = None
        stop_writing = threading.Event()
        writer = None
        try:
            daemon, sock, _ = self.start_fd(name, parent=read_fd)
            client = socket.socket(socket.AF_UNIX)
            client.settimeout(1)
            client.connect(sock)
            client.sendall(b'STATUS\n' if complete_lines else b'ST')
            if complete_lines:
                response = b''
                while not response.endswith(b'END\n'):
                    response += client.recv(4096)
            def dribble():
                while not stop_writing.wait(.04):
                    try:
                        client.sendall(b'STATUS\n' if complete_lines else b' ')
                        if complete_lines:
                            if not client.recv(4096):
                                return
                    except OSError:
                        return
            writer = threading.Thread(target=dribble, daemon=True)
            writer.start()
            time.sleep(.1)
            os.close(write_fd); write_fd = -1
            self.assertEqual(daemon.wait(timeout=3), 0, 'a control client prevented parent-lease shutdown')
            self.assertIn('parent lease closed; shutting down', daemon.stderr.read())
        finally:
            stop_writing.set()
            if client is not None: client.close()
            if writer is not None: writer.join(timeout=2)
            os.close(read_fd)
            if write_fd >= 0: os.close(write_fd)

    def test_parent_eof_stops_during_persistent_status_monitor(self):
        self.lease_during_control('leasemonitor', True)

    def test_parent_eof_stops_during_partial_command_dribble(self):
        self.lease_during_control('leasedribble', False)

    def test_fd_control_session_has_absolute_deadline_despite_dribble(self):
        daemon, sock, _ = self.start_fd('controldeadline')
        client = socket.socket(socket.AF_UNIX)
        client.settimeout(.1)
        client.connect(sock)
        started = time.monotonic()
        closed = False
        try:
            while time.monotonic() - started < 3:
                try:
                    client.sendall(b' ')
                    if client.recv(16) == b'':
                        closed = True
                        break
                except socket.timeout:
                    pass
                except (BrokenPipeError, ConnectionResetError):
                    closed = True
                    break
            self.assertTrue(closed, 'partial input kept extending the control deadline')
            self.assertIsNone(daemon.poll())
            self.assertIn('PEER controldeadline', command(sock, 'STATUS'))
        finally:
            client.close()

    def test_partial_dmabuf_registration_failure_unwinds_without_payload_writes(self):
        with tempfile.TemporaryFile(dir=self.work) as backing:
            backing.truncate(8 * MIB)
            with mmap.mmap(backing.fileno(), 8 * MIB) as view:
                view[:] = b'\xa7' * (8 * MIB)
                sock = os.path.join(self.work, 'partial.sock')
                args, env = self.fd_arguments(backing.fileno(), 'partial', sock)
                env['STUB_FAIL_DMABUF_AT'] = '2'
                done = subprocess.run(args, env=env, pass_fds=(backing.fileno(),), capture_output=True,
                                      text=True, timeout=10)
                self.assertEqual(done.returncode, 2)
                self.assertIn('segment 1 registration failed', done.stderr)
                self.assertEqual(done.stderr.count('STUB_DMABUF_DEREG fd_alive=1 mapping_alive=1'), 1)
                self.assertNotIn('STUB-VIOLATION', done.stderr)
                self.assertEqual(view[:], b'\xa7' * (8 * MIB))

    def test_fd_mode_preserves_preexisting_posix_mailbox_name(self):
        libc = ctypes.CDLL(None, use_errno=True)
        # Darwin declares shm_open variadic; keep only its fixed arguments in
        # argtypes so arm64 passes the optional mode using the correct ABI.
        libc.shm_open.argtypes = [ctypes.c_char_p, ctypes.c_int]
        libc.shm_open.restype = ctypes.c_int
        libc.shm_unlink.argtypes = [ctypes.c_char_p]
        name = 'fd' + str(os.getpid())
        shm_name = ('/mcdma-rpc.' + name).encode()
        fd = libc.shm_open(shm_name, os.O_CREAT | os.O_EXCL | os.O_RDWR, ctypes.c_uint(0o600))
        self.assertGreaterEqual(fd, 0)
        try:
            os.ftruncate(fd, 4096)
            with mmap.mmap(fd, 4096) as original:
                original[:16] = b'previous mailbox'
            daemon, sock, _ = self.start_fd(name)
            self.assertIn('memory=dmabuf', command(sock, 'STATUS'))
            self.assertEqual(command(sock, 'SHUTDOWN'), 'BYE')
            self.assertEqual(daemon.wait(timeout=10), 0)
            reopened = libc.shm_open(shm_name, os.O_RDONLY, ctypes.c_uint(0))
            self.assertGreaterEqual(reopened, 0, 'FD mode unlinked the unrelated POSIX mailbox')
            try:
                with mmap.mmap(reopened, 4096, access=mmap.ACCESS_READ) as previous:
                    self.assertEqual(previous[:16], b'previous mailbox')
            finally: os.close(reopened)
        finally:
            os.close(fd); libc.shm_unlink(shm_name)

    def test_invalid_descriptors_are_rejected_before_socket_or_registration(self):
        with tempfile.TemporaryFile(dir=self.work) as backing:
            backing.truncate(8 * MIB)
            read_fd, write_fd = os.pipe()
            try:
                for label, buffer_fd, parent_fd in [('regular-lease', backing.fileno(), backing.fileno()),
                                                     ('write-lease', backing.fileno(), write_fd),
                                                     ('pipe-buffer', read_fd, None)]:
                    with self.subTest(label=label):
                        sock = os.path.join(self.work, label + '.sock')
                        args, env = self.fd_arguments(buffer_fd, 'badfd', sock, parent_fd)
                        done = subprocess.run(args, env=env, pass_fds=(backing.fileno(), read_fd, write_fd),
                                              capture_output=True, text=True, timeout=10)
                        self.assertEqual(done.returncode, 2)
                        self.assertFalse(os.path.exists(sock))
                        self.assertNotIn('STUB_DMABUF_REG', done.stderr)
            finally:
                os.close(read_fd); os.close(write_fd)

    # Listen end: the daemon under test, driven over its control port and socket.

    def start(self, name):
        port = free_port()
        sock = os.path.join(self.work, name + '.sock')
        env = dict(os.environ, MCDMA_RPCD_SOCKET=sock, STUB_NOCOPY='1')
        daemon = subprocess.Popen([self.daemon, 'listen', name, 'stub0', '0', '4096', f'127.0.0.1:{port}'],
                                  stderr=subprocess.PIPE, env=env, text=True)
        self.addCleanup(stop, daemon, sock)
        deadline = time.monotonic() + 5
        while command(sock, 'STATUS') is None:
            self.assertLess(time.monotonic(), deadline, 'the listen daemon never answered')
            time.sleep(0.05)
        return daemon, port, sock, os.path.join(self.work, 'mcdma-rpc.' + name)

    def test_a_second_daemon_for_the_same_link_leaves_the_live_one_alone(self):
        live, port, sock, box = self.start('dup')
        with open(box, 'r+b') as stream, mmap.mmap(stream.fileno(), 8 * MIB) as mailbox:
            mailbox[4096:4102] = b'MARKER'
            env = dict(os.environ, MCDMA_RPCD_SOCKET=os.path.join(self.work, 'dup2.sock'), STUB_NOCOPY='1')
            second = subprocess.run([self.daemon, 'listen', 'dup', 'stub0', '0', '4096', f'127.0.0.1:{free_port()}'],
                                    capture_output=True, text=True, env=env, timeout=10)
            self.assertEqual(second.returncode, 2)
            self.assertIn('another mcdma-rpcd serves link dup', second.stderr)
            self.assertEqual(bytes(mailbox[4096:4102]), b'MARKER')
        self.assertIsNone(live.poll())
        self.assertIn('PEER dup down', command(sock, 'STATUS'))

    def test_silent_socket_clients_cannot_hold_shutdown_off(self):
        daemon, _, sock, _ = self.start('quiet')
        idle = []
        for _ in range(4):
            client = socket.socket(socket.AF_UNIX)
            client.connect(sock)
            idle.append(client)
        try:
            deadline = time.monotonic() + 10
            while command(sock, 'SHUTDOWN') != 'BYE':
                self.assertLess(time.monotonic(), deadline, 'SHUTDOWN stayed locked out')
                time.sleep(0.2)
            self.assertEqual(daemon.wait(timeout=10), 0)
        finally:
            for client in idle:
                client.close()

    def test_an_idle_control_connection_gives_way_to_the_real_connect_end(self):
        _, port, _, _ = self.start('idle')
        squatter = socket.create_connection(('127.0.0.1', port))
        try:
            time.sleep(1.5)
            with socket.create_connection(('127.0.0.1', port), timeout=5) as mac:
                mac.sendall(hello())
                self.assertTrue(mac.recv(200).startswith(b'HELLO '))
        finally:
            squatter.close()

    def test_a_failed_second_hello_disarms_the_link(self):
        daemon, port, sock, box = self.start('rehello')
        with socket.create_connection(('127.0.0.1', port), timeout=5) as mac, register(sock) as service:
            mac.sendall(hello())
            self.assertTrue(mac.recv(200).startswith(b'HELLO '))
            mac.sendall(b'READY\n')
            mac.sendall(hello('not-a-gid'))
            self.assertEqual(mac.recv(200).strip(), b'ERR qp')
            with open(box, 'r+b') as stream, mmap.mmap(stream.fileno(), 8 * MIB) as mailbox:
                struct.pack_into('<Q', mailbox, 4 * MIB + 128, (1 << 32) | 16)
                time.sleep(0.5)
            self.assertIsNone(daemon.poll(), 'the daemon died writing a reply without a queue pair')
            self.assertIsNotNone(service)

    def test_losing_the_link_ends_the_service_registration(self):
        _, port, sock, _ = self.start('loss')
        with register(sock) as service:
            mac = socket.create_connection(('127.0.0.1', port), timeout=5)
            mac.sendall(hello())
            self.assertTrue(mac.recv(200).startswith(b'HELLO '))
            mac.sendall(b'READY\n')
            time.sleep(0.3)
            mac.close()
            service.settimeout(5)
            self.assertEqual(service.recv(16), b'BYE\n')

    def test_sigterm_stops_cleanly_and_sighup_is_ignored(self):
        daemon, _, sock, box = self.start('signals')
        daemon.send_signal(signal.SIGHUP)
        time.sleep(0.3)
        self.assertIsNone(daemon.poll(), 'SIGHUP from a closed terminal must not stop the daemon')
        daemon.send_signal(signal.SIGTERM)
        self.assertEqual(daemon.wait(timeout=10), 0)
        self.assertFalse(os.path.exists(sock))
        self.assertFalse(os.path.exists(box))


def free_port():
    with socket.socket() as probe:
        probe.bind(('127.0.0.1', 0))
        return probe.getsockname()[1]


def hello(gid='fe80::1'):
    return f'HELLO 1 5 6 {gid} DIRECT {4 * MIB} {4 * MIB} 1 99 12345\n'.encode()


def command(sock, text):
    try:
        with socket.socket(socket.AF_UNIX) as client:
            client.settimeout(2)
            client.connect(sock)
            client.sendall(text.encode() + b'\n')
            answer = b''
            while chunk := client.recv(4096):
                answer += chunk
                if answer.endswith(b'END\n') or answer in (b'BYE\n', b'ERR unknown command\n'):
                    break
            return answer.decode().strip()
    except OSError:
        return None


class register:
    """A service registered with the listen daemon, as an application would."""

    def __init__(self, sock):
        self.client = socket.socket(socket.AF_UNIX)
        self.client.connect(sock)
        self.client.sendall(b'MODE poll\n')
        self.client.settimeout(5)
        if self.client.recv(3) != b'OK\n':
            raise AssertionError('the service was not registered')

    def __enter__(self):
        return self.client

    def __exit__(self, *_):
        self.client.close()


def stop(daemon, sock):
    if daemon.poll() is None:
        command(sock, 'SHUTDOWN')
        try:
            daemon.wait(timeout=10)
        except subprocess.TimeoutExpired:
            daemon.kill()
            daemon.wait()
    daemon.stderr.close()


if __name__ == '__main__':
    unittest.main()
