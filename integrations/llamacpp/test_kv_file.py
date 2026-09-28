"""File transport tests use local files and bounded fake mailboxes, never RDMA."""
import hashlib
import io
from contextlib import redirect_stderr
import os
from pathlib import Path
import struct
import tempfile
import time
import unittest
from unittest.mock import patch

import kv_file as k
from mcdma_rpc_proxy import ProxyError


class FakeMailbox:
    max_request = max_reply = 128

    def __init__(self, service, mutate=None):
        self.service = service
        self.mutate = mutate
        self.calls = []

    def call(self, data, deadline):
        k.remaining(deadline)
        if len(data) > self.max_request:
            raise AssertionError('oversized request')
        self.calls.append(data)
        response = self.service.handle(data)
        if self.mutate:
            response = self.mutate(data, response)
        return response


class FileTransferTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.source = self.root / 'source.state'
        self.output = self.root / 'received.state'
        self.payload = bytes(range(256)) * 19 + b'last'
        self.source.write_bytes(self.payload)
        self.services = []

    def tearDown(self):
        for service in self.services:
            service.close()
        self.temp.cleanup()

    def service(self, **kwargs):
        service = k.FileService(self.source, FakeMailbox.max_reply - k.WIRE.size,
                                timeout=2, **kwargs)
        self.services.append(service)
        return service

    def assert_incomplete_removed(self):
        self.assertFalse(os.path.lexists(self.output))
        self.assertEqual(list(self.root.glob('*.incomplete')), [])
        self.assertEqual(list(self.root.glob('.*.incomplete')), [])

    def test_many_chunks_match_full_file_hash_and_exact_byte_counts(self):
        service = self.service()
        mailbox = FakeMailbox(service)
        result = k.transfer(mailbox, self.output, timeout=2)
        self.assertEqual(self.output.read_bytes(), self.payload)
        self.assertEqual(result['sha256'], hashlib.sha256(self.payload).hexdigest())
        self.assertEqual(result['bytes'], len(self.payload))
        self.assertGreater(result['mcdma_calls'], 50)
        self.assertEqual(result['mailbox_request_bytes'], sum(map(len, mailbox.calls)))
        self.assertEqual(result['framework_payload_staging'], 'host-file')
        self.assertFalse(result['direct_gpu_kv_transfer'])
        self.assertTrue(service.complete)
        self.assertEqual(self.output.stat().st_mode & 0o777, 0o600)

    def test_empty_file_has_metadata_and_finish_without_reads(self):
        self.source.write_bytes(b'')
        mailbox = FakeMailbox(self.service())
        result = k.transfer(mailbox, self.output)
        self.assertEqual(result['bytes'], 0)
        self.assertEqual(len(mailbox.calls), 2)
        self.assertEqual(self.output.read_bytes(), b'')

    def test_data_corruption_fails_hash_and_removes_temporary(self):
        def mutate(request, response):
            kind, sid, rid, offset, body = k.unpack(response)
            if kind == k.DATA:
                body = bytes([body[0] ^ 1]) + body[1:]
            return k.packet(kind, sid, rid, offset, body)
        with self.assertRaisesRegex(k.FileTransferError, 'SHA-256'):
            k.transfer(FakeMailbox(self.service(), mutate), self.output)
        self.assert_incomplete_removed()

    def test_truncated_chunk_fails_even_before_final_hash(self):
        def mutate(request, response):
            return response[:-1] if k.unpack(response)[0] == k.DATA else response
        with self.assertRaisesRegex(k.FileTransferError, 'truncated'):
            k.transfer(FakeMailbox(self.service(), mutate), self.output)
        self.assert_incomplete_removed()

    def test_out_of_order_response_is_rejected(self):
        def mutate(request, response):
            kind, sid, rid, offset, body = k.unpack(response)
            return k.packet(kind, sid, rid, offset + (kind == k.DATA), body)
        with self.assertRaisesRegex(k.FileTransferError, 'out-of-order'):
            k.transfer(FakeMailbox(self.service(), mutate), self.output)
        self.assert_incomplete_removed()

    def test_corrupt_metadata_is_rejected(self):
        def mutate(request, response):
            kind, sid, rid, offset, body = k.unpack(response)
            if kind == k.META:
                body = k.META_BODY.pack(len(self.payload), bytes(32), 0)
            return k.packet(kind, sid, rid, offset, body)
        with self.assertRaisesRegex(k.FileTransferError, 'chunk limit'):
            k.transfer(FakeMailbox(self.service(), mutate), self.output)
        self.assert_incomplete_removed()

    def test_changed_source_after_handshake_fails_closed(self):
        def mutate(request, response):
            if k.unpack(response)[0] == k.META:
                self.source.write_bytes(b'changed')
            return response
        service = self.service()
        with self.assertRaisesRegex(k.FileTransferError, 'changed'):
            k.transfer(FakeMailbox(service, mutate), self.output)
        self.assertTrue(service.failed)
        self.assert_incomplete_removed()

    def test_replaced_named_source_fails_even_with_open_old_descriptor(self):
        service = self.service()
        self.source.rename(self.root / 'old.state')
        self.source.write_bytes(self.payload)
        with self.assertRaisesRegex(k.FileTransferError, 'changed'):
            k.transfer(FakeMailbox(service), self.output)
        self.assert_incomplete_removed()

    def test_truncated_source_read_is_detected_without_metadata_change(self):
        service = self.service()
        actual_read = os.pread
        def short_read(fd, length, offset):
            return actual_read(fd, max(0, length - 1), offset)
        with patch.object(k.os, 'pread', short_read), self.assertRaisesRegex(k.FileTransferError, 'truncated'):
            k.transfer(FakeMailbox(service), self.output)
        self.assert_incomplete_removed()

    def test_existing_target_and_dangling_symlink_are_never_overwritten(self):
        for symlink in (False, True):
            with self.subTest(symlink=symlink):
                if symlink:
                    self.output.symlink_to(self.root / 'absent')
                else:
                    self.output.write_bytes(b'prior')
                mailbox = FakeMailbox(self.service())
                with self.assertRaises(FileExistsError):
                    k.transfer(mailbox, self.output)
                self.assertEqual(mailbox.calls, [])
                if not symlink:
                    self.assertEqual(self.output.read_bytes(), b'prior')
                self.output.unlink()

    def test_target_created_during_transfer_wins_without_overwrite(self):
        def mutate(request, response):
            if k.unpack(response)[0] == k.DONE:
                self.output.write_bytes(b'concurrent owner')
            return response
        with self.assertRaises(FileExistsError):
            k.transfer(FakeMailbox(self.service(), mutate), self.output)
        self.assertEqual(self.output.read_bytes(), b'concurrent owner')
        self.assertEqual(list(self.root.glob('.*.incomplete')), [])

    def test_generation_loss_removes_partial_and_never_retries(self):
        class LostMailbox(FakeMailbox):
            def call(self, data, deadline):
                if len(self.calls) == 3:
                    raise ProxyError('MCDMA link went down or reconnected; session cannot resume')
                return super().call(data, deadline)
        mailbox = LostMailbox(self.service())
        with self.assertRaisesRegex(ProxyError, 'cannot resume'):
            k.transfer(mailbox, self.output)
        self.assertEqual(len(mailbox.calls), 3)
        self.assert_incomplete_removed()

    def test_total_deadline_is_checked_after_mailbox_returns(self):
        def mutate(request, response):
            time.sleep(.02)
            return response
        with self.assertRaisesRegex(ProxyError, 'deadline'):
            k.transfer(FakeMailbox(self.service(), mutate), self.output, timeout=.005)
        self.assert_incomplete_removed()

    def test_service_refuses_symlink_directory_and_fifo_without_blocking(self):
        for kind in ('symlink', 'directory', 'fifo'):
            target = self.root / kind
            if kind == 'symlink':
                target.symlink_to(self.source)
            elif kind == 'directory':
                target.mkdir()
            else:
                os.mkfifo(target)
            start = time.monotonic()
            with self.subTest(kind=kind), self.assertRaises((OSError, k.FileTransferError)):
                k.FileService(target, 80)
            self.assertLess(time.monotonic() - start, 1)

    def test_service_refuses_skipped_range_and_cannot_resume(self):
        service = self.service()
        sid = b's' * 16
        service.handle(k.packet(k.OPEN, sid))
        with self.assertRaisesRegex(k.FileTransferError, 'out-of-order'):
            service.handle(k.packet(k.READ, sid, 1, 1, k.READ_BODY.pack(4)))
        with self.assertRaisesRegex(k.FileTransferError, 'cannot resume'):
            service.handle(k.packet(k.READ, sid, 1, 0, k.READ_BODY.pack(4)))

    def test_service_rehashes_before_final_ack(self):
        service = self.service()
        def mutate(request, response):
            if k.unpack(request)[0] == k.READ and service.offset == service.size:
                service._hash = lambda: bytes(32)
            return response
        with self.assertRaisesRegex(k.FileTransferError, 'hash changed'):
            k.transfer(FakeMailbox(service, mutate), self.output)
        self.assert_incomplete_removed()

    def test_serve_keeps_registration_until_daemon_consumes_final_reply(self):
        sid = b's' * 16
        requests = [k.packet(k.OPEN, sid)]
        offset = 0
        request_id = 0
        while offset < len(self.payload):
            length = min(80, len(self.payload) - offset)
            request_id += 1
            requests.append(k.packet(k.READ, sid, request_id, offset, k.READ_BODY.pack(length)))
            offset += length
        requests.append(k.packet(k.FINISH, sid, request_id + 1, offset,
                                 hashlib.sha256(self.payload).digest()))
        class ServiceMailbox:
            max_request = max_reply = 128
            request_bytes = 4096
            final_published = False
            drain_polls = 0
            replies = []
            def receive(self, timeout):
                if requests:
                    return len(self.replies) + 1, requests.pop(0)
                self.drain_polls += 1
                return None
            def publish(self, seq, data):
                self.replies.append(data)
                self.final_published = k.unpack(data)[0] == k.DONE
            def _load(self, offset):
                if offset != self.request_bytes:
                    raise AssertionError('wrong ready-word offset')
                return len(self.replies) << 32 if self.final_published and self.drain_polls > 1 else 0
        mailbox = ServiceMailbox()
        with redirect_stderr(io.StringIO()):
            result = k.serve(mailbox, self.source, timeout=2)
        self.assertEqual(result['event'], 'served')
        self.assertEqual(mailbox.drain_polls, 2)
        self.assertEqual(k.unpack(mailbox.replies[-1])[0], k.DONE)

    def test_service_initial_idle_deadline_is_bounded(self):
        class IdleMailbox:
            max_request = max_reply = 128
            def receive(self, timeout):
                time.sleep(timeout)
                return None
        start = time.monotonic()
        with redirect_stderr(io.StringIO()), self.assertRaisesRegex(ProxyError, 'deadline'):
            k.serve(IdleMailbox(), self.source, timeout=.01)
        self.assertLess(time.monotonic() - start, 1)

    def test_distinct_pinned_protocol_rejects_rpc_magic_and_reserved_bits(self):
        valid = k.packet(k.OPEN, b's' * 16)
        for invalid in (b'MCRPC001' + valid[8:], valid[:9] + b'x' + valid[10:], valid[:10]):
            with self.subTest(invalid=invalid), self.assertRaises(k.FileTransferError):
                k.unpack(invalid)


if __name__ == '__main__':
    unittest.main()
