"""Protocol checks use real local subprocesses, without GPU/RDMA dependencies."""
import argparse
import importlib.util
import json
import os
import signal
from pathlib import Path
import shlex
import stat
import subprocess
import sys
import tempfile
import time
import unittest

MODULE_PATH = Path(__file__).resolve().parents[1] / 'benchmarks/gpu-rdma/run.py'
SPEC = importlib.util.spec_from_file_location('gpu_rdma_runner', MODULE_PATH)
runner = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = runner
SPEC.loader.exec_module(runner)

# All endpoint data is fictional. This process implements the public control
# sequence while letting tests corrupt evidence independently of exit status.
PEER = r'''
import json, sys, time
config = json.loads(sys.argv[1])
role, backend, payload, seed, mtu = [config[k] for k in ('role', 'backend', 'payload', 'seed', 'mtu')]
fault = config['fault']
def out(line): print(line, flush=True)
def err(line): print(line, file=sys.stderr, flush=True)
def check(phase, errors=0):
    if fault != 'missing-gpu':
        err(f'GPU_VERIFY phase={phase} checked_bytes=24576 errors={errors}')
err(f'GPU_CONFIG backend={backend} gpu={config.get("gpu", 0) + (fault == "wrong-gpu")} name=Example GPU mode=mapped-host seed={seed} payload_bytes={payload} path_mtu={mtu} cpu_payload_copies=0')
if backend == 'vulkan':
    err(f'VULKAN_TEST_CONFIG mode=mapped-host seed={seed} payload_bytes={payload} path_mtu={mtu} cpu_payload_copies=0')
    err('VULKAN_ALLOCATION mode=external-host-import memory_type=0 host_visible=1 host_coherent=1 registered_bytes=24576 backing_bytes=24576 fallback=0')
if fault != 'missing-wrapper':
    err(f'GPU_WRAPPER backend={backend} abi=1 bytes=24576 payload_cpu_copy_bytes=0')
err(f'RDMA_CONFIG device={config.get("device", "rdma0") if fault != "wrong-device" else "other0"} port={config.get("port", 1) + (fault == "wrong-port")} gid_index={config.get("gid", 0) + (fault == "wrong-gid")} vendor=1 part=2 firmware=example')
err('REGISTRATION mode=mapped-host registered=1 bytes=24576 fallback=0')
check('initial-guards')
if fault == 'timeout': time.sleep(30)
if fault == 'oversized': out('x' * 5000); time.sleep(30)
if fault == 'malformed': out('ENDPOINT 1 2 3 4 5 garbage'); sys.exit(0)
out('ENDPOINT 42 12 13579 65536 16384 2001:db8::1')
line = input().split()
assert len(line) == 3 and int(line[0]) == 42 and line[2] == '2001:db8::1'
out('READY')
command = input().split()
assert len(command) == 4 and command[1:] == ['13579', '65536', '16384']
if role == 'initiator':
    assert command[0] == 'INITIATE'
    check('forward-roundtrip')
    out(f'NATIVE_FORWARD write=1 read=1 verified={payload if fault != "wrong-count" else payload - 1}')
    assert input() == 'CHECKREVERSE'
    check('received-reverse', 1 if fault == 'gpu-mismatch' else 0)
    out(f'NATIVE_REVERSE verified={payload}')
else:
    assert command[0] == 'ROUNDTRIP'
    if fault == 'negative': check('received-forward', payload); sys.exit(2)
    check('received-forward')
    check('reverse-roundtrip')
    out(f'PEER_RESULT forward={payload} write=1 read=1 reverse={payload}')
if fault == 'reported-error': err('CQ_ERROR status=1')
if fault != 'missing-cleanup':
    err(f'GPU_WRAPPER_RELEASE backend={backend} cleanup=ok')
    err('GPU_RDMA_COMPLETE mode=mapped-host cleanup=ok payload_cpu_copy_bytes=0')
if fault == 'extra-protocol': out('UNEXPECTED')
if fault == 'unterminated-extra': sys.stdout.write('UNEXPECTED'); sys.stdout.flush()
sys.exit(7 if fault == 'nonzero' else 0)
'''


