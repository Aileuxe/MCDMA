#!/usr/bin/env python3
"""Review-only by default: print the two-host qualification plan; --execute runs it over SSH.

It measures the bond the way a tensor-parallel engine uses it: ping-pong write_signals of 64 bytes, 10, 40 and 160 KB
and 1 MiB, each rank spinning on its window's flag word instead of waiting in the library, progress threads at
user-interactive QoS (MCDMA_FABRIC_QOS=1), every byte checked; then a byte-checked stream. Each cable alone and the
bond, with each host as rank 0 in turn.

No addresses or host identities are built in. Logs contain the supplied private configuration and belong in results/.
This tool does not install software, configure interfaces, enable RDMA or run a GPU workload.
"""
import argparse
import dataclasses
import datetime
import json
import math
import pathlib
import re
import shlex
import subprocess
import sys
import time


SIZES = (64, 10240, 40960, 163840, 1048576)
SMALL = (64, 10240)                 # the bond may not be slower than the better cable by more than 1 us at p50
LARGE = (40960, 163840, 1048576)    # the bond must be faster than the better cable at p50
TOP = 1048576                       # and carry near two cables' rate at the largest size
SLACK_US = 1.0
TOP_HEALTHY = 1.6                   # times the faster cable's GB/s
TOP_DEGRADED = 0.85                 # times the two cables' GB/s added
STREAM_HEALTHY = 1.8                # times the faster cable's stream
STREAM_DEGRADED = 0.9               # times the two cables' streams added
P99_RATIO, P99_SLACK_US = 1.25, 2.0


@dataclasses.dataclass(frozen=True)
class Endpoint:
    host: str
    program: str
    devices: tuple
    interfaces: tuple
    addresses: tuple
    ssh: tuple = ()


@dataclasses.dataclass(frozen=True)
class Case:
    direction: str
    link: str
    seconds: float

    @property
    def label(self):
        return f'{self.direction}-{self.link}'


def ssh_command(endpoint, remote):
    options = [arg for option in endpoint.ssh for arg in ('-o', option)]
    return ['ssh', '-o', 'BatchMode=yes', '-o', 'ServerAliveInterval=5', *options, '--', endpoint.host, remote]


def commands(case, a, b, gid, port, rounds):
    selected = (0, 1) if case.link == 'bond' else (int(case.link[-1]) - 1,)
    sender, receiver = (a, b) if case.direction == 'a-to-b' else (b, a)
    result = []
    for rank, local, remote in ((0, sender, receiver), (1, receiver, sender)):
        device = '+'.join(local.devices[i] for i in selected)
        via = '+'.join(f'{local.interfaces[i]}/{remote.addresses[i]}' for i in selected)
        remote_argv = ['env', 'MCDMA_FABRIC_QOS=1', local.program, device, str(gid), via, str(port), 'qualify',
                       str(rank), str(rounds), ','.join(map(str, SIZES)), str(case.seconds), '0', 'combined', '1',
                       'spin']
        result.append(ssh_command(local, 'exec ' + shlex.join(remote_argv)))
    return result


def plan(seconds):
    # Each cable alone, then the bond, with host a as rank 0 and then host b.
    return [Case(direction, link, seconds) for direction in ('a-to-b', 'b-to-a') for link in ('link1', 'link2', 'bond')]


def fields(line):
    entries = [part.split('=', 1) for part in line.split()]
    if any(len(part) != 2 or not part[0] or not part[1] for part in entries):
        raise ValueError('malformed checker fields')
    result = dict(entries)
    if len(result) != len(entries):
        raise ValueError('duplicate checker fields')
    return result


def number(row, key):
    value = float(row.get(key, 'nan'))
    if not math.isfinite(value) or value <= 0:
        raise ValueError(f'missing or invalid {key}')
    return value


