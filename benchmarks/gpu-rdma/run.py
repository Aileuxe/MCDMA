#!/usr/bin/env python3
"""Validate GPU-produced/consumed RDMA payloads over an SSH control channel."""
import argparse
from dataclasses import dataclass
from datetime import datetime, timezone
import ipaddress
import json
import os
from pathlib import Path
import queue
import re
import selectors
import signal
import shlex
import subprocess
import threading
import time

ALLOCATION_BYTES = 24576
WINDOW_BYTES = 16384
MAX_LOG_BYTES = 1024 * 1024


class ValidationError(RuntimeError):
    pass


def redact(line):
    """Descriptors carry live MR capabilities and must never enter saved logs."""
    if line.lstrip().startswith('ENDPOINT'):
        return 'ENDPOINT [redacted]'
    return re.sub(r'\b(rkey|lkey|address|remote_addr|remote_address|pHostPointer)\s*[=:]\s*\S+',
                  r'\1=[redacted]', line, flags=re.IGNORECASE)


def descriptor(line):
    parts = line.split()
    if len(parts) != 7 or parts[0] != 'ENDPOINT':
        raise ValidationError('malformed endpoint descriptor')
    try:
        if any(not re.fullmatch(r'[0-9]+', value) for value in parts[1:6]):
            raise ValueError()
        qpn, psn, key, address, length = map(int, parts[1:6])
        gid = ipaddress.IPv6Address(parts[6])
        if not (0 < qpn < 2**24 and 0 <= psn < 2**24 and 0 < key < 2**32
                and 0 < address < 2**64 - length and length == WINDOW_BYTES
                and not gid.is_unspecified and not gid.is_multicast):
            raise ValueError()
    except ValueError:
        raise ValidationError('invalid endpoint descriptor fields') from None
    return {'qpn': qpn, 'psn': psn, 'rkey': key, 'address': address,
            'length': length, 'gid': str(gid)}


@dataclass(frozen=True)
class Host:
    name: str
    host: str
    executable: str
    backend: str
    device: str
    gid: int
    gpu: int = 0
    port: int = 1
    key: str | None = None
    jump: str | None = None
    shader: str | None = None
    ssh_config: str | None = None

    def command(self, role, payload, seed, mtu, remote_timeout):
        if not self.host or self.host.startswith('-'):
            raise ValidationError('SSH destination must not begin with an option')
        args = ['ssh', '-T', '-o', 'BatchMode=yes', '-o', 'ConnectTimeout=10',
                '-o', 'ServerAliveInterval=10', '-o', 'ServerAliveCountMax=2']
        if self.ssh_config:
            args += ['-F', self.ssh_config]
        if self.key:
            args += ['-i', self.key]
        if self.jump:
            args += ['-J', self.jump]
        remote = ['timeout', '--kill-after=5s', str(remote_timeout) + 's', 'env',
                  f'MCDMA_PAYLOAD_BYTES={payload}', f'MCDMA_GPU_SEED={seed}',
                  f'MCDMA_PATH_MTU={mtu}', f'MCDMA_RDMA_PORT={self.port}']
        if self.shader:
            remote += ['MCDMA_VULKAN_SHADER=' + self.shader]
        remote += [self.executable, self.device, str(self.gid), 'mapped-host', role, str(self.gpu)]
        return [*args, self.host, 'exec ' + shlex.join(remote)]


