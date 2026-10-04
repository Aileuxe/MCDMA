"""Offline tests of the shared link layer: the link-local exchange, the Thunderbolt placement protocol, mcdma-rpcd over
Thunderbolt and libmcdma-fabric, each built against the strict stub verbs in tests/stub_link_verbs.c. Nothing here
opens a real device except the verbs smoke test, which only lists devices and runs when MCDMA_VERBS_SMOKE=1."""
import os
import pathlib
import platform
import shutil
import subprocess
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
LINK = ['rpc/link_verbs.c', 'rpc/link_tb.c', 'rpc/link_xchg.c']
DAEMON = ['rpc/rpcd_common.c', 'rpc/rpcd_verbs.c', 'rpc/rpcd_listen.c', 'rpc/rpcd_connect.c']
STUB = ['tests/stub_link_verbs.c']
SKIP = 77
# a failing sanitizer or stub exits with a status instead of aborting, so no crash report is raised
QUIET = {'ASAN_OPTIONS': 'abort_on_error=0:detect_leaks=0', 'UBSAN_OPTIONS': 'halt_on_error=1:abort_on_error=0',
         'MCDMA_FABRIC_LOG': '0'}


def compiler_flags(work):
    flags = ['cc', '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-DMCDMA_LINK_TEST_INTERFACES']
    probe = pathlib.Path(work) / 'probe.c'
    probe.write_text('int main(void) { return 0; }\n')
    sanitized = [*flags, '-fsanitize=address,undefined']
    if subprocess.run([*sanitized, str(probe), '-o', str(probe.with_suffix(''))], capture_output=True).returncode == 0:
        return sanitized
    return flags


