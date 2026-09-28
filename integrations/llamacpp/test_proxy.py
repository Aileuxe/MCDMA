"""Protocol tests use real local sockets and a bounded in-process mailbox."""

import ctypes
import json
import mmap
import os
import socket
import stat
import struct
import sys
import tempfile
import threading
import time
from pathlib import Path
from types import SimpleNamespace
import unittest
import uuid
from unittest.mock import Mock, patch

import mcdma_rpc_proxy as p


class FakeMailbox:
    def __init__(self, service, capacity=128):
        self.service = service
        self.max_request = self.max_reply = capacity
        self.requests = []
        self.replies = []

    def call(self, data, deadline):
        p.remaining(deadline)
        if len(data) > self.max_request:
            raise AssertionError("request exceeds mailbox")
        self.requests.append(data)
        answer = self.service.handle(data)
        if len(answer) > self.max_reply:
            raise AssertionError("reply exceeds mailbox")
        self.replies.append(answer)
        return answer


class IntegrationTests(unittest.TestCase):
    def exercise(self, bodies, *, split_bytes=False):
        caller, proxy = socket.socketpair()
        backend, server_socket = socket.socketpair()
        service = p.RpcService(lambda: backend, 128 - p.WIRE.size, timeout=2)
        mailbox = FakeMailbox(service)
        client = p.RpcClient(mailbox, timeout=2)
        errors, observed = [], []
        replies = {0: b"\x12" * 16, 8: bytes(range(256)) * 11, 15: struct.pack("<I", 1)}

        def native_server():
            try:
                for command, expected in bodies:
                    deadline = time.monotonic() + 2
                    header = p.recv_exact(server_socket, 9, deadline)
                    actual_command, length = p.RPC_HEADER.unpack(header)
                    body = p.recv_exact(server_socket, length, deadline)
                    observed.append((actual_command, body))
                    self.assertEqual(actual_command, command)
                    self.assertEqual(body, bytes(24) if command == p.HELLO else expected)
                    if command in p.NO_RESPONSE:
                        continue
                    response = p.RPC_VERSION + b"\0" + b"x" * 24 if command == p.HELLO else replies[command]
                    data = p.U64.pack(len(response)) + response
                    if split_bytes:
                        for byte in data:
                            server_socket.sendall(bytes((byte,)))
                    else:
                        server_socket.sendall(data)
                self.assertEqual(server_socket.recv(1), b"")
            except BaseException as exc:
                errors.append(exc)
            finally:
                server_socket.close()

        def proxy_thread():
            try:
                client.run(proxy)
            except BaseException as exc:
                errors.append(exc)
            finally:
                service.close()
                proxy.close()

        workers = [threading.Thread(target=native_server), threading.Thread(target=proxy_thread)]
        for worker in workers:
            worker.start()
        try:
            for command, body in bodies:
                data = p.RPC_HEADER.pack(command, len(body)) + body
                if split_bytes:
                    for byte in data:
                        caller.sendall(bytes((byte,)))
                else:
                    caller.sendall(data)
                if command not in p.NO_RESPONSE:
                    deadline = time.monotonic() + 2
                    length = p.U64.unpack(p.recv_exact(caller, 8, deadline))[0]
                    response = p.recv_exact(caller, length, deadline)
                    expected = p.RPC_VERSION + bytes(25) if command == p.HELLO else replies[command]
                    self.assertEqual(response, expected)
        finally:
            caller.close()
            for worker in workers:
                worker.join(3)
                self.assertFalse(worker.is_alive(), "proxy or fake server hung")
        if errors:
            raise errors[0]
        self.assertEqual(len(observed), len(bodies))
        self.assertEqual(client.metrics.counts["mcdma_request_payload_bytes"], sum(map(len, mailbox.requests)))
        self.assertEqual(client.metrics.counts["mcdma_reply_payload_bytes"], sum(map(len, mailbox.replies)))
        self.assertEqual(client.metrics.counts["rpc_request_frame_bytes"], sum(9 + len(body) for _, body in bodies))
        return mailbox, client

    def test_large_bidirectional_frames_and_all_no_response_commands(self):
        bodies = [(p.HELLO, b"s" * 24), (15, b""), (0, b"a" * 12)]
        bodies += [(command, bytes(range(256)) * 11) for command in sorted(p.NO_RESPONSE)]
        bodies += [(8, b"get")]
        mailbox, client = self.exercise(bodies)
        self.assertGreater(len(mailbox.requests), 200)
        self.assertEqual(client.metrics.commands["GRAPH_COMPUTE"], 1)
        self.assertEqual(client.metrics.commands["SET_TENSOR"], 1)
        self.assertEqual(client.metrics.counts["rpc_response_body_bytes"], 28 + 4 + 16 + 2816)

    def test_one_byte_socket_fragmentation(self):
        self.exercise([(p.HELLO, b"r" * 24), (6, b"weights" * 33), (8, b"get")], split_bytes=True)


