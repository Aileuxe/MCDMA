#!/usr/bin/env python3
"""Pinned llama.cpp RPC frames over mcdma-rpcd, with explicit host staging.

Only the sockets between this process and llama.cpp use TCP, on loopback.
Every interhost application byte uses a registered mcdma-rpcd mailbox.
This adapter does not provide direct GPU tensor allocation or GPU zero-copy.
"""

from __future__ import annotations

import argparse
import ctypes
import fcntl
import json
import mmap
import os
import re
import select
import socket
import stat
import struct
import sys
import tempfile
import time
import uuid
from collections import Counter
from contextlib import suppress
from pathlib import Path


LLAMA_COMMIT = "4da6337767f973e2b4d0797e5b323d77d8565e4a"
RPC_VERSION = bytes((7, 0, 0))
CAPS_BYTES = 24
HELLO = 14
COMMANDS = (
    "ALLOC_BUFFER", "GET_ALIGNMENT", "GET_MAX_SIZE", "BUFFER_GET_BASE",
    "FREE_BUFFER", "BUFFER_CLEAR", "SET_TENSOR", "SET_TENSOR_HASH",
    "GET_TENSOR", "COPY_TENSOR", "GRAPH_COMPUTE", "GET_DEVICE_MEMORY",
    "INIT_TENSOR", "GET_ALLOC_SIZE", "HELLO", "DEVICE_COUNT",
    "GRAPH_RECOMPUTE", "MEMSET_TENSOR",
)
NO_RESPONSE = frozenset((4, 5, 6, 10, 12, 16, 17))
RPC_HEADER = struct.Struct("<BQ")
U64 = struct.Struct("<Q")
WIRE = struct.Struct("<8sB7x16sQQ")
MAGIC = b"MCRPC001"
OPEN, BEGIN, DATA, PULL, CLOSE = range(1, 6)
ACK, REPLY, ERROR = 128, 129, 255
CONTROL = 4096
MASK32 = (1 << 32) - 1
DEFAULT_MAX_FRAME = 64 << 30


class ProxyError(RuntimeError):
    """A session failed and must not be retried or resumed."""


def owned_regular_info(descriptor: int):
    info = os.fstat(descriptor)
    if not stat.S_ISREG(info.st_mode) or info.st_uid != os.geteuid() or info.st_nlink != 1:
        raise ProxyError("expected an owned regular file with exactly one link")
    return info


def open_owned_regular(path, *, create: bool = False):
    flags = os.O_RDWR | os.O_NOFOLLOW | os.O_CLOEXEC | os.O_NONBLOCK
    if create:
        flags |= os.O_CREAT
    descriptor = os.open(path, flags, 0o600)
    try:
        return descriptor, owned_regular_info(descriptor)
    except BaseException:
        os.close(descriptor)
        raise


def write_metrics(path, data: dict) -> None:
    target = Path(path)
    descriptor, temporary = tempfile.mkstemp(prefix=f".{target.name}.", suffix=".tmp", dir=target.parent)
    try:
        with os.fdopen(descriptor, "w") as output:
            json.dump(data, output, indent=2, sort_keys=True)
            output.write("\n")
        os.replace(temporary, target)
    finally:
        with suppress(FileNotFoundError):
            os.unlink(temporary)


def remaining(deadline: float) -> float:
    seconds = deadline - time.monotonic()
    if seconds <= 0:
        raise ProxyError("operation deadline expired")
    return seconds


def recv_exact(sock: socket.socket, size: int, deadline: float, *, boundary: bool = False) -> bytes | None:
    data = bytearray()
    while len(data) < size:
        sock.settimeout(remaining(deadline))
        chunk = sock.recv(size - len(data))
        if not chunk:
            if boundary and not data:
                return None
            raise ProxyError("local RPC connection closed inside a frame")
        data.extend(chunk)
    return bytes(data)


def send_all(sock: socket.socket, data: bytes, deadline: float) -> None:
    sock.settimeout(remaining(deadline))
    sock.sendall(data)


def connect_loopback(port: int, timeout: float) -> socket.socket:
    sock = socket.create_connection(("127.0.0.1", port), timeout)
    try:
        # Native RPC headers and bodies arrive in separate mailbox calls.
        # Do not hold a small body behind the header's delayed TCP ACK.
        sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    except BaseException:
        sock.close()
        raise
    return sock