def parse_pair(sender, receiver, case, rounds, links):
    expected = {'abi': '1', 'links': str(links), 'mode': 'combined', 'progress': '1', 'qos': '1', 'wait': 'spin',
                'verification': 'every_byte'}
    precision_seconds = 0.000500001   # seconds are printed to three decimals
    builds = set()
    for rank, text in enumerate((sender, receiver)):
        if re.findall(rf'^fabric-check: rank {rank} (PASS|FAIL)$', text, re.M) != ['PASS']:
            raise ValueError(f'rank {rank} did not report PASS')
        for wrong in re.findall(r'(?:^|\s)wrong_words=(\S+)', text):
            if int(wrong) != 0:
                raise ValueError(f'rank {rank} reported corrupt payload')
        metadata = re.findall(r'^fabric-check: metadata (.+)$', text, re.M)
        if len(metadata) != 1:
            raise ValueError(f'rank {rank} lacks unique configuration metadata')
        metadata = fields(metadata[0])
        if any(metadata.get(key) != value for key, value in expected.items()):
            raise ValueError(f'rank {rank} metadata differs from its requested configuration')
        duration = float(metadata.get('stream_seconds', 'nan'))
        if not math.isfinite(duration) or abs(duration - case.seconds) > precision_seconds:
            raise ValueError(f'rank {rank} requested stream duration differs')
        builds.add(metadata.get('build', 'unknown'))
    if len(builds) != 1 or 'unknown' in builds:
        raise ValueError('the two ranks do not run the same make-built fabric-check')
    result = {'build': builds.pop(), 'p50_us': {}, 'p99_us': {}, 'gb_s': {}}
    for rank, text in enumerate((sender, receiver)):
        sizes = set()
        for row in re.findall(r'^fabric-check: (size=.+)$', text, re.M):
            row = fields(row)
            size = int(row.get('size', '0'))
            if size in sizes or int(row.get('rounds', '0')) != rounds or row.get('wrong_words') != '0':
                raise ValueError(f'rank {rank} has duplicate sizes or incorrect rounds/results')
            sizes.add(size)
            if rank == 0:
                result['p50_us'][size] = number(row, 'rtt_p50_us')
                result['p99_us'][size] = number(row, 'rtt_p99_us')
                result['gb_s'][size] = number(row, 'gb_s')
        if sizes != set(SIZES):
            raise ValueError(f'rank {rank} lacks its requested ping-pong sizes')
    if not case.seconds:
        return result
    match = re.search(r'^fabric-check: stream bytes=(\d+) seconds=([\d.]+) gbit_s=([\d.]+) '
                      r'checked_bytes=(\d+) gb_s=([\d.]+)$', sender, re.M)
    received = re.search(r'^fabric-check: stream checked_bytes=(\d+) wrong_words=0$', receiver, re.M)
    if not match or not received:
        raise ValueError('missing byte-checked stream result')
    byte_count, elapsed, reported_rate, checked = int(match[1]), float(match[2]), float(match[3]), int(match[4])
    if not 64 << 20 <= byte_count <= (1 << 64) - 1 or byte_count % (4 << 20):
        raise ValueError('stream bytes must contain at least two complete eight-slot cycles')
    if checked != byte_count or int(received[1]) != byte_count:
        raise ValueError('transferred and checked byte counts differ')
    if not math.isfinite(elapsed) or elapsed <= precision_seconds or elapsed + precision_seconds < case.seconds:
        raise ValueError('stream was shorter than requested or has no positive rate')
    # The elapsed field and reported rate have independent rounding. Their possible true-rate intervals must meet.
    low = byte_count * 8 / (elapsed + precision_seconds) / 1e9
    high = byte_count * 8 / (elapsed - precision_seconds) / 1e9
    if not math.isfinite(reported_rate) or reported_rate <= 0 or reported_rate + 0.005000001 < low or \
            reported_rate - 0.005000001 > high:
        raise ValueError('printed stream rate is inconsistent with transferred bytes and elapsed time')
    rate = byte_count * 8 / elapsed / 1e9
    counters = {}
    for m in re.finditer(r'^fabric-check: stream link=(\d+) posted_bytes=(\d+) completed_bytes=(\d+)$', sender, re.M):
        index, posted, completed = map(int, m.groups())
        if index in counters or not posted or posted != completed:
            raise ValueError('link counters are duplicated, idle or incomplete')
        counters[index] = {'posted_bytes': posted, 'completed_bytes': completed}
    if set(counters) != set(range(links)) or sum(v['posted_bytes'] for v in counters.values()) != byte_count:
        raise ValueError('link payload counters do not sum to the byte-checked stream')
    result.update(bytes=byte_count, seconds=elapsed, gbit_s=rate, stream_gb_s=rate / 8, reported_gbit_s=reported_rate,
                  links=counters)
    return result