class ServiceValidationTests(unittest.TestCase):
    def setUp(self):
        self.backend, self.native = socket.socketpair()
        self.service = p.RpcService(lambda: self.backend, 80, timeout=.1)
        self.sid = b"s" * 16
        self.service.handle(p.packet(p.OPEN, self.sid, 0, body=p.LLAMA_COMMIT.encode()))

    def tearDown(self):
        self.service.close()
        self.native.close()

    def start_hello(self):
        self.service.handle(p.packet(p.BEGIN, self.sid, 1, body=p.RPC_HEADER.pack(p.HELLO, 24)))

    def test_handshake_capabilities_must_be_disabled(self):
        self.start_hello()
        with self.assertRaisesRegex(p.ProxyError, "capabilities"):
            self.service.handle(p.packet(p.DATA, self.sid, 1, body=b"x" * 24))

    def test_wrong_session_and_duplicate_begin_are_rejected(self):
        with self.assertRaisesRegex(p.ProxyError, "session"):
            self.service.handle(p.packet(p.BEGIN, b"x" * 16, 1, body=p.RPC_HEADER.pack(p.HELLO, 24)))
        self.start_hello()
        with self.assertRaisesRegex(p.ProxyError, "out-of-order"):
            self.start_hello()

    def test_skipped_and_oversized_chunks_are_rejected(self):
        self.start_hello()
        for offset, body in ((1, bytes(4)), (0, bytes(25)), (0, b"")):
            with self.assertRaises(p.ProxyError):
                self.service.handle(p.packet(p.DATA, self.sid, 1, offset, body))

    def test_pull_cannot_overtake_request(self):
        self.start_hello()
        with self.assertRaisesRegex(p.ProxyError, "incomplete request"):
            self.service.handle(p.packet(p.PULL, self.sid, 1))

    def test_wrong_native_protocol_is_rejected(self):
        self.start_hello()
        self.service.handle(p.packet(p.DATA, self.sid, 1, body=bytes(24)))
        self.native.sendall(p.U64.pack(28) + bytes((6, 0, 0, 0)) + bytes(24))
        with self.assertRaisesRegex(p.ProxyError, "version"):
            self.service.handle(p.packet(p.PULL, self.sid, 1))

    def test_stalled_native_reply_has_deadline(self):
        self.start_hello()
        self.service.handle(p.packet(p.DATA, self.sid, 1, body=bytes(24)))
        started = time.monotonic()
        with self.assertRaises((TimeoutError, p.ProxyError)):
            self.service.handle(p.packet(p.PULL, self.sid, 1))
        self.assertLess(time.monotonic() - started, .5)

    def test_source_pin_is_required(self):
        self.service.close()
        with self.assertRaisesRegex(p.ProxyError, "source pin"):
            self.service.handle(p.packet(p.OPEN, self.sid, 0, body=b"other source"))

    def test_session_expiry_closes_native_socket(self):
        self.service.deadline = time.monotonic() - 1
        with self.assertRaisesRegex(p.ProxyError, "expired"):
            self.service.expire()
        self.assertEqual(self.native.recv(1), b"")

    def test_healthy_close_permits_a_fresh_session(self):
        self.service.handle(p.packet(p.CLOSE, self.sid, 0))
        self.assertEqual(self.native.recv(1), b"")
        other_backend, other_native = socket.socketpair()
        try:
            self.service.connect = lambda: other_backend
            other_sid = b"t" * 16
            self.service.handle(p.packet(p.OPEN, other_sid, 0, body=p.LLAMA_COMMIT.encode()))
            self.service.handle(p.packet(p.BEGIN, other_sid, 1, body=p.RPC_HEADER.pack(p.HELLO, 24)))
            self.assertEqual(p.recv_exact(other_native, 9, time.monotonic() + 1), p.RPC_HEADER.pack(p.HELLO, 24))
        finally:
            other_native.close()