def packet(kind: int, session: bytes, rpc_id: int, offset: int = 0, body: bytes = b"") -> bytes:
    return WIRE.pack(MAGIC, kind, session, rpc_id, offset) + body


def unpack(data: bytes) -> tuple[int, bytes, int, int, bytes]:
    if len(data) < WIRE.size:
        raise ProxyError("truncated mailbox protocol header")
    magic, kind, session, rpc_id, offset = WIRE.unpack_from(data)
    if magic != MAGIC or any(data[9:16]):
        raise ProxyError("unsupported mailbox protocol")
    return kind, session, rpc_id, offset, data[WIRE.size:]


def validate_command(command: int, size: int, first: bool, maximum: int) -> None:
    if not 0 <= command < len(COMMANDS):
        raise ProxyError("unknown pinned llama.cpp RPC command")
    if (command == HELLO) != first:
        raise ProxyError("HELLO must be the first and only handshake")
    if size > maximum:
        raise ProxyError("RPC frame exceeds --max-frame-bytes")
    if command == HELLO and size != CAPS_BYTES:
        raise ProxyError("HELLO does not match the pinned protocol")


class Metrics:
    def __init__(self) -> None:
        self.started = time.monotonic()
        self.counts: Counter = Counter()
        self.commands: Counter = Counter()

    def snapshot(self, event: str, error: str | None = None) -> dict:
        return {
            "event": event,
            "llama_commit": LLAMA_COMMIT,
            "rpc_protocol": "7.0.0",
            "interhost_transport": "mcdma-rpcd-mailbox",
            "framework_payload_staging": "host",
            "direct_gpu_tensor_transfer": False,
            "elapsed_s": time.monotonic() - self.started,
            "counters": dict(self.counts),
            "commands": dict(self.commands),
            "error": error,
        }