def acceptance(results, condition):
    checks = []
    healthy = condition == 'healthy'
    for direction in ('a-to-b', 'b-to-a'):
        singles = [results[Case(direction, link, results['seconds']).label] for link in ('link1', 'link2')]
        bonded = results[Case(direction, 'bond', results['seconds']).label]
        if results['seconds']:
            rates = [s['gbit_s'] for s in singles]
            required = STREAM_HEALTHY * max(rates) if healthy else STREAM_DEGRADED * sum(rates)
            checks.append({'direction': direction, 'check': 'stream GB/s', 'actual': bonded['gbit_s'] / 8,
                           'required': required / 8, 'pass': bonded['gbit_s'] >= required})
        for size in SIZES:
            best = min(s['p50_us'][size] for s in singles)
            limit = best + SLACK_US if size in SMALL else best
            ok = bonded['p50_us'][size] <= limit if size in SMALL else bonded['p50_us'][size] < limit
            checks.append({'direction': direction, 'check': 'p50 us', 'size': size, 'actual': bonded['p50_us'][size],
                           'maximum' if size in SMALL else 'below': limit, 'pass': ok})
            ceiling = P99_RATIO * min(s['p99_us'][size] for s in singles) + P99_SLACK_US
            checks.append({'direction': direction, 'check': 'p99 us', 'size': size, 'actual': bonded['p99_us'][size],
                           'maximum': ceiling, 'pass': bonded['p99_us'][size] <= ceiling})
        rates = [s['gb_s'][TOP] for s in singles]
        required = TOP_HEALTHY * max(rates) if healthy else TOP_DEGRADED * sum(rates)
        checks.append({'direction': direction, 'check': 'ping-pong GB/s', 'size': TOP, 'actual': bonded['gb_s'][TOP],
                       'required': required, 'pass': bonded['gb_s'][TOP] >= required})
    return checks


def table(results):
    lines = []
    for direction in ('a-to-b', 'b-to-a'):
        lines.append(f'{direction}: p50 / p99 us, GB/s   ' + '   '.join(f'{link:>24}' for link in ('link1', 'link2', 'bond')))
        for size in SIZES:
            cells = []
            for link in ('link1', 'link2', 'bond'):
                r = results[Case(direction, link, results['seconds']).label]
                cells.append(f'{r["p50_us"][size]:8.2f} / {r["p99_us"][size]:8.2f}, {r["gb_s"][size]:6.2f}')
            lines.append(f'  {size:>9} B                  ' + '   '.join(f'{c:>24}' for c in cells))
        if results['seconds']:
            cells = [f'{results[Case(direction, link, results["seconds"]).label]["stream_gb_s"]:.2f} GB/s'
                     for link in ('link1', 'link2', 'bond')]
            lines.append('  stream                       ' + '   '.join(f'{c:>24}' for c in cells))
    return '\n'.join(lines)


def run_pair(cmds, output, timeout):
    processes, files = [], []
    deadline = time.monotonic() + timeout
    try:
        for rank, cmd in enumerate(cmds):
            out = (output / f'rank{rank}.stdout').open('w')
            err = (output / f'rank{rank}.stderr').open('w')
            files.extend((out, err))
            processes.append(subprocess.Popen(cmd, stdin=subprocess.DEVNULL, stdout=out, stderr=err))
        for process in processes:
            status = process.wait(timeout=max(0.1, deadline - time.monotonic()))
            if status:
                raise RuntimeError(f'fabric-check/SSH exited {status}; inspect this case\'s logs')
    finally:
        for process in processes:
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    # Leave it to the owner rather than SIGKILL a live RDMA process.
                    print('SSH did not stop; inspect the remote checker before another run', file=sys.stderr)
        for f in files:
            f.close()
    return tuple((output / f'rank{rank}.stdout').read_text() for rank in (0, 1))


def leftover(endpoint):
    """A fabric-check still running from an earlier case would hold the ports and the devices."""
    done = subprocess.run(ssh_command(endpoint, 'pgrep -x fabric-check'), stdin=subprocess.DEVNULL,
                          capture_output=True, text=True, timeout=60)
    if done.returncode not in (0, 1):
        raise RuntimeError(f'cannot check {endpoint.host} for a running fabric-check (ssh exited {done.returncode})')
    return done.returncode == 0