class WordHelper:
    def __init__(self, mailbox):
        self.mailbox = mailbox
        self.on_wait = None

    def mcdma_rpc_wait_word(self, address, seq, equal, spin, timeout):
        if timeout and self.on_wait:
            self.on_wait()
        value = ctypes.c_uint64.from_address(address).value
        current = value >> 32
        return value if (current == seq if equal else current != 0 and current != seq) else 0

    def mcdma_rpc_store_word(self, address, value):
        ctypes.c_uint64.from_address(address).value = value


class MailboxFailureTests(unittest.TestCase):
    def setUp(self):
        self.box = p.Mailbox.__new__(p.Mailbox)
        self.box.service = False
        self.box.control = self.box.lock = None
        self.box.request_bytes = self.box.reply_bytes = 8192
        self.box.max_request = self.box.max_reply = 4096
        self.box.mapping = mmap.mmap(-1, 16384)
        self.box.base = ctypes.addressof(ctypes.c_char.from_buffer(self.box.mapping))
        self.box.helper = WordHelper(self.box)
        self.box.seq, self.box.generation = 2, 5
        p.U64.pack_into(self.box.mapping, 64, 1)
        p.U64.pack_into(self.box.mapping, 72, 5)

    def tearDown(self):
        self.box.close()

    def test_reconnect_during_call_fails_instead_of_replay(self):
        self.box.helper.on_wait = lambda: p.U64.pack_into(self.box.mapping, 72, 6)
        with self.assertRaisesRegex(p.ProxyError, "reconnected"):
            self.box.call(b"request", time.monotonic() + 1)
        self.assertEqual(self.box.seq, 3)

    def test_link_down_and_sequence_wrap_fail(self):
        p.U64.pack_into(self.box.mapping, 64, 0)
        with self.assertRaisesRegex(p.ProxyError, "link went down"):
            self.box.call(b"request", time.monotonic() + 1)
        p.U64.pack_into(self.box.mapping, 64, 1)
        self.box.seq = p.MASK32
        with self.assertRaisesRegex(p.ProxyError, "sequence exhausted"):
            self.box.call(b"request", time.monotonic() + 1)

    def test_no_service_reply_times_out_without_replay(self):
        with self.assertRaisesRegex(p.ProxyError, "deadline"):
            self.box.call(b"request", time.monotonic() + .01)
        self.assertEqual(self.box.seq, 3)


class ProtocolTests(unittest.TestCase):
    def test_bad_framing_and_unpinned_commands(self):
        for data in (b"bad", bytes(p.WIRE.size), p.packet(p.OPEN, bytes(16), 0)[:9] + b"x" + bytes(38)):
            with self.assertRaises(p.ProxyError):
                p.unpack(data)
        for cmd, size, first in ((18, 0, False), (6, 0, True), (14, 24, False), (14, 0, True), (6, 101, False)):
            with self.assertRaises(p.ProxyError):
                p.validate_command(cmd, size, first, 100)


class LocalFileTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.root = Path(self.directory.name)

    def tearDown(self):
        self.directory.cleanup()

    def helper(self):
        return SimpleNamespace(
            mcdma_rpc_abi=Mock(return_value=1),
            mcdma_rpc_wait_word=Mock(side_effect=lambda address, *args: ctypes.c_uint64.from_address(address).value),
            mcdma_rpc_store_word=Mock(),
        )

    def mailbox_file(self):
        path = self.root / "mailbox"
        data = bytearray(16384)
        p.U64.pack_into(data, 64, 1)
        p.U64.pack_into(data, 72, 1)
        p.U64.pack_into(data, 256, 8192)
        p.U64.pack_into(data, 264, 8192)
        path.write_bytes(data)
        return path

    def test_owned_lock_is_private_and_existing_contents_are_preserved(self):
        path = self.root / "lock"
        descriptor, _ = p.open_owned_regular(path, create=True)
        try:
            self.assertEqual(stat.S_IMODE(os.fstat(descriptor).st_mode), 0o600)
            self.assertFalse(os.get_inheritable(descriptor))
            os.write(descriptor, b"existing lock")
        finally:
            os.close(descriptor)
        descriptor, _ = p.open_owned_regular(path, create=True)
        try:
            self.assertEqual(os.read(descriptor, 100), b"existing lock")
        finally:
            os.close(descriptor)

    def test_open_refuses_symlink_hardlink_directory_and_fifo(self):
        target = self.root / "target"
        target.write_bytes(b"do not change")
        symlink = self.root / "symlink"
        symlink.symlink_to(target)
        hardlink = self.root / "hardlink"
        os.link(target, hardlink)
        directory = self.root / "directory"
        directory.mkdir()
        fifo = self.root / "fifo"
        os.mkfifo(fifo)
        for path in (symlink, hardlink, directory, fifo):
            with self.subTest(path=path.name), self.assertRaises((OSError, p.ProxyError)):
                p.open_owned_regular(path, create=True)
        self.assertEqual(target.read_bytes(), b"do not change")

    def test_mailbox_refuses_symlink_before_mapping_or_daemon_registration(self):
        mailbox = self.mailbox_file()
        alias = self.root / "alias"
        alias.symlink_to(mailbox)
        with patch.object(p.ctypes, "CDLL", return_value=self.helper()), patch.object(p.socket, "socket") as connect:
            with self.assertRaises(OSError):
                p.Mailbox("test", "fake", service=True, mailbox_path=str(alias))
            connect.assert_not_called()

    def test_foreign_owner_metadata_is_rejected_and_descriptor_closed(self):
        path = self.mailbox_file()
        original_stat = os.fstat
        opened = []
        original_open = os.open

        def open_file(*args, **kwargs):
            descriptor = original_open(*args, **kwargs)
            opened.append(descriptor)
            return descriptor

        def foreign_stat(descriptor):
            info = original_stat(descriptor)
            return SimpleNamespace(st_mode=info.st_mode, st_uid=os.geteuid() + 1,
                                   st_nlink=info.st_nlink, st_size=info.st_size)

        with patch.object(p.os, "fstat", side_effect=foreign_stat), patch.object(p.os, "open", side_effect=open_file):
            with self.assertRaisesRegex(p.ProxyError, "owned regular file"):
                p.open_owned_regular(path)
        with self.assertRaises(OSError):
            original_stat(opened[0])

    def test_client_lock_refuses_symlink_and_valid_overridden_mailbox_works(self):
        mailbox = self.mailbox_file()
        target = self.root / "target"
        target.write_bytes(b"unchanged")
        lock = self.root / "lock"
        lock.symlink_to(target)
        original_open = os.open

        def open_file(path, *args, **kwargs):
            if path == "/tmp/mcdma-llama.test.client.lock":
                path = lock
            return original_open(path, *args, **kwargs)

        with patch.object(p.os, "open", side_effect=open_file), patch.object(p.ctypes, "CDLL", return_value=self.helper()):
            with self.assertRaises(OSError):
                p.Mailbox("test", "fake", service=False, mailbox_path=str(mailbox))
            self.assertEqual(target.read_bytes(), b"unchanged")
            lock.unlink()
            lock.write_bytes(b"existing lock")
            box = p.Mailbox("test", "fake", service=False, mailbox_path=str(mailbox))
            try:
                self.assertEqual(box.max_request, 4096)
                self.assertEqual(box.generation, 1)
            finally:
                box.close()
            self.assertEqual(lock.read_bytes(), b"existing lock")

    def test_metrics_replace_symlink_without_following_fixed_temp_or_target(self):
        victim = self.root / "victim"
        victim.write_text("unchanged")
        target = self.root / "metrics.json"
        target.symlink_to(victim)
        old_temporary = self.root / "metrics.json.tmp"
        old_temporary.symlink_to(victim)
        p.write_metrics(target, {"first": True})
        self.assertEqual(victim.read_text(), "unchanged")
        self.assertTrue(old_temporary.is_symlink())
        self.assertFalse(target.is_symlink())
        self.assertEqual(stat.S_IMODE(target.stat().st_mode), 0o600)
        self.assertEqual(json.loads(target.read_text()), {"first": True})
        p.write_metrics(target, {"second": True})
        self.assertEqual(json.loads(target.read_text()), {"second": True})
        self.assertEqual(list(self.root.glob(".metrics.json.*.tmp")), [])

    @unittest.skipUnless(sys.platform == "darwin", "native Darwin shared-memory behavior")
    def test_native_darwin_shared_memory_remains_usable(self):
        name = "test-" + uuid.uuid4().hex[:12]
        object_name = ("/mcdma-rpc." + name).encode()
        original_cdll = ctypes.CDLL
        libc = original_cdll(None, use_errno=True)
        libc.shm_open.argtypes = [ctypes.c_char_p, ctypes.c_int]
        libc.shm_unlink.argtypes = [ctypes.c_char_p]
        descriptor = libc.shm_open(object_name, os.O_RDWR | os.O_CREAT | os.O_EXCL, ctypes.c_uint(0o600))
        self.assertGreaterEqual(descriptor, 0)
        box = None
        try:
            os.ftruncate(descriptor, 16384)
            with mmap.mmap(descriptor, 16384) as mapped:
                p.U64.pack_into(mapped, 64, 1)
                p.U64.pack_into(mapped, 72, 1)
                p.U64.pack_into(mapped, 256, 8192)
                p.U64.pack_into(mapped, 264, 8192)
            original_open = os.open

            def open_file(path, *args, **kwargs):
                if path == f"/tmp/mcdma-llama.{name}.client.lock":
                    path = self.root / "native-lock"
                return original_open(path, *args, **kwargs)

            helper = self.helper()
            with patch.object(p.os, "open", side_effect=open_file), patch.object(
                    p.ctypes, "CDLL", side_effect=lambda path, **kw: helper if path == "fake" else original_cdll(path, **kw)):
                box = p.Mailbox(name, "fake", service=False)
                self.assertEqual(box.max_request, 4096)
                self.assertEqual(box.generation, 1)
        finally:
            if box is not None:
                box.close()
            os.close(descriptor)
            self.assertEqual(libc.shm_unlink(object_name), 0)

    def test_failed_metrics_replace_removes_unique_temporary(self):
        target = self.root / "metrics.json"
        target.write_text("old metrics")
        with patch.object(p.os, "replace", side_effect=OSError("replace denied")):
            with self.assertRaisesRegex(OSError, "replace denied"):
                p.write_metrics(target, {"new": True})
        self.assertEqual(target.read_text(), "old metrics")
        self.assertEqual(list(self.root.glob(".metrics.json.*.tmp")), [])


if __name__ == "__main__":
    unittest.main()