class RpcService:
    """One ordered native RPC connection, fed by bounded mailbox messages."""

    def __init__(self, connect, capacity: int, *, timeout: float = 300,
                 maximum: int = DEFAULT_MAX_FRAME, metrics: Metrics | None = None) -> None:
        if capacity < 64:
            raise ProxyError("reply payload capacity must be at least 64 bytes")
        self.connect = connect
        self.capacity = capacity
        self.timeout = timeout
        self.maximum = maximum
        self.metrics = metrics or Metrics()
        self.session = None
        self.sock = None
        self.last_id = 0
        self.command = None
        self.request_size = self.request_offset = 0
        self.reply_size = None
        self.reply_offset = 0
        self.deadline = 0.0

    def close(self) -> None:
        if self.sock is not None:
            with suppress(OSError):
                self.sock.shutdown(socket.SHUT_RDWR)
            self.sock.close()
        self.sock = self.session = None
        self.command = None

    def expire(self) -> None:
        if self.session is not None and time.monotonic() >= self.deadline:
            self.close()
            raise ProxyError("active RPC session expired")

    def _finish(self) -> None:
        self.command = None
        self.reply_size = None
        self.reply_offset = 0
        self.deadline = time.monotonic() + self.timeout

    def handle(self, data: bytes) -> bytes:
        kind, sid, rid, offset, body = unpack(data)
        self.expire()
        if kind == OPEN:
            if self.session is not None or rid or offset or body != LLAMA_COMMIT.encode():
                raise ProxyError("session busy or source pin mismatch")
            self.sock = self.connect()
            self.session, self.last_id = sid, 0
            self.deadline = time.monotonic() + self.timeout
            self.metrics.counts["sessions"] += 1
            return packet(ACK, sid, rid)
        if sid != self.session or self.sock is None:
            raise ProxyError("missing or wrong session")
        if kind == CLOSE:
            if body or offset or rid != self.last_id or self.command is not None:
                raise ProxyError("CLOSE while an RPC frame is incomplete")
            self.close()
            return packet(ACK, sid, rid)
        if kind == BEGIN:
            if self.command is not None or rid != self.last_id + 1 or offset or len(body) != RPC_HEADER.size:
                raise ProxyError("out-of-order RPC frame")
            command, size = RPC_HEADER.unpack(body)
            validate_command(command, size, self.last_id == 0, self.maximum)
            self.deadline = time.monotonic() + self.timeout
            send_all(self.sock, body, self.deadline)
            self.command, self.last_id = command, rid
            self.request_size, self.request_offset = size, 0
            self.reply_size, self.reply_offset = None, 0
            self.metrics.commands[COMMANDS[command]] += 1
            self.metrics.counts["rpc_request_frame_bytes"] += len(body)
            if not size and command in NO_RESPONSE:
                self._finish()
            return packet(ACK, sid, rid)
        if rid != self.last_id or self.command is None:
            raise ProxyError("request does not belong to the active RPC frame")
        if kind == DATA:
            if (offset != self.request_offset or not body
                    or len(body) > self.request_size - self.request_offset):
                raise ProxyError("out-of-order or oversized RPC request chunk")
            if self.command == HELLO and any(body):
                raise ProxyError("native RDMA capabilities must be disabled")
            send_all(self.sock, body, self.deadline)
            self.request_offset += len(body)
            self.metrics.counts["rpc_request_body_bytes"] += len(body)
            self.metrics.counts["rpc_request_frame_bytes"] += len(body)
            new_offset = self.request_offset
            if self.request_offset == self.request_size and self.command in NO_RESPONSE:
                self._finish()
            return packet(ACK, sid, rid, new_offset)
        if kind != PULL or body or offset != self.reply_offset or self.request_offset != self.request_size:
            raise ProxyError("invalid reply pull or incomplete request")
        prefix = b""
        if self.reply_size is None:
            prefix = recv_exact(self.sock, U64.size, self.deadline)
            self.reply_size = U64.unpack(prefix)[0]
            if self.reply_size > self.maximum:
                raise ProxyError("RPC reply exceeds --max-frame-bytes")
            if self.command == HELLO:
                if self.reply_size != 4 + CAPS_BYTES:
                    raise ProxyError("HELLO response size does not match protocol 7")
                hello = recv_exact(self.sock, self.reply_size, self.deadline)
                if hello[:3] != RPC_VERSION:
                    raise ProxyError("RPC server version differs from pinned 7.0.0")
                answer = prefix + hello[:4] + bytes(CAPS_BYTES)
                self.metrics.counts["rpc_response_body_bytes"] += self.reply_size
                self.metrics.counts["rpc_response_frame_bytes"] += len(answer)
                self._finish()
                return packet(REPLY, sid, rid, offset, answer)
        left = U64.size + self.reply_size - self.reply_offset - len(prefix)
        chunk = recv_exact(self.sock, min(self.capacity - len(prefix), left), self.deadline)
        answer = prefix + chunk
        self.reply_offset += len(answer)
        self.metrics.counts["rpc_response_body_bytes"] += len(chunk)
        self.metrics.counts["rpc_response_frame_bytes"] += len(answer)
        if self.reply_offset == U64.size + self.reply_size:
            self._finish()
        return packet(REPLY, sid, rid, offset, answer)