def self_test():
    def fixture(case, links, p50=None, gb=None, build='4242'):
        count, elapsed = 64 << 20, case.seconds + 0.001
        metadata = (f'fabric-check: metadata abi=1 device=x via=y gid_index=1 links={links} mode=combined progress=1 '
                    f'qos=1 wait_poll=1 wait=spin stream_seconds={case.seconds:.3f} verification=every_byte '
                    f'build={build}\n')
        sender, receiver = metadata, metadata
        for size in SIZES:
            latency = (p50 or {}).get(size, 10.0)
            rate = (gb or {}).get(size, 2 * size / latency / 1e3)
            sender += (f'fabric-check: size={size} rounds=10 wrong_words=0 rtt_median_us={latency:.2f} '
                       f'rtt_p99_us={latency * 1.1:.2f} rtt_max_us={latency * 2:.2f} rtt_p50_us={latency:.2f} '
                       f'gb_s={rate:.3f}\n')
            receiver += f'fabric-check: size={size} rounds=10 wrong_words=0\n'
        if case.seconds:
            sender += (f'fabric-check: stream bytes={count} seconds={elapsed:.3f} '
                       f'gbit_s={count * 8 / elapsed / 1e9:.2f} checked_bytes={count} '
                       f'gb_s={count / elapsed / 1e9:.3f}\n')
            for k in range(links):
                sender += f'fabric-check: stream link={k} posted_bytes={count // links} completed_bytes={count // links}\n'
            receiver += f'fabric-check: stream checked_bytes={count} wrong_words=0\n'
        return sender + 'fabric-check: rank 0 PASS\n', receiver + 'fabric-check: rank 1 PASS\n'

    case = Case('a-to-b', 'bond', 1)
    sender, receiver = fixture(case, 2)
    measured = parse_pair(sender, receiver, case, 10, 2)
    assert measured['bytes'] == 64 << 20 and measured['build'] == '4242'
    assert math.isclose(measured['gbit_s'], (64 << 20) * 8 / 1.001 / 1e9)
    assert measured['reported_gbit_s'] == 0.54
    assert set(measured['p50_us']) == set(SIZES) and measured['p99_us'][64] == 11.0
    bad_pairs = [
        (sender.replace('completed_bytes=33554432', 'completed_bytes=33554431'), receiver),
        (sender.replace('posted_bytes=33554432', 'posted_bytes=33554433'), receiver),
        (sender.replace('link=1 posted_bytes=', 'link=0 posted_bytes='), receiver),
        (sender, receiver.replace('checked_bytes=67108864', 'checked_bytes=67108863')),
        (sender, receiver.replace('wrong_words=0', 'wrong_words=1', 1)),
        (sender.replace('rank 0 PASS', 'rank 0 FAIL'), receiver),
        (sender.replace('seconds=1.001', 'seconds=0.998'), receiver),
        (sender.replace('gbit_s=0.54', 'gbit_s=140.00'), receiver),
        (sender.replace('67108864', '100'), receiver.replace('67108864', '100')),
        (sender.replace('67108864', '67108865'), receiver.replace('67108864', '67108865')),
        (sender, receiver.replace('rounds=10', 'rounds=9')),
        (sender.replace('rtt_p50_us=10.00', 'rtt_p50_us=nan', 1), receiver),
        (sender.replace('rtt_p99_us=11.00', 'rtt_p99_us=0', 1), receiver),
        (sender.replace('size=163840', 'size=64'), receiver),
        (sender.replace('size=1048576 rounds', 'size=1048575 rounds'), receiver),
        (sender.replace(' gb_s=', ' rate=', 1), receiver),
        (sender, receiver.replace('build=4242', 'build=4243')),
        (sender.replace('build=4242', 'build=unknown'), receiver.replace('build=4242', 'build=unknown')),
    ]
    for key, value in (('abi', '2'), ('links', '1'), ('mode', 'split'), ('progress', '0'), ('qos', '0'),
                       ('wait', 'library'), ('stream_seconds', '2.000'), ('verification', 'final_slots')):
        was = re.search(rf'\b{key}=(\S+)', receiver)[1]
        bad_pairs.append((sender, receiver.replace(f'{key}={was}', f'{key}={value}')))
    for bad_sender, bad_receiver in bad_pairs:
        try:
            parse_pair(bad_sender, bad_receiver, case, 10, 2)
        except ValueError:
            pass
        else:
            raise AssertionError('invalid qualification output passed')
    old_case = dataclasses.replace(case, seconds=60)
    old_sender, old_receiver = fixture(old_case, 2)
    old_sender = old_sender.replace('67108864', '100').replace('33554432', '50').replace('gbit_s=0.01', 'gbit_s=140.00')
    old_receiver = old_receiver.replace('67108864', '100')
    try:
        parse_pair(old_sender, old_receiver, old_case, 10, 2)
    except ValueError:
        pass
    else:
        raise AssertionError('100 bytes over 60.001 seconds reported as 140 Gbit/s passed')
    for requested in plan(60):
        links = 2 if requested.link == 'bond' else 1
        parse_pair(*fixture(requested, links), requested, 10, links)
    latency = parse_pair(*fixture(Case('a-to-b', 'link1', 0), 1), Case('a-to-b', 'link1', 0), 10, 1)
    assert 'bytes' not in latency

    # One cable at 9 GB/s, the other degraded, and a bond that beats both as the gates require.
    single = {64: 9.0, 10240: 12.0, 40960: 20.0, 163840: 45.0, 1048576: 240.0}
    slow = {size: value * 1.5 for size, value in single.items()}
    bond = {64: 9.5, 10240: 12.5, 40960: 15.0, 163840: 30.0, 1048576: 130.0}

    def results_for(bond_p50, bond_gb, rate=(16.0, 9.0, 30.0)):
        results = {'seconds': 60}
        for direction in ('a-to-b', 'b-to-a'):
            for link, p50, gbit in (('link1', single, rate[0]), ('link2', slow, rate[1]), ('bond', bond_p50, rate[2])):
                gb = {size: 2 * size / value / 1e3 for size, value in p50.items()}
                if link == 'bond':
                    gb.update(bond_gb)
                results[Case(direction, link, 60).label] = {
                    'p50_us': dict(p50), 'p99_us': {k: v * 1.1 for k, v in p50.items()}, 'gb_s': gb,
                    'gbit_s': gbit, 'stream_gb_s': gbit / 8}
        return results

    good = results_for(bond, {})
    assert all(c['pass'] for c in acceptance(good, 'healthy')), acceptance(good, 'healthy')
    assert all(c['pass'] for c in acceptance(good, 'degraded'))
    assert 'stream' in table(good) and '1048576' in table(good)
    failing = [
        results_for({**bond, 10240: 13.5}, {}),                  # small exchange more than 1 us slower
        results_for({**bond, 40960: 20.0}, {}),                  # 40 KB not faster than the better cable
        results_for(bond, {1048576: 1.5 * 2 * 1048576 / 240.0 / 1e3}),  # 1 MiB under 1.6x the faster cable
        results_for(bond, {}, rate=(16.0, 9.0, 28.0)),           # stream under 1.8x the faster cable
    ]
    for results in failing:
        assert not all(c['pass'] for c in acceptance(results, 'healthy'))
    slow_tail = results_for(bond, {})
    slow_tail[Case('b-to-a', 'bond', 60).label]['p99_us'][163840] = 1.25 * 45.0 * 1.1 + 2.5
    assert not all(c['pass'] for c in acceptance(slow_tail, 'healthy'))
    degraded = results_for(bond, {1048576: 0.84 * (2 * 1048576 / 240.0 + 2 * 1048576 / 360.0) / 1e3})
    assert not all(c['pass'] for c in acceptance(degraded, 'degraded'))
    assert all(c['pass'] for c in acceptance(results_for(bond, {}, rate=(16.0, 9.0, 22.5)), 'degraded'))
    assert not all(c['pass'] for c in acceptance(results_for(bond, {}, rate=(16.0, 9.0, 22.4)), 'degraded'))

    assert len(plan(60)) == 6
    endpoint = Endpoint('host.example.invalid', "/opt/checker $(literal) 'name'", ('rdma_en2', 'rdma_en3'),
                        ('en2', 'en3'), ('192.0.2.1', '198.51.100.1'), ('ProxyJump=jump.example.invalid',))
    for command in commands(case, endpoint, endpoint, 1, 18620, 10):
        assert command[command.index('--') - 1] == 'ProxyJump=jump.example.invalid'
        argv = shlex.split(command[-1][5:])
        assert argv[1] == 'MCDMA_FABRIC_QOS=1' and argv[2] == endpoint.program
        assert argv[10] == '64,10240,40960,163840,1048576' and argv[-3:] == ['combined', '1', 'spin']
    precise = dataclasses.replace(case, seconds=4096.125)
    for command in commands(precise, endpoint, endpoint, 1, 18620, 10):
        assert float(shlex.split(command[-1][5:])[11]) == precise.seconds
    print('dual-pipe-qualify: offline parser/threshold self-test PASS')


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--self-test', action='store_true', help='offline parser/threshold tests; no SSH')
    parser.add_argument('--execute', action='store_true', help='explicitly run the printed plan over SSH')
    for side in ('a', 'b'):
        for name in ('host', 'program'):
            parser.add_argument(f'--{name}-{side}')
        for name in ('devices', 'interfaces', 'addresses'):
            parser.add_argument(f'--{name}-{side}', nargs=2, metavar=('LINK1', 'LINK2'))
        parser.add_argument(f'--ssh-option-{side}', action='append', default=[], metavar='OPTION',
                            help=f'an ssh -o option for host {side}, such as ProxyJump=HOST; repeatable')
    parser.add_argument('--condition', choices=('healthy', 'degraded'))
    parser.add_argument('--gid-index', type=int, default=1)
    parser.add_argument('--base-port', type=int, default=18620, help='reserves this port and the next for the bond')
    parser.add_argument('--rounds', type=int, default=10000)
    parser.add_argument('--seconds', type=float, default=60)
    parser.add_argument('--output', type=pathlib.Path, help='private logs; default results/dual-pipe-TIMESTAMP')
    args = parser.parse_args()
    if args.self_test:
        if args.execute:
            parser.error('--self-test cannot be combined with --execute')
        self_test()
        return 0
    if args.condition is None or args.gid_index < 0 or not 1 <= args.base_port < 65535 or args.rounds < 100:
        parser.error('choose --condition and valid gid-index, base-port and at least 100 rounds')
    if not 60 <= args.seconds <= 86400:
        parser.error('--seconds must be 60 through 86400 for sustained qualification')
    endpoints = []
    for side in ('a', 'b'):
        values = [getattr(args, f'{name}_{side}') for name in ('host', 'program', 'devices', 'interfaces', 'addresses')]
        if not all(values):
            parser.error(f'supply host, program, devices, interfaces and addresses for side {side}')
        if values[0].startswith('-') or any('+' in v for group in values[2:] for v in group):
            parser.error('a host cannot start with -, and each lane argument must describe one lane')
        options = tuple(getattr(args, f'ssh_option_{side}'))
        if any(not option or option.startswith('-') for option in options):
            parser.error('each --ssh-option is one KEY=VALUE for ssh -o')
        endpoints.append(Endpoint(values[0], values[1], *(tuple(v) for v in values[2:]), options))
    cases = plan(args.seconds)
    for case in cases:
        print(f'# {case.label}')
        for cmd in commands(case, *endpoints, args.gid_index, args.base_port, args.rounds):
            print(shlex.join(cmd))
    if not args.execute:
        print('Review only: no SSH or hardware operation ran; add --execute after reviewing this plan.')
        return 0
    for endpoint in endpoints:
        try:
            running = leftover(endpoint)
        except (RuntimeError, subprocess.TimeoutExpired) as exc:
            print(f'FAIL before the first case: {exc}', file=sys.stderr)
            return 1
        if running:
            print(f'a fabric-check is still running on {endpoint.host}; stop it before qualifying', file=sys.stderr)
            return 1
    stamp = datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%SZ')
    output = args.output or pathlib.Path(__file__).resolve().parents[1] / 'results' / f'dual-pipe-{stamp}'
    output.mkdir(parents=True, exist_ok=False)
    (output / 'configuration.json').write_text(json.dumps(vars(args), default=str, indent=2) + '\n')
    results = {'seconds': args.seconds}
    builds = set()
    for index, case in enumerate(cases):
        case_dir = output / f'{index:02d}-{case.label}'
        case_dir.mkdir()
        try:
            texts = run_pair(commands(case, *endpoints, args.gid_index, args.base_port, args.rounds), case_dir,
                             case.seconds + 600)
            result = parse_pair(*texts, case, args.rounds, 2 if case.link == 'bond' else 1)
        except (RuntimeError, ValueError, subprocess.TimeoutExpired) as exc:
            print(f'FAIL {case.label}: {exc}; private logs: {case_dir}', file=sys.stderr)
            return 1
        builds.add(result['build'])
        results[case.label] = result
        (case_dir / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    if len(builds) != 1:
        print('FAIL: the cases ran different fabric-check builds', file=sys.stderr)
        return 1
    checks = acceptance(results, args.condition)
    (output / 'summary.json').write_text(json.dumps({'condition': args.condition, 'build': builds.pop(),
                                                     'results': results, 'checks': checks}, indent=2) + '\n')
    print(table(results))
    print(json.dumps(checks, indent=2))
    passed = all(c['pass'] for c in checks)
    print(f'dual-pipe-qualify: {"PASS" if passed else "FAIL"}; private logs: {output}')
    return 0 if passed else 1


if __name__ == '__main__':
    sys.exit(main())
