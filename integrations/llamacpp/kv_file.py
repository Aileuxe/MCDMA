#!/usr/bin/env python3
"""Copy one fixed KV-state file through MCDMA mailboxes with CPU file staging.

The protocol transfers opaque file bytes and verifies SHA-256; it does not run
inference, interpret a llama.cpp state file or transfer GPU KV buffers directly.
Use a dedicated link with no concurrent proxy service. Serve exports one file
once, and pull publishes a new output only after full verification.
"""
from __future__ import annotations

import argparse
from contextlib import suppress
import hashlib
import json
import os
from pathlib import Path
import stat
import struct
import sys
import tempfile
import time
import uuid

from mcdma_rpc_proxy import Mailbox, ProxyError, positive, remaining

MAGIC = b'MCKV0001'
WIRE = struct.Struct('<8sB7x16sQQ')
META_BODY = struct.Struct('<Q32sI')
READ_BODY = struct.Struct('<I')
OPEN, READ, FINISH = 1, 2, 3
META, DATA, DONE, ERROR = 128, 129, 130, 255
MAX_FILE_BYTES = 64 << 30
DEFAULT_CHUNK = 1 << 20


class FileTransferError(ProxyError):
    """The transfer is invalid and cannot be resumed."""


def packet(kind, session, request_id=0, offset=0, body=b''):
    return WIRE.pack(MAGIC, kind, session, request_id, offset) + body


def unpack(data):
    if len(data) < WIRE.size:
        raise FileTransferError('truncated file protocol header')
    magic, kind, session, request_id, offset = WIRE.unpack_from(data)
    if magic != MAGIC or any(data[9:16]):
        raise FileTransferError('unsupported file protocol version')
    return kind, session, request_id, offset, data[WIRE.size:]


def identity(info):
    return (info.st_dev, info.st_ino, info.st_size, info.st_mtime_ns, info.st_ctime_ns)


class FileService:
    """One immutable-by-observation regular file, never a wire-supplied path."""
    def __init__(self, filename, capacity, *, timeout=300, maximum=MAX_FILE_BYTES,
                 chunk_bytes=DEFAULT_CHUNK):
        if capacity < META_BODY.size or chunk_bytes < 1:
            raise FileTransferError('mailbox capacity or chunk limit is too small')
        self.capacity = min(capacity, chunk_bytes, 2**32 - 1)
        self.timeout = timeout
        self.filename = Path(filename)
        self.fd = None
        self.session = None
        self.request_id = self.offset = 0
        self.complete = self.failed = False
        self.deadline = time.monotonic() + timeout
        preparing = time.monotonic()
        self.started = None
        try:
            self.fd = os.open(self.filename, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK | os.O_CLOEXEC)
            info = os.fstat(self.fd)
            if not stat.S_ISREG(info.st_mode):
                raise FileTransferError('export must be a regular file')
            if info.st_size > maximum:
                raise FileTransferError('export exceeds maximum file size')
            self.snapshot = identity(info)
            self.size = info.st_size
            self.digest = self._hash()
            self.prepare_wall_s = time.monotonic() - preparing
            self.deadline = time.monotonic() + timeout
        except BaseException:
            self.close()
            raise

    def check_source(self):
        remaining(self.deadline)
        try:
            current = os.fstat(self.fd)
            named = os.stat(self.filename, follow_symlinks=False)
        except OSError:
            raise FileTransferError('export disappeared or changed') from None
        if (identity(current) != self.snapshot or identity(named) != self.snapshot
                or not stat.S_ISREG(named.st_mode)):
            raise FileTransferError('export changed during transfer')

    def _hash(self):
        self.check_source()
        digest = hashlib.sha256()
        offset = 0
        while offset < self.size:
            remaining(self.deadline)
            data = os.pread(self.fd, min(DEFAULT_CHUNK, self.size - offset), offset)
            if not data:
                raise FileTransferError('export truncated during hash')
            digest.update(data)
            offset += len(data)
        self.check_source()
        return digest.digest()

    def handle(self, data):
        if self.failed or self.complete:
            raise FileTransferError('file session cannot resume')
        try:
            return self._handle(data)
        except BaseException:
            self.failed = True
            raise

    def _handle(self, data):
        kind, session, request_id, offset, body = unpack(data)
        self.check_source()
        if kind == OPEN:
            if self.session is not None or request_id or offset or body or not any(session):
                raise FileTransferError('invalid or duplicate file handshake')
            self.session = session
            self.started = time.monotonic()
            self.deadline = self.started + self.timeout
            return packet(META, session, body=META_BODY.pack(self.size, self.digest, self.capacity))
        if (session != self.session or request_id != self.request_id + 1 or offset != self.offset):
            raise FileTransferError('out-of-order file request')
        if kind == READ:
            if len(body) != READ_BODY.size:
                raise FileTransferError('malformed file range')
            length, = READ_BODY.unpack(body)
            if not 0 < length <= min(self.capacity, self.size - self.offset):
                raise FileTransferError('file range exceeds export or mailbox')
            chunk = os.pread(self.fd, length, self.offset)
            if len(chunk) != length:
                raise FileTransferError('export truncated during range read')
            self.check_source()
            response = packet(DATA, session, request_id, self.offset, chunk)
            self.offset += length
            self.request_id = request_id
            return response
        if kind != FINISH or self.offset != self.size or body != self.digest:
            raise FileTransferError('incomplete or invalid file finish')
        # Rehash the fixed descriptor before acknowledging: a metadata snapshot
        # alone cannot detect all writes through an existing writable mapping.
        if self._hash() != self.digest:
            raise FileTransferError('export hash changed during transfer')
        self.complete = True
        self.request_id = request_id
        return packet(DONE, session, request_id, offset, self.digest)

    def close(self):
        if self.fd is not None:
            os.close(self.fd)
            self.fd = None