class RpcClient:
    def __init__(self, mailbox, *, timeout: float = 300, maximum: int = DEFAULT_MAX_FRAME,
                 metrics: Metrics | None = None) -> None:
        self.mailbox = mailbox
        self.timeout = timeout
        self.maximum = maximum
        self.capacity = mailbox.max_request - WIRE.size
        if self.capacity < 64 or mailbox.max_reply - WIRE.size < 64:
            raise ProxyError("mailbox payload capacity is too small")
        self.metrics = metrics or Metrics()
        self.sid = uuid.uuid4().bytes

    def _call(self, kind: int, rid: int, offset: int, body: bytes, deadline: float,
              expected: int = ACK, expected_offset: int | None = None) -> bytes:
        request = packet(kind, self.sid, rid, offset, body)
        response = self.mailbox.call(request, deadline)
        self.metrics.counts["mcdma_calls"] += 1
        self.metrics.counts["mcdma_request_payload_bytes"] += len(request)
        self.metrics.counts["mcdma_reply_payload_bytes"] += len(response)
        got, sid, got_id, got_offset, payload = unpack(response)
        if got == ERROR:
            raise ProxyError("remote proxy: " + payload.decode(errors="replace"))
        if (got != expected or sid != self.sid or got_id != rid
                or got_offset != (offset if expected_offset is None else expected_offset)):
            raise ProxyError("mismatched mailbox response")
        if expected == ACK and payload:
            raise ProxyError("ACK contains unexpected bytes")
        return payload

    def run(self, sock: socket.socket) -> None:
        self._call(OPEN, 0, 0, LLAMA_COMMIT.encode(), time.monotonic() + self.timeout)
        self.metrics.counts["sessions"] += 1
        rid = 0
        while True:
            deadline = time.monotonic() + self.timeout
            header = recv_exact(sock, RPC_HEADER.size, deadline, boundary=True)
            if header is None:
                self._call(CLOSE, rid, 0, b"", deadline)
                return
            command, size = RPC_HEADER.unpack(header)
            validate_command(command, size, rid == 0, self.maximum)
            rid += 1
            self._call(BEGIN, rid, 0, header, deadline)
            self.metrics.commands[COMMANDS[command]] += 1
            self.metrics.counts["rpc_request_frame_bytes"] += len(header)
            offset = 0
            while offset < size:
                chunk = recv_exact(sock, min(self.capacity, size - offset), deadline)
                if command == HELLO:
                    chunk = bytes(len(chunk))
                self._call(DATA, rid, offset, chunk, deadline, expected_offset=offset + len(chunk))
                self.metrics.counts["rpc_request_body_bytes"] += len(chunk)
                self.metrics.counts["rpc_request_frame_bytes"] += len(chunk)
                offset += len(chunk)
            if command in NO_RESPONSE:
                continue
            offset = 0
            total = None
            while total is None or offset < total:
                chunk = self._call(PULL, rid, offset, b"", deadline, REPLY)
                if total is None:
                    if len(chunk) < U64.size:
                        raise ProxyError("reply has no native RPC length")
                    total = U64.size + U64.unpack_from(chunk)[0]
                    if total - U64.size > self.maximum:
                        raise ProxyError("RPC reply exceeds --max-frame-bytes")
                    if command == HELLO and (total != 36 or chunk[8:11] != RPC_VERSION
                                             or any(chunk[12:])):
                        raise ProxyError("bad HELLO response or native RDMA capabilities")
                    self.metrics.counts["rpc_response_body_bytes"] -= U64.size
                if not chunk or offset + len(chunk) > total:
                    raise ProxyError("oversized or empty reply chunk")
                send_all(sock, chunk, deadline)
                offset += len(chunk)
                self.metrics.counts["rpc_response_body_bytes"] += len(chunk)
                self.metrics.counts["rpc_response_frame_bytes"] += len(chunk)