@unittest.skipUnless(shutil.which('cc'), 'C compiler required')
class LinkTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.work = tempfile.mkdtemp(prefix='mcdma-link-', dir='/tmp')
        flags = compiler_flags(cls.work)
        dirs = [f'-DMCDMA_RPC_BOX_DIR="{cls.work}"', f'-DMCDMA_RPC_LOCK_DIR="{cls.work}"',
                f'-DMCDMA_RPC_SOCK_DIR="{cls.work}"', '-DMCDMA_RPC_LISTEN_LOCK=".listen"',
                '-DMCDMA_RPC_HANDSHAKE_S=2', '-DMCDMA_RPC_LIVENESS_S=3']
        builds = {
            'link-tb': [*LINK, *STUB, 'tests/test_link_tb.c'],
            'link-xchg': [*LINK, *STUB, 'tests/test_link_xchg.c'],
            'rpcd-tb': [*dirs, '-DRPC_ECHO_NO_MAIN', *DAEMON, *LINK, *STUB, 'rpc/rpc_echo.c', 'tests/test_rpcd_tb.c'],
            'fabric': ['-DMCDMA_FABRIC_TEST_DMABUF', 'rpc/libmcdma_fabric.c', *LINK, *STUB, 'tests/test_fabric.c'],
            'fabric-check': ['-DFABRIC_CHECK_NO_MAIN', 'rpc/fabric_check.c', 'rpc/libmcdma_fabric.c', *LINK, *STUB,
                             'tests/test_fabric_check.c'],
            'mesh-check': ['-DMESH_CHECK_NO_MAIN', 'rpc/mesh_check.c', 'rpc/libmcdma_fabric.c', *LINK, *STUB,
                           'tests/test_mesh_check.c'],
        }
        for name, sources in builds.items():
            subprocess.run([*flags, *sources, '-lpthread', '-o', os.path.join(cls.work, name)], cwd=ROOT, check=True)

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.work, ignore_errors=True)

    def run_case(self, binary, *args, timeout=180, **env):
        done = subprocess.run([os.path.join(self.work, binary), *args], capture_output=True, text=True,
                              timeout=timeout, env=dict(os.environ, **QUIET, **env))
        if done.returncode == SKIP:
            self.skipTest('the loopback interface has no link-local address')
        self.assertEqual(done.returncode, 0, (done.stdout + done.stderr)[-3000:])

    def test_the_exchange_admits_only_on_link_peers(self):
        self.run_case('link-xchg')

    def test_signals_land_after_the_writes_before_them(self):
        for seed in ('1', '2', '3'):
            for lazy in ({}, {'STUB_LAZY': '1'}):
                with self.subTest(seed=seed, lazy=bool(lazy)):
                    self.run_case('link-tb', 'order', STUB_SEED=seed, **lazy)

    def test_a_write_and_its_signal_as_one_message_land_in_order(self):
        for seed in ('1', '2'):
            for lazy in ({}, {'STUB_LAZY': '1'}):
                with self.subTest(seed=seed, lazy=bool(lazy)):
                    self.run_case('link-tb', 'joined', STUB_SEED=seed, **lazy)

    def test_overlapping_writes_land_in_posting_order(self):
        for seed in ('1', '4'):
            with self.subTest(seed=seed):
                self.run_case('link-tb', 'overlap', STUB_SEED=seed, STUB_LAZY='1')

    def test_random_traffic_both_ways_matches_a_mirror(self):
        for seed, depth in (('1', '4095'), ('2', '4095'), ('3', '64'), ('5', '8')):
            with self.subTest(seed=seed, depth=depth):
                self.run_case('link-tb', 'random', STUB_SEED=seed, STUB_TB_DEPTH=depth)

    def test_a_write_outside_the_accepted_range_fails_the_link(self):
        self.run_case('link-tb', 'bounds')

    def test_sends_waiting_for_credit_are_torn_down_cleanly(self):
        self.run_case('link-tb', 'teardown')

    def test_daemon_calls_round_trip_over_thunderbolt(self):
        for seed, lazy in (('1', {}), ('2', {'STUB_LAZY': '1'})):
            with self.subTest(seed=seed):
                self.run_case('rpcd-tb', STUB_SEED=seed, **lazy)

    def test_fabric_over_thunderbolt(self):
        for seed in ('5', '6'):
            with self.subTest(seed=seed):
                self.run_case('fabric', 'tb', STUB_SEED=seed, STUB_STRICT='1', STUB_LAZY='1')

    def test_fabric_over_roce_with_every_key_checked(self):
        self.run_case('fabric', 'roce', STUB_STRICT='1')

    def test_fabric_registers_dma_bufs_without_fallback(self):
        self.run_case('fabric', 'dmabuf')

    def test_fabric_ranks_on_different_links_refuse_each_other(self):
        self.run_case('fabric', 'mismatch')

    def test_fabric_refuses_bad_arguments(self):
        self.run_case('fabric', 'args')

    def test_fabric_check_passes_on_both_link_kinds(self):
        self.run_case('fabric-check', 'tb', STUB_SEED='3', STUB_LAZY='1')
        self.run_case('fabric-check', 'roce', STUB_STRICT='1')

    def test_four_node_mesh_all_reduces_in_node_order(self):
        self.run_case('mesh-check', '1,16,300', '1', STUB_SEED='4', timeout=600)
        self.run_case('mesh-check', '1,4', '2', STUB_SEED='7', STUB_LAZY='1', timeout=600)

    @unittest.skipUnless(os.environ.get('MCDMA_VERBS_SMOKE') == '1', 'set MCDMA_VERBS_SMOKE=1 to list real devices')
    def test_verbs_smoke_lists_real_devices(self):
        library = '-lrdma' if platform.system() == 'Darwin' else '-libverbs'
        binary = os.path.join(self.work, 'verbs-smoke')
        subprocess.run(['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror', 'tests/verbs_smoke.c', *LINK, library,
                        '-lpthread', '-o', binary], cwd=ROOT, check=True)
        done = subprocess.run([binary], capture_output=True, text=True, timeout=30)
        self.assertEqual(done.returncode, 0, done.stderr)
        print('\n' + done.stdout.strip())


if __name__ == '__main__':
    unittest.main()