def transfer(mailbox, output, *, timeout=300, maximum=MAX_FILE_BYTES,
             chunk_bytes=DEFAULT_CHUNK):
    """Pull into a private temp, verify all bytes, then atomically create output."""
    if mailbox.max_request < WIRE.size + 32 or mailbox.max_reply < WIRE.size + META_BODY.size:
        raise FileTransferError('mailbox is too small for file protocol')
    if chunk_bytes < 1:
        raise FileTransferError('chunk limit must be positive')
    output = Path(output)
    # lexists also rejects dangling symlinks; the hard-link publication below
    # independently enforces no overwrite if a target appears during transfer.
    if os.path.lexists(output):
        raise FileExistsError('output already exists')
    session = uuid.uuid4().bytes
    started = time.monotonic()
    deadline = started + timeout
    calls = request_bytes = reply_bytes = 0
    temp_path = None
    fd = None

    def call(kind, request_id, offset, body, expected):
        nonlocal calls, request_bytes, reply_bytes
        remaining(deadline)
        request = packet(kind, session, request_id, offset, body)
        response = mailbox.call(request, deadline)
        remaining(deadline)
        calls += 1
        request_bytes += len(request)
        reply_bytes += len(response)
        if len(response) > mailbox.max_reply:
            raise FileTransferError('reply exceeds mailbox capacity')
        got, got_session, got_id, got_offset, payload = unpack(response)
        if got == ERROR:
            raise FileTransferError('remote refused file request')
        if (got != expected or got_session != session or got_id != request_id or got_offset != offset):
            raise FileTransferError('mismatched or out-of-order file response')
        return payload

    try:
        metadata = call(OPEN, 0, 0, b'', META)
        if len(metadata) != META_BODY.size:
            raise FileTransferError('malformed file metadata')
        size, expected_hash, remote_chunk = META_BODY.unpack(metadata)
        if size > maximum or not remote_chunk:
            raise FileTransferError('invalid file size or chunk limit')
        capacity = min(remote_chunk, mailbox.max_reply - WIRE.size, chunk_bytes)
        fd, temporary = tempfile.mkstemp(prefix='.' + output.name + '.', suffix='.incomplete', dir=output.parent)
        temp_path = Path(temporary)
        digest = hashlib.sha256()
        offset = request_id = 0
        while offset < size:
            length = min(capacity, size - offset)
            request_id += 1
            data = call(READ, request_id, offset, READ_BODY.pack(length), DATA)
            if len(data) != length:
                raise FileTransferError('truncated or oversized file chunk')
            digest.update(data)
            view = memoryview(data)
            while view:
                remaining(deadline)
                written = os.write(fd, view)
                if written <= 0:
                    raise FileTransferError('incomplete output write')
                view = view[written:]
            offset += length
        if digest.digest() != expected_hash:
            raise FileTransferError('file SHA-256 mismatch')
        final = call(FINISH, request_id + 1, offset, expected_hash, DONE)
        if final != expected_hash:
            raise FileTransferError('invalid final hash acknowledgment')
        os.fsync(fd)
        remaining(deadline)
        os.close(fd)
        fd = None
        os.link(temp_path, output, follow_symlinks=False)
        temp_path.unlink()
        temp_path = None
        return {'event': 'complete', 'protocol': MAGIC.decode(), 'bytes': size,
                'sha256': expected_hash.hex(), 'transfer_wall_s': time.monotonic() - started,
                'mcdma_calls': calls, 'mailbox_request_bytes': request_bytes,
                'mailbox_reply_bytes': reply_bytes, 'interhost_transport': 'mcdma-rpcd-mailbox',
                'framework_payload_staging': 'host-file', 'direct_gpu_kv_transfer': False}
    finally:
        if fd is not None:
            os.close(fd)
        if temp_path is not None:
            with suppress(FileNotFoundError):
                temp_path.unlink()