class Mailbox:
    """ABI-1 mailbox access through the native acquire/release helper."""

    def __init__(self, name: str, library: str, *, service: bool,
                 mailbox_path: str | None = None, socket_path: str | None = None) -> None:
        if not re.fullmatch(r"[A-Za-z0-9_-]+", name):
            raise ProxyError("link name must contain only letters, digits, underscores or hyphens")
        self.service = service
        self.control = self.lock = self.mapping = None
        self.helper = ctypes.CDLL(library)
        self.helper.mcdma_rpc_abi.restype = ctypes.c_uint32
        if self.helper.mcdma_rpc_abi() != 1:
            raise ProxyError("libmcdma-rpc ABI mismatch")
        self.helper.mcdma_rpc_wait_word.restype = ctypes.c_uint64
        self.helper.mcdma_rpc_wait_word.argtypes = [ctypes.c_void_p, ctypes.c_uint32, ctypes.c_int,
                                                  ctypes.c_uint64, ctypes.c_uint64]
        self.helper.mcdma_rpc_store_word.restype = None
        self.helper.mcdma_rpc_store_word.argtypes = [ctypes.c_void_p, ctypes.c_uint64]
        try:
            if not service:
                descriptor, _ = open_owned_regular(f"/tmp/mcdma-llama.{name}.client.lock", create=True)
                self.lock = os.fdopen(descriptor, "r+b")
                fcntl.flock(self.lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            if sys.platform == "darwin" and mailbox_path is None:
                libc = ctypes.CDLL(None, use_errno=True)
                # shm_open is variadic on Darwin; its mode argument must use
                # the variadic calling convention on Apple silicon.
                libc.shm_open.argtypes = [ctypes.c_char_p, ctypes.c_int]
                descriptor = libc.shm_open(f"/mcdma-rpc.{name}".encode(), os.O_RDWR, ctypes.c_uint(0))
                if descriptor < 0:
                    raise OSError(ctypes.get_errno(), "shm_open failed")
                try:
                    # POSIX shared-memory names do not resolve filesystem symlinks.
                    os.set_inheritable(descriptor, False)
                    info = os.fstat(descriptor)
                    # Darwin's kernel-owned shm namespace reports no file type
                    # and no filesystem links, unlike Linux /dev/shm files.
                    if info.st_uid != os.geteuid() or stat.S_IFMT(info.st_mode) != 0 or info.st_nlink != 0:
                        raise ProxyError("expected an owned Darwin POSIX shared-memory object")
                except BaseException:
                    os.close(descriptor)
                    raise
            else:
                descriptor, info = open_owned_regular(mailbox_path or f"/dev/shm/mcdma-rpc.{name}")
            try:
                self.mapping = mmap.mmap(descriptor, info.st_size)
            finally:
                os.close(descriptor)
            self.base = ctypes.addressof(ctypes.c_char.from_buffer(self.mapping))
            if len(self.mapping) < CONTROL:
                raise ProxyError("mailbox is smaller than its control page")
            self.request_bytes, self.reply_bytes = self._load(256), self._load(264)
            if (self.request_bytes <= CONTROL or self.reply_bytes <= CONTROL
                    or self.request_bytes + self.reply_bytes != len(self.mapping)):
                raise ProxyError("invalid or uninitialized mailbox sizes")
            self.max_request = self.request_bytes - CONTROL
            self.max_reply = self.reply_bytes - CONTROL
            if service:
                self.control = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                self.control.settimeout(5)
                self.control.connect(socket_path or f"/tmp/mcdma-rpcd.{name}.sock")
                self.control.sendall(b"MODE poll\n")
                answer = bytearray()
                while not answer.endswith(b"\n") and len(answer) < 256:
                    chunk = self.control.recv(1)
                    if not chunk:
                        break
                    answer.extend(chunk)
                if answer != b"OK\n":
                    raise ProxyError("daemon refused exclusive service registration")
                self.last = self._load(0) >> 32
            else:
                self.seq = self._load(0) >> 32
                self.generation = self._load(72)
                self._check_link()
        except BaseException:
            self.close()
            raise

    def _load(self, offset: int) -> int:
        # Get an acquire-ordered snapshot, including non-sequence control words.
        seq = U64.unpack_from(self.mapping, offset)[0] >> 32
        return self.helper.mcdma_rpc_wait_word(self.base + offset, seq, 1, 0, 0)

    def _check_link(self) -> None:
        if self._load(64) != 1 or self._load(72) != self.generation:
            raise ProxyError("MCDMA link went down or reconnected; session cannot resume")

    def call(self, data: bytes, deadline: float) -> bytes:
        if self.service or len(data) > self.max_request:
            raise ProxyError("invalid mailbox call")
        self._check_link()
        self.seq = (self.seq + 1) & MASK32
        if not self.seq:
            raise ProxyError("mailbox sequence exhausted; restart the link before reuse")
        self.mapping[CONTROL:CONTROL + len(data)] = data
        self.helper.mcdma_rpc_store_word(self.base, self.seq << 32 | len(data))
        while True:
            self._check_link()
            word = self.helper.mcdma_rpc_wait_word(self.base + self.request_bytes + 64, self.seq,
                                                  1, 100_000, int(min(.05, remaining(deadline)) * 1e9))
            if not word:
                continue
            self._check_link()
            length = word & MASK32
            if length > self.max_reply:
                raise ProxyError("daemon reply exceeds mailbox capacity")
            start = self.request_bytes + CONTROL
            answer = self.mapping[start:start + length]
            self._check_link()
            return answer

    def receive(self, timeout: float = .1) -> tuple[int, bytes] | None:
        if not self.service:
            raise ProxyError("receive requires the service mailbox")
        readable, _, _ = select.select([self.control], [], [], 0)
        if readable:
            raise ProxyError("daemon ended service registration; session cannot resume")
        word = self.helper.mcdma_rpc_wait_word(self.base, self.last, 0, 100_000, int(timeout * 1e9))
        if not word:
            return None
        if select.select([self.control], [], [], 0)[0]:
            raise ProxyError("daemon ended service registration while receiving")
        length = word & MASK32
        if length > self.max_request:
            raise ProxyError("daemon request exceeds mailbox capacity")
        self.last = word >> 32
        return self.last, self.mapping[CONTROL:CONTROL + length]

    def publish(self, seq: int, data: bytes) -> None:
        if not self.service or len(data) > self.max_reply:
            raise ProxyError("invalid mailbox reply")
        if select.select([self.control], [], [], 0)[0]:
            raise ProxyError("daemon ended service registration before reply")
        start = self.request_bytes + CONTROL
        self.mapping[start:start + len(data)] = data
        self.helper.mcdma_rpc_store_word(self.base + self.request_bytes + 128, seq << 32 | len(data))

    def close(self) -> None:
        for obj in (self.control, self.mapping, self.lock):
            if obj is not None:
                obj.close()
        self.control = self.mapping = self.lock = None


def positive(value: str) -> float:
    number = float(value)
    if not 0 < number < float("inf"):
        raise argparse.ArgumentTypeError("must be a finite positive number")
    return number


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mode", choices=("client", "service"))
    parser.add_argument("--link", required=True)
    parser.add_argument("--library", default=os.environ.get("MCDMA_RPC_LIBRARY", "libmcdma-rpc.so"))
    parser.add_argument("--port", type=int, required=True, help="local loopback listener or rpc-server port")
    parser.add_argument("--timeout", type=positive, default=300, help="maximum seconds per frame and idle session")
    parser.add_argument("--max-frame-bytes", type=int, default=DEFAULT_MAX_FRAME)
    parser.add_argument("--mailbox-path")
    parser.add_argument("--daemon-socket")
    parser.add_argument("--metrics", help="write aggregate JSON after each session and at exit")
    args = parser.parse_args(argv)
    if not 0 < args.port < 65536 or args.max_frame_bytes < CAPS_BYTES:
        parser.error("invalid port or maximum frame size")
    metrics = Metrics()
    mailbox = handler = listener = None

    def report(event, error=None):
        data = metrics.snapshot(event, error)
        print(json.dumps(data, sort_keys=True), file=sys.stderr, flush=True)
        if args.metrics:
            write_metrics(args.metrics, data)

    try:
        mailbox = Mailbox(args.link, args.library, service=args.mode == "service",
                          mailbox_path=args.mailbox_path, socket_path=args.daemon_socket)
        if args.mode == "service":
            handler = RpcService(lambda: connect_loopback(args.port, args.timeout),
                                 mailbox.max_reply - WIRE.size, timeout=args.timeout,
                                 maximum=args.max_frame_bytes, metrics=metrics)
            report("ready")
            while True:
                handler.expire()
                item = mailbox.receive()
                if item is None:
                    continue
                seq, request = item
                try:
                    response = handler.handle(request)
                except Exception as exc:
                    with suppress(Exception):
                        _, sid, rid, offset, _ = unpack(request)
                        message = str(exc).encode()[:mailbox.max_reply - WIRE.size]
                        mailbox.publish(seq, packet(ERROR, sid, rid, offset, message))
                    raise
                mailbox.publish(seq, response)
                metrics.counts["mcdma_calls"] += 1
                metrics.counts["mcdma_request_payload_bytes"] += len(request)
                metrics.counts["mcdma_reply_payload_bytes"] += len(response)
                if handler.session is None:
                    report("session_closed")
        else:
            listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            listener.bind(("127.0.0.1", args.port))
            listener.listen(1)
            report("ready")
            while True:
                client, _ = listener.accept()
                with client:
                    client.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
                    RpcClient(mailbox, timeout=args.timeout, maximum=args.max_frame_bytes,
                              metrics=metrics).run(client)
                report("session_closed")
    except KeyboardInterrupt:
        report("stopped")
        return 0
    except Exception as exc:
        metrics.counts["failures"] += 1
        report("failed", str(exc))
        return 1
    finally:
        if handler is not None:
            handler.close()
        if mailbox is not None:
            mailbox.close()
        if listener is not None:
            listener.close()


if __name__ == "__main__":
    raise SystemExit(main())
