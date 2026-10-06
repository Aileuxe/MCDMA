"""Offline tests of the shared link layer: the link-local exchange, the Thunderbolt placement protocol, mcdma-rpcd over
Thunderbolt and libmcdma-fabric, each built against the strict stub verbs in tests/stub_link_verbs.c. Nothing here
opens a real device except the verbs smoke test, which only lists devices and runs when MCDMA_VERBS_SMOKE=1."""
import os
import pathlib
import platform
import shutil
import subprocess
import sys
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
LINK = ['rpc/link_verbs.c', 'rpc/link_tb.c', 'rpc/link_xchg.c']
DAEMON = ['rpc/rpcd_common.c', 'rpc/rpcd_verbs.c', 'rpc/rpcd_listen.c', 'rpc/rpcd_connect.c']
STUB = ['tests/stub_link_verbs.c']
SKIP = 77
# a failing sanitizer or stub exits with a status instead of aborting, so no crash report is raised
QUIET = {'ASAN_OPTIONS': 'abort_on_error=0:detect_leaks=0', 'UBSAN_OPTIONS': 'halt_on_error=1:abort_on_error=0',
         'TSAN_OPTIONS': 'halt_on_error=1:abort_on_error=0:exitcode=66', 'MCDMA_FABRIC_LOG': '0'}
BASE = ['cc', '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-DMCDMA_LINK_TEST_INTERFACES']
FABRIC = ['-DMCDMA_FABRIC_TEST_DMABUF', '-DMCDMA_FABRIC_TESTING', 'rpc/libmcdma_fabric.c', *LINK, *STUB,
          'tests/test_fabric.c']


def sanitizer(work, kinds):
    probe = pathlib.Path(work) / 'probe.c'
    probe.write_text('int main(void) { return 0; }\n')
    flags = [*BASE, f'-fsanitize={kinds}']
    built = subprocess.run([*flags, str(probe), '-o', str(probe.with_suffix(''))], capture_output=True)
    return flags if built.returncode == 0 else None