class Endpoint:
    def __init__(self, host, role, payload, seed, mtu, timeout, directory, popen=subprocess.Popen):
        self.name = host.name
        self.timeout = timeout
        self.lines = queue.Queue()
        self.diagnostics = []
        self.reader_errors = []
        self.threads = []
        self.logs = []
        self.stop_readers = threading.Event()
        # The remote watchdog outlives an interrupted SSH client and bounds jobs
        # that cannot receive EOF because the management connection disappears.
        remote_timeout = max(60, int(timeout * 8 + 10))
        self.process = popen(host.command(role, payload, seed, mtu, remote_timeout),
                             stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                             stderr=subprocess.PIPE, text=True, bufsize=1, start_new_session=True)
        try:
            for stream_name in ('stdout', 'stderr'):
                fd = os.open(directory / f'{host.name}.{stream_name}.log',
                             os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o600)
                log = os.fdopen(fd, 'w')
                self.logs.append(log)
                thread = threading.Thread(target=self._read,
                                          args=(stream_name, log), daemon=True)
                self.threads.append(thread)
                thread.start()
        except BaseException:
            self.close()
            raise

    def _read(self, stream_name, log):
        # Never hold a BufferedReader lock across a blocking read: a descendant
        # may keep this pipe open after the SSH process has already exited.
        descriptor = getattr(self.process, stream_name).fileno()
        pending = bytearray()
        total = 0

        def record(line):
            if len(line) > 4096:
                raise ValidationError('endpoint output exceeds log limit')
            value = line.decode('utf-8').rstrip('\r\n')
            safe = redact(value)
            log.write(safe + '\n')
            log.flush()
            if stream_name == 'stdout':
                self.lines.put(value)
            else:
                self.diagnostics.append(safe)

        try:
            os.set_blocking(descriptor, False)
            with selectors.DefaultSelector() as selector:
                selector.register(descriptor, selectors.EVENT_READ)
                while True:
                    stopping = self.stop_readers.is_set()
                    if not selector.select(0 if stopping else .05):
                        if stopping:
                            break
                        continue
                    try:
                        chunk = os.read(descriptor, 4096)
                    except BlockingIOError:
                        continue
                    if not chunk:
                        if pending:
                            record(pending)
                        break
                    total += len(chunk)
                    if total > MAX_LOG_BYTES:
                        raise ValidationError('endpoint output exceeds log limit')
                    pending.extend(chunk)
                    while b'\n' in pending:
                        end = pending.index(b'\n') + 1
                        record(pending[:end])
                        del pending[:end]
                    if len(pending) > 4096:
                        raise ValidationError('endpoint output exceeds log limit')
        except ValidationError as exc:
            self.reader_errors.append(str(exc))
        except (OSError, ValueError, UnicodeError):
            self.reader_errors.append('endpoint log reader failed')
        finally:
            if stream_name == 'stdout':
                self.lines.put(None)

    def line(self, expected):
        try:
            value = self.lines.get(timeout=self.timeout)
        except queue.Empty:
            raise ValidationError(f'{self.name}: timed out waiting for {expected}') from None
        if value is None:
            raise ValidationError(f'{self.name}: exited waiting for {expected}')
        if value != expected and not value.startswith(expected + ' '):
            raise ValidationError(f'{self.name}: unexpected response while waiting for {expected}')
        return value

    def send(self, value):
        try:
            self.process.stdin.write(value + '\n')
            self.process.stdin.flush()
        except (BrokenPipeError, OSError):
            raise ValidationError(f'{self.name}: control channel closed') from None

    def finish(self):
        try:
            code = self.process.wait(timeout=self.timeout)
        except subprocess.TimeoutExpired:
            raise ValidationError(f'{self.name}: timed out waiting for process exit') from None
        for thread in self.threads:
            thread.join(timeout=self.timeout)
        if any(thread.is_alive() for thread in self.threads):
            raise ValidationError(f'{self.name}: output did not close')
        if self.reader_errors:
            raise ValidationError(f'{self.name}: {self.reader_errors[0]}')
        if code:
            raise ValidationError(f'{self.name}: endpoint exited with code {code}')
        if any(value is not None for value in list(self.lines.queue)):
            raise ValidationError(f'{self.name}: unexpected trailing protocol output')

    def close(self):
        # Own the SSH process group, including a ProxyJump subprocess. The
        # independent remote watchdog still bounds the job across disconnects.
        if self.process.stdin:
            try:
                self.process.stdin.close()
            except OSError:
                pass
        process_error = None
        try:
            # Reap an already-exited leader before signaling its remaining group;
            # some systems report EPERM for a group containing only that zombie.
            self.process.poll()
            try:
                os.killpg(self.process.pid, signal.SIGTERM)
            except ProcessLookupError:
                pass
            try:
                self.process.wait(timeout=1)
            except subprocess.TimeoutExpired:
                pass
            # A departed group leader does not mean its pipe-holding descendants
            # departed too; kill the owned group even when poll() is already final.
            try:
                os.killpg(self.process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            self.process.wait(timeout=1)
        except (OSError, subprocess.SubprocessError) as exc:
            process_error = exc
        self.stop_readers.set()
        deadline = time.monotonic() + 1
        for thread in self.threads:
            thread.join(timeout=max(0, deadline - time.monotonic()))
        if any(thread.is_alive() for thread in self.threads):
            # Do not take an I/O object's lock from a worker that did not stop.
            raise OSError('endpoint output reader did not stop')
        for log in self.logs:
            log.close()
        for stream_name in ('stdout', 'stderr'):
            stream = getattr(self.process, stream_name)
            if stream:
                stream.close()
        if process_error is not None:
            raise process_error


def fields(line, marker):
    if not line.startswith(marker + ' '):
        raise ValidationError('missing ' + marker)
    pairs = line[len(marker) + 1:].split()
    if any(item.count('=') != 1 for item in pairs):
        raise ValidationError('malformed ' + marker)
    result = dict(item.split('=', 1) for item in pairs)
    if len(result) != len(pairs):
        raise ValidationError('duplicate field in ' + marker)
    return result


def require_result(line, marker, expected):
    if fields(line, marker) != {key: str(value) for key, value in expected.items()}:
        raise ValidationError(marker + ': verification or completion failed')


def validate_gpu(diagnostics, host, role, payload, seed, mtu):
    backend = host.backend
    phases = (['initial-guards', 'forward-roundtrip', 'received-reverse'] if role == 'initiator'
              else ['initial-guards', 'received-forward', 'reverse-roundtrip'])
    checks = [fields(line, 'GPU_VERIFY') for line in diagnostics if line.startswith('GPU_VERIFY ')]
    if checks != [{'phase': phase, 'checked_bytes': str(ALLOCATION_BYTES), 'errors': '0'}
                  for phase in phases]:
        raise ValidationError('GPU verification phases, guards or mismatch counts failed')
    configs = [line for line in diagnostics if line.startswith('GPU_CONFIG ')]
    if len(configs) != 1 or not re.search(r'\bbackend=' + re.escape(backend) + r'(?:\s|$)', configs[0]):
        raise ValidationError('missing or unexpected GPU backend')
    gpu_indices = re.findall(r'(?:^|\s)gpu=([^\s]+)', configs[0])
    if gpu_indices != [str(host.gpu)]:
        raise ValidationError('reported GPU index does not match requested endpoint')
    rdma = [fields(line, 'RDMA_CONFIG') for line in diagnostics if line.startswith('RDMA_CONFIG ')]
    if len(rdma) != 1 or any(rdma[0].get(key) != str(value) for key, value in
                             {'device': host.device, 'port': host.port, 'gid_index': host.gid}.items()):
        raise ValidationError('reported RDMA identity does not match requested endpoint')
    config_marker = 'VULKAN_TEST_CONFIG' if backend == 'vulkan' else 'GPU_CONFIG'
    configs = [line for line in diagnostics if line.startswith(config_marker + ' ')]
    expected = {'mode': 'mapped-host', 'seed': seed, 'payload_bytes': payload,
                'path_mtu': mtu, 'cpu_payload_copies': 0}
    if len(configs) != 1 or any(not re.search(r'\b' + key + '=' + re.escape(str(value)) + r'(?:\s|$)', configs[0])
                               for key, value in expected.items()):
        raise ValidationError('GPU configuration does not match requested run')
    registration = f'REGISTRATION mode=mapped-host registered=1 bytes={ALLOCATION_BYTES} fallback=0'
    cleanup = 'GPU_RDMA_COMPLETE mode=mapped-host cleanup=ok payload_cpu_copy_bytes=0'
    if diagnostics.count(registration) != 1 or diagnostics.count(cleanup) != 1:
        raise ValidationError('missing successful registration or cleanup')
    if backend in ('cuda', 'vulkan'):
        wrapped = f'GPU_WRAPPER backend={backend} abi=1 bytes={ALLOCATION_BYTES} payload_cpu_copy_bytes=0'
        released = f'GPU_WRAPPER_RELEASE backend={backend} cleanup=ok'
        if diagnostics.count(wrapped) != 1 or diagnostics.count(released) != 1:
            raise ValidationError('missing public GPU wrapper import or release')
    if backend == 'vulkan':
        allocation = [fields(line, 'VULKAN_ALLOCATION') for line in diagnostics
                      if line.startswith('VULKAN_ALLOCATION ')]
        if len(allocation) != 1 or any(allocation[0].get(key) != value for key, value in
                                     {'mode': 'external-host-import', 'host_visible': '1',
                                      'host_coherent': '1', 'registered_bytes': str(ALLOCATION_BYTES),
                                      'fallback': '0'}.items()):
            raise ValidationError('missing coherent Vulkan host import')
    if any(re.search(r'\b\w*(?:ERROR|TIMEOUT)\b', line) for line in diagnostics):
        raise ValidationError('endpoint reported an error')


def verify_pair(a, b, payload):
    da, db = descriptor(a.line('ENDPOINT')), descriptor(b.line('ENDPOINT'))
    a.send('{qpn} {psn} {gid}'.format(**db))
    b.send('{qpn} {psn} {gid}'.format(**da))
    require_ready = lambda endpoint: endpoint.line('READY') == 'READY'
    if not require_ready(a) or not require_ready(b):
        raise ValidationError('malformed READY marker')
    a.send('INITIATE {rkey} {address} {length}'.format(**db))
    require_result(a.line('NATIVE_FORWARD'), 'NATIVE_FORWARD',
                   {'write': 1, 'read': 1, 'verified': payload})
    b.send('ROUNDTRIP {rkey} {address} {length}'.format(**da))
    require_result(b.line('PEER_RESULT'), 'PEER_RESULT',
                   {'forward': payload, 'write': 1, 'read': 1, 'reverse': payload})
    a.send('CHECKREVERSE')
    require_result(a.line('NATIVE_REVERSE'), 'NATIVE_REVERSE', {'verified': payload})
    a.finish()
    b.finish()


def run(hosts, directory, payload=4096, seed=0, peer_seed=None, mtu=1024,
        timeout=40, swap=False, endpoint_factory=Endpoint):
    directory = Path(directory).expanduser().absolute()
    # Requiring a new directory avoids overwriting prior evidence; even failed
    # attempts retain their manifest and whatever diagnostics were collected.
    directory.mkdir(mode=0o700, parents=False, exist_ok=False)
    report = {'schema': 1, 'pass': False, 'started_utc': datetime.now(timezone.utc).isoformat(),
              'payload_bytes': payload, 'allocation_checked_bytes': ALLOCATION_BYTES,
              'seed': seed, 'peer_seed': seed if peer_seed is None else peer_seed,
              'path_mtu': mtu, 'initiator': hosts[1 if swap else 0].name,
              'claim': 'GPU-produced and GPU-verified shared-memory RDMA correctness',
              'throughput_measured': False, 'endpoints': []}
    endpoints = []
    error = None
    ordered = list(reversed(hosts)) if swap else list(hosts)
    try:
        for index, host in enumerate(ordered):
            role = 'initiator' if index == 0 else 'responder'
            host_seed = seed if index == 0 or peer_seed is None else peer_seed
            report['endpoints'].append({'name': host.name, 'backend': host.backend,
                                        'role': role, 'device': host.device, 'gid_index': host.gid,
                                        'gpu_index': host.gpu, 'rdma_port': host.port})
            endpoints.append(endpoint_factory(host, role, payload, host_seed, mtu, timeout, directory))
        verify_pair(*endpoints, payload)
        for index, endpoint in enumerate(endpoints):
            validate_gpu(endpoint.diagnostics, ordered[index],
                         'initiator' if index == 0 else 'responder', payload,
                         seed if index == 0 or peer_seed is None else peer_seed, mtu)
        report['pass'] = True
    except (ValidationError, OSError, subprocess.SubprocessError) as exc:
        # Exception messages from OS/process constructors can contain host paths.
        error = str(exc) if isinstance(exc, ValidationError) else type(exc).__name__
        report['error'] = error
    except KeyboardInterrupt:
        error = 'interrupted'
        report['error'] = error
    finally:
        for endpoint in endpoints:
            try:
                endpoint.close()
            except (OSError, subprocess.SubprocessError):
                report['pass'] = False
                report['error'] = error = 'failed to stop local control process'
        report['finished_utc'] = datetime.now(timezone.utc).isoformat()
        fd = os.open(directory / 'manifest.json', os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o600)
        with os.fdopen(fd, 'w') as output:
            json.dump(report, output, indent=2)
            output.write('\n')
    return report


def bounded(maximum, minimum=0):
    def convert(value):
        try:
            result = int(value)
        except ValueError:
            raise argparse.ArgumentTypeError('expected an integer') from None
        if not minimum <= result <= maximum:
            raise argparse.ArgumentTypeError(f'expected {minimum}..{maximum}')
        return result
    return convert



def ssh_config_file(value):
    try:
        path = Path(value).expanduser().resolve(strict=True)
        if not path.is_file() or not os.access(path, os.R_OK):
            raise OSError()
    except (OSError, ValueError):
        raise argparse.ArgumentTypeError('SSH config must be an existing readable file') from None
    return str(path)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--results', required=True, type=Path,
                        help='new private directory; parent must exist; never committed')
    parser.add_argument('--ssh-config', type=ssh_config_file,
                        help='private SSH config passed with -F, including jump-host credentials')
    parser.add_argument('--payload', type=int, choices=(1024, 4096), default=4096)
    parser.add_argument('--seed', type=bounded(2**32 - 1), default=0)
    parser.add_argument('--peer-seed', type=bounded(2**32 - 1),
                        help='different low byte deliberately makes the verifier fail')
    parser.add_argument('--mtu', type=int, choices=(1024, 4096), default=1024)
    parser.add_argument('--timeout', type=bounded(300, 1), default=40)
    parser.add_argument('--swap', action='store_true', help='make endpoint b the initiator')
    for name in ('a', 'b'):
        for option in ('host', 'exe', 'device'):
            parser.add_argument(f'--{name}-{option}', required=True)
        parser.add_argument(f'--{name}-backend', choices=('cuda', 'vulkan', 'hip'), required=True)
        parser.add_argument(f'--{name}-gid', type=bounded(255), required=True)
        parser.add_argument(f'--{name}-port', type=bounded(255, 1), default=1)
        parser.add_argument(f'--{name}-gpu', type=bounded(255), default=0)
        for option in ('key', 'jump', 'shader'):
            parser.add_argument(f'--{name}-{option}')
    args = parser.parse_args(argv)
    hosts = [Host(name, *(getattr(args, name + '_' + field) for field in
                         ('host', 'exe', 'backend', 'device', 'gid', 'gpu', 'port', 'key', 'jump', 'shader')),
                  ssh_config=args.ssh_config)
             for name in ('a', 'b')]
    try:
        report = run(hosts, args.results, args.payload, args.seed, args.peer_seed,
                     args.mtu, args.timeout, args.swap)
    except OSError:
        print('GPU_RDMA_VALIDATION pass=0 reason=cannot-create-new-private-results-directory')
        return 1
    print('GPU_RDMA_VALIDATION pass=' + str(int(report['pass'])))
    return 0 if report['pass'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