def serve(mailbox, source, *, timeout=300, maximum=MAX_FILE_BYTES, chunk_bytes=DEFAULT_CHUNK):
    if mailbox.max_request < WIRE.size + 32:
        raise FileTransferError('mailbox is too small for file protocol')
    service = FileService(source, mailbox.max_reply - WIRE.size, timeout=timeout,
                          maximum=maximum, chunk_bytes=chunk_bytes)
    try:
        print(json.dumps({'event': 'ready', 'protocol': MAGIC.decode(), 'bytes': service.size,
                          'sha256': service.digest.hex(), 'framework_payload_staging': 'host-file',
                          'source_prepare_wall_s': service.prepare_wall_s,
                          'direct_gpu_kv_transfer': False}), file=sys.stderr, flush=True)
        while not service.complete:
            remaining(service.deadline)
            item = mailbox.receive(min(.1, remaining(service.deadline)))
            if item is None:
                continue
            seq, request = item
            try:
                response = service.handle(request)
            except Exception:
                with suppress(Exception):
                    _, sid, rid, offset, _ = unpack(request)
                    mailbox.publish(seq, packet(ERROR, sid, rid, offset))
                raise
            mailbox.publish(seq, response)
        # The daemon checks service detachment before checking the staged word.
        # Keep registration alive until it has consumed the final staged reply;
        # its acquire-loaded ready word is set inside the synchronous send path.
        while mailbox._load(mailbox.request_bytes) >> 32 != seq:
            remaining(service.deadline)
            if mailbox.receive(min(.001, remaining(service.deadline))) is not None:
                raise FileTransferError('unexpected request after completed file')
        return {'event': 'served', 'protocol': MAGIC.decode(), 'bytes': service.size,
                'sha256': service.digest.hex(), 'transfer_wall_s': time.monotonic() - service.started,
                'source_prepare_wall_s': service.prepare_wall_s,
                'framework_payload_staging': 'host-file', 'direct_gpu_kv_transfer': False}
    finally:
        service.close()


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('mode', choices=('serve', 'pull'))
    parser.add_argument('--link', required=True)
    parser.add_argument('--file', help='single fixed regular file exported by serve')
    parser.add_argument('--output', help='new destination file created by pull; never overwritten')
    parser.add_argument('--library', default=os.environ.get('MCDMA_RPC_LIBRARY', 'libmcdma-rpc.so'))
    parser.add_argument('--mailbox-path')
    parser.add_argument('--daemon-socket')
    parser.add_argument('--timeout', type=positive, default=300,
                        help='total transfer deadline and initial service idle deadline in seconds')
    parser.add_argument('--max-file-bytes', type=int, default=MAX_FILE_BYTES)
    parser.add_argument('--chunk-bytes', type=int, default=DEFAULT_CHUNK)
    parser.add_argument('--metrics', help='new private JSON metrics file; never overwritten')
    args = parser.parse_args(argv)
    if ((args.mode == 'serve' and (not args.file or args.output)) or
            (args.mode == 'pull' and (not args.output or args.file))):
        parser.error('serve requires only --file; pull requires only --output')
    if args.max_file_bytes < 0 or not 0 < args.chunk_bytes < 2**32:
        parser.error('invalid file size or chunk limit')
    mailbox = None
    metrics_fd = None
    try:
        if args.metrics:
            metrics_fd = os.open(args.metrics, os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o600)
        mailbox = Mailbox(args.link, args.library, service=args.mode == 'serve',
                          mailbox_path=args.mailbox_path, socket_path=args.daemon_socket)
        operation = serve if args.mode == 'serve' else transfer
        result = operation(mailbox, args.file if args.mode == 'serve' else args.output,
                           timeout=args.timeout, maximum=args.max_file_bytes, chunk_bytes=args.chunk_bytes)
        status = 0
    except (Exception, KeyboardInterrupt) as exc:
        result = {'event': 'failed', 'protocol': MAGIC.decode(), 'error_type': type(exc).__name__,
                  'framework_payload_staging': 'host-file', 'direct_gpu_kv_transfer': False}
        if isinstance(exc, ProxyError):
            result['error'] = str(exc)
        status = 1
    finally:
        if mailbox is not None:
            mailbox.close()
    print(json.dumps(result, sort_keys=True), flush=True)
    if metrics_fd is not None:
        with os.fdopen(metrics_fd, 'w') as output:
            json.dump(result, output, indent=2, sort_keys=True)
            output.write('\n')
    return status


if __name__ == '__main__':
    raise SystemExit(main())