def inherited_pipe_case(directory, mode):
    """Isolate a potentially stuck close behind the test process watchdog."""
    directory = Path(directory)
    child_pid = directory / 'descendant.pid'
    peer_pid = directory / 'peer.pid'
    descendant = "import os, pathlib, signal, sys, time; signal.signal(signal.SIGTERM, signal.SIG_IGN); pathlib.Path(sys.argv[1]).write_text(str(os.getpid())); time.sleep(30)"
    parent = """import os, pathlib, subprocess, sys, time
pathlib.Path(sys.argv[2]).write_text(str(os.getpid()))
subprocess.Popen([sys.executable, '-c', sys.argv[3], sys.argv[1]])
while not pathlib.Path(sys.argv[1]).exists(): time.sleep(.001)
if sys.argv[4] == 'exit': sys.exit(0)
if sys.argv[4] == 'complete':
    source = sys.argv[5]
    sys.argv = [sys.argv[0], sys.argv[6]]
    exec(source)
time.sleep(30)
"""
    hosts = [runner.Host(name, 'example.invalid', '/opt/check/peer', 'cuda', 'rdma0', 0)
             for name in ('a', 'b')]
    held_writers = []
    def factory(host, role, payload, seed, mtu, timeout, results):
        config = dict(role=role, backend=host.backend, payload=payload, seed=seed, mtu=mtu, fault='')
        def popen(command, **kwargs):
            argv = ([sys.executable, '-c', parent, str(child_pid), str(peer_pid), descendant, mode, PEER, json.dumps(config)]
                    if host.name == 'a' else [sys.executable, '-u', '-c', PEER, json.dumps(config)])
            if mode == 'held-writer' and host.name == 'a':
                # A writer outside the owned group cannot be made to close by
                # killing that group, so reader cancellation must work itself.
                argv = [sys.executable, '-u', '-c', PEER, json.dumps(config)]
                pipes = [os.pipe(), os.pipe()]
                kwargs['stdout'], kwargs['stderr'] = pipes[0][1], pipes[1][1]
                process = subprocess.Popen(argv, **kwargs)
                held_writers.extend(write_fd for _, write_fd in pipes)
                process.stdout = os.fdopen(pipes[0][0], 'r')
                process.stderr = os.fdopen(pipes[1][0], 'r')
                return process
            return subprocess.Popen(argv, **kwargs)
        return runner.Endpoint(host, role, payload, seed, mtu, timeout, results, popen=popen)
    try:
        report = runner.run(hosts, directory / 'results', timeout=.2, endpoint_factory=factory)
        print(json.dumps(report), flush=True)
    finally:
        for descriptor in held_writers:
            os.close(descriptor)



class RunnerTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.directory = Path(self.temp.name) / 'new-results'
        self.hosts = [runner.Host('a', 'cuda.example.invalid', '/opt/check/gpu-verbs-cuda',
                                  'cuda', 'example_rdma0', 0),
                      runner.Host('b', 'vulkan.example.invalid', '/opt/check/gpu-verbs-vulkan',
                                  'vulkan', 'example_rdma1', 1)]
        self.processes = []
        self.roles = []
        self.commands = []

    def tearDown(self):
        for process in self.processes:
            if process.poll() is None:
                process.kill()
                process.wait()
        self.temp.cleanup()

    def factory(self, faults=None, fail_start=None):
        faults = faults or {}
        def start(host, role, payload, seed, mtu, timeout, directory):
            if host.name == fail_start:
                raise OSError('deliberately refused subprocess creation')
            self.roles.append((host.name, role))
            config = dict(role=role, backend=host.backend, payload=payload, seed=seed,
                          mtu=mtu, fault=faults.get(host.name, ''), gpu=host.gpu, device=host.device,
                          port=host.port, gid=host.gid)
            def popen(command, **kwargs):
                self.commands.append(command)
                process = subprocess.Popen([sys.executable, '-u', '-c', PEER,
                                            json.dumps(config)], **kwargs)
                self.processes.append(process)
                return process
            return runner.Endpoint(host, role, payload, seed, mtu, timeout, directory, popen=popen)
        return start

    def execute(self, faults=None, **kwargs):
        result = runner.run(self.hosts, self.directory, timeout=0.4,
                            endpoint_factory=self.factory(faults), **kwargs)
        self.assertTrue(all(process.poll() is not None for process in self.processes))
        self.assertTrue(all(getattr(process, stream).closed for process in self.processes
                            for stream in ('stdin', 'stdout', 'stderr')))
        self.assertEqual(result, json.loads((self.directory / 'manifest.json').read_text()))
        return result

    def test_success_checks_both_roles_and_redacts_capabilities(self):
        report = self.execute(seed=17, payload=1024, mtu=4096)
        self.assertTrue(report['pass'])
        self.assertEqual(report['allocation_checked_bytes'], 24576)
        self.assertFalse(report['throughput_measured'])
        self.assertEqual(self.roles, [('a', 'initiator'), ('b', 'responder')])
        for filename in ('a.stdout.log', 'b.stdout.log'):
            log = (self.directory / filename).read_text()
            self.assertIn('ENDPOINT [redacted]', log)
            self.assertNotIn('13579', log)
            self.assertNotIn('65536', log)
            self.assertNotIn('2001:db8', log)
        self.assertEqual(stat.S_IMODE(self.directory.stat().st_mode), 0o700)
        for path in self.directory.iterdir():
            self.assertEqual(stat.S_IMODE(path.stat().st_mode), 0o600)

    def test_swap_reverses_protocol_roles(self):
        self.assertTrue(self.execute(swap=True)['pass'])
        self.assertEqual(self.roles, [('b', 'initiator'), ('a', 'responder')])

    def test_gpu_mismatch_rejects_successful_protocol_and_exit(self):
        report = self.execute({'a': 'gpu-mismatch'})
        self.assertFalse(report['pass'])
        self.assertIn('GPU verification', report['error'])
        self.assertTrue(all(process.returncode == 0 for process in self.processes))

    def test_mismatched_reported_gpu_and_rdma_identities_are_rejected(self):
        for fault in ('wrong-gpu', 'wrong-device', 'wrong-port', 'wrong-gid'):
            with self.subTest(fault=fault):
                self.directory = Path(self.temp.name) / fault
                result = self.execute({'b': fault})
                self.assertFalse(result['pass'])
                self.assertRegex(result['error'], 'reported (GPU index|RDMA identity)')

    def test_missing_public_wrapper_rejects_standalone_proof(self):
        self.assertFalse(self.execute({'b': 'missing-wrapper'})['pass'])

    def test_missing_gpu_evidence_rejects_success(self):
        self.assertFalse(self.execute({'b': 'missing-gpu'})['pass'])

    def test_missing_cleanup_on_either_peer_rejects_success(self):
        report = self.execute({'b': 'missing-cleanup'})
        self.assertFalse(report['pass'])
        self.assertIn('cleanup', report['error'])

    def test_corrupted_result_count_stops_both_processes(self):
        report = self.execute({'a': 'wrong-count'})
        self.assertFalse(report['pass'])
        self.assertIn('NATIVE_FORWARD', report['error'])

    def test_negative_control_retains_mismatch_and_fails(self):
        report = self.execute({'b': 'negative'}, seed=17, peer_seed=18)
        self.assertFalse(report['pass'])
        self.assertIn('phase=received-forward checked_bytes=24576 errors=4096',
                      (self.directory / 'b.stderr.log').read_text())

    def test_nonzero_exit_overrules_complete_markers(self):
        report = self.execute({'b': 'nonzero'})
        self.assertFalse(report['pass'])
        self.assertIn('code 7', report['error'])

    def test_unexpected_trailing_stdout_rejects_success(self):
        self.assertFalse(self.execute({'a': 'extra-protocol'})['pass'])

    def test_unterminated_trailing_output_is_still_validated_on_normal_exit(self):
        report = self.execute({'a': 'unterminated-extra'})
        self.assertFalse(report['pass'])
        self.assertIn('trailing protocol output', report['error'])

    def test_explicit_nondefault_gpu_port_and_gid_are_accepted(self):
        self.hosts[1] = runner.Host('b', 'vulkan.example.invalid', '/opt/check/gpu-verbs-vulkan',
                                    'vulkan', 'example_rdma1', 7, gpu=3, port=2)
        self.assertTrue(self.execute()['pass'])

    def test_error_diagnostic_overrules_complete_markers(self):
        self.assertFalse(self.execute({'b': 'reported-error'})['pass'])

    def test_timeout_is_bounded_and_preserves_failure_manifest(self):
        start = time.monotonic()
        report = self.execute({'a': 'timeout'})
        self.assertFalse(report['pass'])
        self.assertIn('timed out', report['error'])
        self.assertLess(time.monotonic() - start, 5)

    def test_inherited_pipes_do_not_block_cleanup_or_leave_descendant(self):
        for mode in ('exit', 'timeout', 'complete', 'held-writer'):
            with self.subTest(mode=mode):
                case = Path(self.temp.name) / mode
                case.mkdir()
                process = subprocess.Popen([sys.executable, str(Path(__file__).resolve()),
                                            '--inherited-pipe-case', str(case), mode],
                                           stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                           text=True, start_new_session=True)
                started = time.monotonic()
                try:
                    try:
                        stdout, stderr = process.communicate(timeout=4)
                    except subprocess.TimeoutExpired:
                        self.fail('runner cleanup blocked on a pipe inherited by a descendant')
                    self.assertEqual(process.returncode, 0, stderr)
                    self.assertFalse(json.loads(stdout)['pass'])
                    self.assertLess(time.monotonic() - started, 4)
                    if mode == 'held-writer':
                        continue
                    pid = int((case / 'descendant.pid').read_text())
                    deadline = time.monotonic() + 1
                    while True:
                        state = subprocess.run(['ps', '-o', 'stat=', '-p', str(pid)],
                                               capture_output=True, text=True).stdout.strip()
                        if not state or state.startswith('Z'):
                            break
                        if time.monotonic() >= deadline:
                            self.fail('cleanup left its pipe-holding descendant running')
                        time.sleep(.01)
                finally:
                    # This also bounds the regression on the old implementation.
                    for pidfile in ('descendant.pid', 'peer.pid'):
                        if (case / pidfile).exists():
                            pid = int((case / pidfile).read_text())
                            try:
                                os.kill(pid, signal.SIGKILL)
                            except ProcessLookupError:
                                pass
                    try:
                        os.killpg(process.pid, signal.SIGKILL)
                    except ProcessLookupError:
                        pass
                    process.communicate(timeout=2)

    def test_malformed_descriptor_does_not_leak_received_fields(self):
        report = self.execute({'a': 'malformed'})
        self.assertFalse(report['pass'])
        self.assertNotIn('garbage', report['error'])
        self.assertNotIn('garbage', (self.directory / 'a.stdout.log').read_text())

    def test_output_limit_stops_unbounded_peer(self):
        report = self.execute({'a': 'oversized'})
        self.assertFalse(report['pass'])
        self.assertLess((self.directory / 'a.stdout.log').stat().st_size, 4096)

    def test_partial_start_failure_stops_first_endpoint(self):
        report = runner.run(self.hosts, self.directory, timeout=0.4,
                            endpoint_factory=self.factory(fail_start='b'))
        self.assertFalse(report['pass'])
        self.assertEqual(report['error'], 'OSError')
        self.assertTrue(all(process.poll() is not None for process in self.processes))

    def test_existing_results_are_never_overwritten(self):
        self.directory.mkdir()
        evidence = self.directory / 'manifest.json'
        evidence.write_text('prior evidence')
        with self.assertRaises(FileExistsError):
            runner.run(self.hosts, self.directory, endpoint_factory=self.factory())
        self.assertEqual(evidence.read_text(), 'prior evidence')
        self.assertEqual(self.processes, [])

    def test_descriptor_rejects_invalid_region_ranges_and_protocol_fields(self):
        for line in ('ENDPOINT 0 1 1 65536 16384 2001:db8::1',
                     'ENDPOINT 1 1 4294967296 65536 16384 2001:db8::1',
                     'ENDPOINT 1 1 1 18446744073709551615 16384 2001:db8::1',
                     'ENDPOINT 1 1 1 65536 4096 2001:db8::1',
                     'ENDPOINT 1 1 1 65536 16384 ::',
                     'ENDPOINT 1 1 1 65536 16384 2001:db8::1 extra'):
            with self.subTest(line=line), self.assertRaises(runner.ValidationError):
                runner.descriptor(line)

    def test_ssh_command_keeps_shell_metacharacters_in_arguments(self):
        host = runner.Host('a', 'example.invalid', '/opt/test $literal/check;echo unwanted',
                           'cuda', 'rdma0', 4, key='/private/example key',
                           jump='jump.example.invalid', shader='/opt/shaders/a b.spv')
        command = host.command('initiator', 4096, 1, 1024, 90)
        remote = shlex.split(command[-1])
        self.assertEqual(command[-2], 'example.invalid')
        self.assertIn(host.executable, remote)
        self.assertIn('MCDMA_VULKAN_SHADER=/opt/shaders/a b.spv', remote)
        self.assertEqual(remote[:4], ['exec', 'timeout', '--kill-after=5s', '90s'])
        self.assertEqual(command[command.index('-i') + 1], '/private/example key')
        self.assertNotIn('echo', remote)

    def test_explicit_private_ssh_config_is_validated_and_passed(self):
        path = Path(self.temp.name) / 'private ssh config'
        path.write_text('Host example.invalid\n    BatchMode yes\n')
        resolved = runner.ssh_config_file(str(path))
        host = runner.Host('a', 'example.invalid', '/opt/check/peer', 'cuda', 'rdma0', 0,
                           jump='jump.example.invalid', ssh_config=resolved)
        command = host.command('initiator', 4096, 17, 1024, 90)
        self.assertEqual(command[command.index('-F') + 1], str(path.resolve()))
        self.assertEqual(command[command.index('-J') + 1], 'jump.example.invalid')
        self.assertNotIn(str(path), command[-1])
        for invalid in (str(path.parent), str(path.parent / 'absent')):
            with self.subTest(invalid=invalid), self.assertRaises(argparse.ArgumentTypeError):
                runner.ssh_config_file(invalid)

    def test_named_capabilities_are_redacted(self):
        self.assertEqual(runner.redact('rkey=123 address=0xabc remote_addr:65536'),
                         'rkey=[redacted] address=[redacted] remote_addr=[redacted]')


if __name__ == '__main__':
    if len(sys.argv) == 4 and sys.argv[1] == '--inherited-pipe-case':
        inherited_pipe_case(sys.argv[2], sys.argv[3])
    else:
        unittest.main()