def compiler_flags(work):
    return sanitizer(work, 'address,undefined') or BASE


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
            'fabric': FABRIC,
            'fabric-check': ['-DFABRIC_CHECK_NO_MAIN', 'rpc/fabric_check.c', 'rpc/libmcdma_fabric.c', *LINK, *STUB,
                             'tests/test_fabric_check.c'],
            'mesh-check': ['-DMESH_CHECK_NO_MAIN', 'rpc/mesh_check.c', 'rpc/libmcdma_fabric.c', *LINK, *STUB,
                           'tests/test_mesh_check.c'],
        }
        for name, sources in builds.items():
            subprocess.run([*flags, *sources, '-lpthread', '-o', os.path.join(cls.work, name)], cwd=ROOT, check=True)
        # the bond's progress threads, signal queue and tail queues under ThreadSanitizer, where the compiler has it
        cls.threads = sanitizer(cls.work, 'thread')
        if cls.threads:
            subprocess.run([*cls.threads, *FABRIC, '-lpthread', '-o', os.path.join(cls.work, 'fabric-tsan')], cwd=ROOT,
                           check=True)

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.work, ignore_errors=True)

    def run_case(self, binary, *args, timeout=180, **env):
        done = subprocess.run([os.path.join(self.work, binary), *args], capture_output=True, text=True,
                              timeout=timeout, env={**os.environ, **QUIET, **env})
        if done.returncode == SKIP:
            self.skipTest('the loopback interface has no link-local address')
        self.assertEqual(done.returncode, 0, (done.stdout + done.stderr)[-3000:])
        return done

    def library_lines(self, done):
        return [line for line in done.stderr.splitlines() if line.startswith('mcdma-fabric: ')]

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

    def test_bonded_fabric_uses_both_thunderbolt_links(self):
        for seed, depth in (('5', '4095'), ('6', '8')):
            with self.subTest(seed=seed, depth=depth):
                self.run_case('fabric', 'bond', STUB_SEED=seed, STUB_TB_DEPTH=depth, STUB_STRICT='1', STUB_LAZY='1')

    def test_bond_device_and_via_lists_are_strict(self):
        self.run_case('fabric', 'bond-args', STUB_STRICT='1')

    def test_a_single_link_peer_cannot_satisfy_a_bond(self):
        self.run_case('fabric', 'bond-mismatch', STUB_STRICT='1')

    def test_bond_signals_wait_for_both_receive_placements(self):
        for seed in ('1', '4'):
            with self.subTest(seed=seed):
                self.run_case('fabric', 'bond-order', STUB_SEED=seed, STUB_STRICT='1', STUB_LAZY='1')

    def test_bond_keeps_using_the_faster_link_when_one_is_delayed(self):
        self.run_case('fabric', 'bond-schedule', STUB_SEED='5', STUB_STRICT='1', STUB_LAZY='1')

    def test_bond_overlapping_calls_preserve_posting_order(self):
        self.run_case('fabric', 'bond-overlap', STUB_SEED='3', STUB_STRICT='1', STUB_LAZY='1')

    def test_one_failed_link_poisons_the_whole_bond(self):
        self.run_case('fabric', 'bond-fail', STUB_SEED='2', STUB_STRICT='1', STUB_LAZY='1')

    def test_bond_signal_credit_backpressures_instead_of_poisoning(self):
        self.run_case('fabric', 'bond-credit', STUB_SEED='4', STUB_STRICT='1', STUB_LAZY='1')

    def test_bond_small_writes_stay_whole_at_tiny_depth_and_registration_boundaries(self):
        for depth in ('4095', '4'):
            with self.subTest(depth=depth):
                self.run_case('fabric', 'bond-small', STUB_TB_DEPTH=depth, STUB_SEED='3',
                              STUB_STRICT='1', STUB_LAZY='1')

    def test_a_dead_bond_peer_does_not_fail_a_live_peers_flag_wait(self):
        self.run_case('fabric', 'bond-wait', STUB_SEED='6', STUB_STRICT='1', STUB_LAZY='1')

    def test_public_maximum_names_fit_bond_lane_suffixes(self):
        self.run_case('fabric', 'bond-names', STUB_STRICT='1')

    def test_receives_go_back_after_a_reply_has_posted(self):
        for seed, lazy in (('1', {'STUB_LAZY': '1'}), ('2', {})):
            with self.subTest(seed=seed, lazy=bool(lazy)):
                self.run_case('link-tb', 'repost', STUB_SEED=seed, **lazy)

    def test_bond_write_signals_are_cut_one_message_a_link(self):
        self.run_case('link-tb', 'tail', STUB_SEED='2', STUB_LAZY='1')
        self.run_case('fabric', 'bond-plan')
        for seed, depth in (('1', None), ('4', None), ('2', '8')):
            with self.subTest(seed=seed, depth=depth):
                depth_env = {'STUB_TB_DEPTH': depth} if depth else {}
                self.run_case('fabric', 'bond-stripe', STUB_SEED=seed, STUB_STRICT='1', STUB_LAZY='1', **depth_env)
        with self.subTest(fallback='a tail its link thread never takes is posted by the sender'):
            self.run_case('fabric', 'bond-stripe', STUB_SEED='3', STUB_STRICT='1', STUB_LAZY='1', BOND_ASLEEP='1')

    def test_bond_carries_tensor_parallel_exchanges_on_both_links(self):
        for seed, lazy in (('1', {'STUB_LAZY': '1'}), ('3', {'STUB_LAZY': '1'}), ('5', {})):
            with self.subTest(seed=seed, lazy=bool(lazy)):
                self.run_case('fabric', 'bond-pingpong', STUB_SEED=seed, STUB_STRICT='1', **lazy)

    def test_bond_later_cut_writes_win(self):
        for seed in ('2', '7'):
            with self.subTest(seed=seed):
                self.run_case('fabric', 'bond-later', STUB_SEED=seed, STUB_STRICT='1', STUB_LAZY='1')

    def test_bond_tails_never_start_with_a_head(self):
        self.run_case('fabric', 'bond-magic', STUB_SEED='3', STUB_STRICT='1', STUB_LAZY='1')

    def test_bond_gives_a_slower_link_less(self):
        self.run_case('fabric', 'bond-uneven', STUB_SEED='6', STUB_STRICT='1', STUB_LAZY='1')

    def test_bond_threads_are_race_free(self):
        if not self.threads:
            self.skipTest('the compiler has no ThreadSanitizer')
        for scenario in ('bond', 'bond-order', 'bond-overlap', 'bond-fail', 'bond-credit', 'bond-stripe',
                         'bond-pingpong', 'bond-later', 'bond-magic', 'bond-down', 'down', 'zero'):
            with self.subTest(scenario=scenario):
                self.run_case('fabric-tsan', scenario, STUB_SEED='4', STUB_STRICT='1', STUB_LAZY='1', timeout=300)
        with self.subTest(scenario='bond-stripe, sender fallback'):
            self.run_case('fabric-tsan', 'bond-stripe', STUB_SEED='4', STUB_STRICT='1', STUB_LAZY='1', BOND_ASLEEP='1',
                          timeout=300)

    def test_a_zero_length_write_signal_is_a_signal(self):
        done = self.run_case('fabric', 'zero', STUB_SEED='5', STUB_STRICT='1', STUB_LAZY='1', MCDMA_FABRIC_LOG='1')
        # nothing fails, so the only lines are the goodbyes of the closing pairs
        self.assertEqual([line for line in self.library_lines(done) if 'said goodbye' not in line], [])

    def test_a_failed_end_tells_the_other_why_in_one_line_each(self):
        for scenario, link in (('down', 'tb1'), ('bond-down', 'tb3')):
            with self.subTest(scenario=scenario):
                done = self.run_case('fabric', scenario, STUB_SEED='5', STUB_STRICT='1', STUB_LAZY='1',
                                     MCDMA_FABRIC_LOG='1')
                failed = [line for line in self.library_lines(done) if 'thunderbolt link failed' in line]
                heard = [line for line in self.library_lines(done) if 'peer down: the other end failed' in line]
                self.assertEqual(len(failed), 1, done.stderr)
                self.assertTrue(failed[0].startswith(f'mcdma-fabric: {link}: '), done.stderr)
                self.assertEqual(len(heard), 1, done.stderr)
                self.assertIn(f'{link}: an unexpected completion', heard[0])

    def test_fabric_check_passes_on_both_link_kinds(self):
        self.run_case('fabric-check', 'tb', STUB_SEED='3', STUB_LAZY='1')
        self.run_case('fabric-check', 'roce', STUB_STRICT='1')

    def test_the_dual_pipe_qualifier_parses_and_gates_offline(self):
        done = subprocess.run([sys.executable, '-B', 'tests/dual_pipe_qualify.py', '--self-test'], cwd=ROOT,
                              capture_output=True, text=True, timeout=120)
        self.assertEqual(done.returncode, 0, (done.stdout + done.stderr)[-3000:])

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
