#!/usr/bin/env python3
"""Review-only by default: print the two-host qualification plan; --execute runs it over SSH.

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


@dataclasses.dataclass(frozen=True)
class Endpoint:
    host: str
    program: str
    devices: tuple
    interfaces: tuple
    addresses: tuple


@dataclasses.dataclass(frozen=True)
class Case:
    direction: str
    link: str
    qos: int
    wait_poll: int
    mode: str
    seconds: float

    @property
    def label(self):
        return f'{self.direction}-{self.link}-qos{self.qos}-poll{self.wait_poll}-{self.mode}-{self.seconds:g}s'


def commands(case, a, b, gid, port, rounds):
    selected = (0, 1) if case.link == 'bond' else (int(case.link[-1]) - 1,)
    sender, receiver = (a, b) if case.direction == 'a-to-b' else (b, a)
    result = []
    for rank, local, remote in ((0, sender, receiver), (1, receiver, sender)):
        device = '+'.join(local.devices[i] for i in selected)
        via = '+'.join(f'{local.interfaces[i]}/{remote.addresses[i]}' for i in selected)
        remote_argv = ['env', f'MCDMA_FABRIC_QOS={case.qos}', f'MCDMA_FABRIC_WAIT_POLL={case.wait_poll}',
                       local.program, device, str(gid), via, str(port), 'qualify', str(rank), str(rounds),
                       '64,4096', str(case.seconds), '0', case.mode, '1']
        result.append(['ssh', '-o', 'BatchMode=yes', '-o', 'ServerAliveInterval=5', '--', local.host,
                       'exec ' + shlex.join(remote_argv)])
    return result


def plan(seconds):
    # Sustained runs first: each physical cable and then the bond, in both directions.
    cases = [Case(direction, link, 1, 1, 'combined', seconds)
             for direction in ('a-to-b', 'b-to-a') for link in ('link1', 'link2', 'bond')]
    # The baseline RTT samples are already present above; these runs have no stream.
    for qos, wait_poll, mode in ((0, 1, 'combined'), (1, 0, 'combined'), (0, 0, 'combined'), (1, 1, 'split')):
        cases.extend(Case(direction, link, qos, wait_poll, mode, 0)
                     for direction in ('a-to-b', 'b-to-a') for link in ('link1', 'link2', 'bond'))
    return cases


def fields(line):
    entries = [part.split('=', 1) for part in line.split()]
    if any(len(part) != 2 or not part[0] or not part[1] for part in entries):
        raise ValueError('malformed checker fields')
    result = dict(entries)
    if len(result) != len(entries):
        raise ValueError('duplicate checker fields')
    return result


def parse_pair(sender, receiver, case, rounds, links):
    expected = {'abi': '1', 'links': str(links), 'mode': case.mode, 'progress': '1',
                'qos': str(case.qos), 'wait_poll': str(case.wait_poll), 'verification': 'every_byte'}
    precision_seconds = 0.000500001   # seconds are printed to three decimals
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
    latency = {}
    for rank, text in enumerate((sender, receiver)):
        sizes = {}
        for row in re.findall(r'^fabric-check: (size=.+)$', text, re.M):
            row = fields(row)
            size = int(row.get('size', '0'))
            if size in sizes or int(row.get('rounds', '0')) != rounds or row.get('wrong_words') != '0':
                raise ValueError(f'rank {rank} has duplicate sizes or incorrect rounds/results')
            sizes[size] = row
            if rank == 0:
                p50 = float(row.get('rtt_p50_us', 'nan'))
                if not math.isfinite(p50) or p50 <= 0:
                    raise ValueError('missing or invalid p50')
                latency[size] = p50
        if set(sizes) != {64, 4096}:
            raise ValueError(f'rank {rank} lacks its two requested latency sizes')
    result = {'p50_us': latency}
    if not case.seconds:
        return result
    match = re.search(r'^fabric-check: stream bytes=(\d+) seconds=([\d.]+) gbit_s=([\d.]+) '
                      r'checked_bytes=(\d+)$', sender, re.M)
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
    result.update(bytes=byte_count, seconds=elapsed, gbit_s=rate, reported_gbit_s=reported_rate, links=counters)
    return result


def acceptance(results, condition):
    checks = []
    for direction in ('a-to-b', 'b-to-a'):
        singles = [results[Case(direction, link, 1, 1, 'combined', results['seconds']).label]
                   for link in ('link1', 'link2')]
        bonded = results[Case(direction, 'bond', 1, 1, 'combined', results['seconds']).label]
        rates = [s['gbit_s'] for s in singles]
        required = 1.8 * max(rates) if condition == 'healthy' else 0.9 * sum(rates)
        checks.append({'direction': direction, 'check': 'stream', 'actual_gbit_s': bonded['gbit_s'],
                       'required_gbit_s': required, 'pass': bonded['gbit_s'] >= required})
        for size in (64, 4096):
            limit = min(s['p50_us'][size] for s in singles) + 1.0
            checks.append({'direction': direction, 'check': 'p50', 'size': size,
                           'actual_us': bonded['p50_us'][size], 'maximum_us': limit,
                           'pass': bonded['p50_us'][size] <= limit})
    return checks


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


def self_test():
    def fixture(case):
        count, elapsed = 64 << 20, case.seconds + 0.001
        metadata = (f'fabric-check: metadata abi=1 links=2 mode={case.mode} progress=1 qos={case.qos} '
                    f'wait_poll={case.wait_poll} stream_seconds={case.seconds:.3f} verification=every_byte\n')
        sender = metadata + ('fabric-check: size=64 rounds=10 wrong_words=0 rtt_median_us=9.00 rtt_p50_us=9.00\n'
                             'fabric-check: size=4096 rounds=10 wrong_words=0 rtt_median_us=10.00 rtt_p50_us=10.00\n')
        receiver = metadata + ('fabric-check: size=64 rounds=10 wrong_words=0\n'
                               'fabric-check: size=4096 rounds=10 wrong_words=0\n')
        if case.seconds:
            sender += (f'fabric-check: stream bytes={count} seconds={elapsed:.3f} '
                       f'gbit_s={count * 8 / elapsed / 1e9:.2f} checked_bytes={count}\n'
                       f'fabric-check: stream link=0 posted_bytes={count // 2} completed_bytes={count // 2}\n'
                       f'fabric-check: stream link=1 posted_bytes={count // 2} completed_bytes={count // 2}\n')
            receiver += f'fabric-check: stream checked_bytes={count} wrong_words=0\n'
        return sender + 'fabric-check: rank 0 PASS\n', receiver + 'fabric-check: rank 1 PASS\n'

    case = Case('a-to-b', 'bond', 1, 1, 'combined', 1)
    sender, receiver = fixture(case)
    measured = parse_pair(sender, receiver, case, 10, 2)
    assert measured['bytes'] == 64 << 20
    assert math.isclose(measured['gbit_s'], (64 << 20) * 8 / 1.001 / 1e9)
    assert measured['reported_gbit_s'] == 0.54
    bad_pairs = [
        (sender.replace('completed_bytes=33554432', 'completed_bytes=33554431'), receiver),
        (sender.replace('posted_bytes=33554432', 'posted_bytes=33554433'), receiver),
        (sender.replace('link=1 posted_bytes=', 'link=0 posted_bytes='), receiver),
        (sender, receiver.replace('checked_bytes=67108864', 'checked_bytes=67108863')),
        (sender, receiver.replace('wrong_words=0', 'wrong_words=1')),
        (sender.replace('rank 0 PASS', 'rank 0 FAIL'), receiver),
        (sender.replace('seconds=1.001', 'seconds=0.998'), receiver),
        (sender.replace('gbit_s=0.54', 'gbit_s=140.00'), receiver),
        (sender.replace('67108864', '100'), receiver.replace('67108864', '100')),
        (sender.replace('67108864', '67108865'), receiver.replace('67108864', '67108865')),
        (sender, receiver.replace('rounds=10', 'rounds=9')),
        (sender.replace('rtt_p50_us=9.00', 'rtt_p50_us=nan'), receiver),
        (sender.replace('size=4096', 'size=64'), receiver),
    ]
    for key, value in (('abi', '2'), ('links', '1'), ('mode', 'split'), ('progress', '0'), ('qos', '0'),
                       ('wait_poll', '0'), ('stream_seconds', '2.000'), ('verification', 'final_slots')):
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
    old_sender, old_receiver = fixture(old_case)
    old_sender = old_sender.replace('67108864', '100').replace('33554432', '50').replace('gbit_s=0.01', 'gbit_s=140.00')
    old_receiver = old_receiver.replace('67108864', '100')
    try:
        parse_pair(old_sender, old_receiver, old_case, 10, 2)
    except ValueError:
        pass
    else:
        raise AssertionError('100 bytes over 60.001 seconds reported as 140 Gbit/s passed')
    for requested in plan(60):
        # The same strict metadata checks cover the whole QoS/poll/mode matrix, including no-stream runs.
        requested = dataclasses.replace(requested, link='bond')
        parse_pair(*fixture(requested), requested, 10, 2)
    results = {'seconds': 60}
    for direction in ('a-to-b', 'b-to-a'):
        for link, rate, latency in (('link1', 1, 10), ('link2', 2, 9), ('bond', 3.6, 10)):
            results[Case(direction, link, 1, 1, 'combined', 60).label] = {
                'gbit_s': rate, 'p50_us': {64: latency, 4096: latency}}
    assert all(c['pass'] for c in acceptance(results, 'healthy'))
    assert all(c['pass'] for c in acceptance(results, 'degraded'))
    bonded = results[Case('a-to-b', 'bond', 1, 1, 'combined', 60).label]
    bonded['gbit_s'] = 3.5999
    assert not all(c['pass'] for c in acceptance(results, 'healthy'))
    bonded['gbit_s'] = 2.7
    assert all(c['pass'] for c in acceptance(results, 'degraded'))
    bonded['gbit_s'] = 2.6999
    assert not all(c['pass'] for c in acceptance(results, 'degraded'))
    bonded['gbit_s'] = 3.6
    bonded['p50_us'][4096] = 10.0001
    assert not all(c['pass'] for c in acceptance(results, 'healthy'))
    assert len(plan(60)) == 30
    endpoint = Endpoint('host.example.invalid', "/opt/checker $(literal) 'name'", ('rdma_en2', 'rdma_en3'),
                        ('en2', 'en3'), ('192.0.2.1', '198.51.100.1'))
    for command in commands(case, endpoint, endpoint, 1, 18620, 10):
        assert shlex.split(command[-1][5:])[3] == endpoint.program
    precise = dataclasses.replace(case, seconds=4096.125)
    for command in commands(precise, endpoint, endpoint, 1, 18620, 10):
        assert float(shlex.split(command[-1][5:])[12]) == precise.seconds
    print('dual-pipe-qualify: offline parser/threshold self-test PASS')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--self-test', action='store_true', help='offline parser/threshold tests; no SSH')
    parser.add_argument('--execute', action='store_true', help='explicitly run the printed plan over SSH')
    for side in ('a', 'b'):
        for name in ('host', 'program'):
            parser.add_argument(f'--{name}-{side}')
        for name in ('devices', 'interfaces', 'addresses'):
            parser.add_argument(f'--{name}-{side}', nargs=2, metavar=('LINK1', 'LINK2'))
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
    if args.condition is None or args.gid_index < 0 or not 1 <= args.base_port < 65535 or args.rounds < 1:
        parser.error('choose --condition and valid gid-index, base-port and rounds')
    if not 60 <= args.seconds <= 86400:
        parser.error('--seconds must be 60 through 86400 for sustained qualification')
    endpoints = []
    for side in ('a', 'b'):
        values = [getattr(args, f'{name}_{side}') for name in ('host', 'program', 'devices', 'interfaces', 'addresses')]
        if not all(values):
            parser.error(f'supply host, program, devices, interfaces and addresses for side {side}')
        if values[0].startswith('-') or any('+' in v for group in values[2:] for v in group):
            parser.error('a host cannot start with -, and each lane argument must describe one lane')
        endpoints.append(Endpoint(values[0], values[1], *(tuple(v) for v in values[2:])))
    cases = plan(args.seconds)
    for case in cases:
        print(f'# {case.label}')
        for cmd in commands(case, *endpoints, args.gid_index, args.base_port, args.rounds):
            print(shlex.join(cmd))
    if not args.execute:
        print('Review only: no SSH or hardware operation ran; add --execute after reviewing this plan.')
        return 0
    stamp = datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%SZ')
    output = args.output or pathlib.Path(__file__).resolve().parents[1] / 'results' / f'dual-pipe-{stamp}'
    output.mkdir(parents=True, exist_ok=False)
    (output / 'configuration.json').write_text(json.dumps(vars(args), default=str, indent=2) + '\n')
    results = {'seconds': args.seconds}
    for index, case in enumerate(cases):
        case_dir = output / f'{index:02d}-{case.label}'
        case_dir.mkdir()
        try:
            texts = run_pair(commands(case, *endpoints, args.gid_index, args.base_port, args.rounds), case_dir,
                             case.seconds + 240)
            result = parse_pair(*texts, case, args.rounds, 2 if case.link == 'bond' else 1)
        except (RuntimeError, ValueError, subprocess.TimeoutExpired) as exc:
            print(f'FAIL {case.label}: {exc}; private logs: {case_dir}', file=sys.stderr)
            return 1
        results[case.label] = result
        (case_dir / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    checks = acceptance(results, args.condition)
    (output / 'summary.json').write_text(json.dumps({'condition': args.condition, 'results': results,
                                                  'checks': checks}, indent=2) + '\n')
    print(json.dumps(checks, indent=2))
    passed = all(c['pass'] for c in checks)
    print(f'dual-pipe-qualify: {"PASS" if passed else "FAIL"}; private logs: {output}')
    return 0 if passed else 1


if __name__ == '__main__':
    sys.exit(main())
