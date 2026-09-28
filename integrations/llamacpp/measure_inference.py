#!/usr/bin/env python3
"""Measure pinned llama.cpp HTTP inference and host-file KV save/restore.

Results include raw SSE events and token outputs and must remain private.
This tool does not transfer state between hosts or claim direct GPU KV access.
"""
from __future__ import annotations

import argparse
import codecs
from datetime import datetime, timezone
import hashlib
import http.client
import json
import math
import os
from pathlib import Path
import re
import socket
import struct
import threading
import time
from urllib.parse import urlsplit

SIZES = (512, 2048, 8192)
MAX_LINE_BYTES = 1024 * 1024
MAX_EVENTS = 16384
PREFIX = '<|im_start|>system\nYou are a concise science teacher.<|im_end|>\n<|im_start|>user\n'
PARAGRAPH = ('A weather station measures temperature, air pressure, humidity, wind direction and rainfall. '
             'Water evaporates from oceans and lakes, rises in moist air, cools, and condenses into tiny droplets. '
             'Clouds form when enough droplets collect, and precipitation returns water to the surface. '
             'These notes describe a fictional weather survey and contain no measurements from a real place.\n')
SUFFIX = ('\nExplain the water cycle and how clouds produce rain in clear language. Give useful examples and avoid '
          'repeating these notes.<|im_end|>\n<|im_start|>assistant\n<think>\n\n</think>\n')


class MeasurementError(RuntimeError):
    pass


def strict_json(data):
    def reject_constant(_):
        raise MeasurementError('server JSON contains a non-finite number')
    return json.loads(data, parse_constant=reject_constant)


def token_list(value):
    if not isinstance(value, list) or any(type(x) is not int or not 0 <= x < 2**31 for x in value):
        raise MeasurementError('expected a list of nonnegative signed-32-bit token IDs')
    return value


def token_digest(tokens):
    return hashlib.sha256(b''.join(struct.pack('<I', x) for x in tokens)).hexdigest()


def state_basename(value):
    if not re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9_.-]{0,95}', value):
        raise argparse.ArgumentTypeError('state name must be a simple 1..96 character basename')
    return value


def state_for_repeat(name, repetition, repetitions):
    if repetitions == 1:
        return name
    stem, dot, suffix = name.rpartition('.')
    if not dot:
        stem, suffix = name, ''
    return f'{stem}-{repetition + 1:03d}' + ('.' + suffix if suffix else '')


def server_url(value):
    try:
        parsed = urlsplit(value)
        port = parsed.port
    except ValueError:
        raise argparse.ArgumentTypeError('invalid server URL') from None
    if (parsed.scheme not in ('http', 'https') or not parsed.hostname or parsed.username is not None
            or parsed.password is not None or parsed.query or parsed.fragment
            or parsed.path not in ('', '/') or (port is not None and not 1 <= port <= 65535)):
        raise argparse.ArgumentTypeError('use an http(s) origin without credentials, path, query or fragment')
    return value.rstrip('/')


class Client:
    """No redirects or proxy environment; a watchdog bounds each HTTP exchange."""
    def __init__(self, url, timeout=300, max_response=16 << 20, max_total=64 << 20):
        self.url, self.timeout = url, timeout
        self.max_response, self.max_total, self.received = max_response, max_total, 0

    def request(self, path, body, consume):
        parsed = urlsplit(self.url)
        kind = http.client.HTTPSConnection if parsed.scheme == 'https' else http.client.HTTPConnection
        connection = kind(parsed.hostname, parsed.port, timeout=self.timeout)
        held_socket = [None]
        expired = threading.Event()

        def cancel():
            expired.set()
            target = held_socket[0] or connection.sock
            if target is not None:
                try:
                    target.shutdown(socket.SHUT_RDWR)
                except OSError:
                    pass

        watchdog = threading.Timer(self.timeout, cancel)
        watchdog.daemon = True
        started = time.monotonic()
        watchdog.start()
        response_bytes = 0
        try:
            connection.connect()
            held_socket[0] = connection.sock
            if expired.is_set():
                raise MeasurementError('HTTP exchange exceeded its deadline')
            encoded = json.dumps(body, separators=(',', ':'), allow_nan=False).encode('utf-8')
            connection.request('POST', path, body=encoded,
                               headers={'Content-Type': 'application/json', 'Accept': 'application/json, text/event-stream'})
            response = connection.getresponse()
            if response.status != 200:
                raise MeasurementError(f'HTTP request failed with status {response.status}')
            while True:
                if expired.is_set() or time.monotonic() - started >= self.timeout:
                    raise MeasurementError('HTTP exchange exceeded its deadline')
                chunk = response.read1(65536)
                if not chunk:
                    break
                response_bytes += len(chunk)
                self.received += len(chunk)
                if response_bytes > self.max_response or self.received > self.max_total:
                    raise MeasurementError('HTTP response capture exceeded the configured byte limit')
                consume(chunk)
            if expired.is_set():
                raise MeasurementError('HTTP exchange exceeded its deadline')
        except (OSError, http.client.HTTPException) as exc:
            reason = 'HTTP exchange exceeded its deadline' if expired.is_set() else 'HTTP transport failed: ' + type(exc).__name__
            raise MeasurementError(reason) from None
        finally:
            watchdog.cancel()
            connection.close()
        return {'wall_s': time.monotonic() - started, 'response_bytes': response_bytes}

    def json(self, path, body):
        chunks = []
        timing = self.request(path, body, chunks.append)
        try:
            value = strict_json(b''.join(chunks))
        except (ValueError, UnicodeError):
            raise MeasurementError('server returned invalid JSON') from None
        if not isinstance(value, dict) or 'error' in value:
            raise MeasurementError('server returned an error or non-object JSON response')
        return value, timing


class SSE:
    def __init__(self, record, started):
        self.record, self.started = record, started
        self.decoder = codecs.getincrementaldecoder('utf-8')('strict')
        self.pending = self.block = ''
        self.final = None
        self.done = False
        record.update(events=[], raw_sse_events=[], output_tokens=[], output_text='', ttft_s=None,
                      sse_capture_complete=False)

    def feed(self, chunk):
        try:
            self.pending += self.decoder.decode(chunk)
        except UnicodeError:
            raise MeasurementError('SSE response is not UTF-8') from None
        while '\n' in self.pending:
            line, self.pending = self.pending.split('\n', 1)
            if len(line.encode('utf-8')) > MAX_LINE_BYTES:
                raise MeasurementError('SSE line exceeds capture limit')
            self.block += line + '\n'
            if not line.rstrip('\r'):
                block = self.block
                self.block = ''
                self.event(block)
        if len(self.pending.encode('utf-8')) > MAX_LINE_BYTES or len(self.block.encode('utf-8')) > MAX_LINE_BYTES:
            raise MeasurementError('SSE event exceeds capture limit')

    def event(self, raw):
        elapsed = time.monotonic() - self.started
        self.record['raw_sse_events'].append({'arrival_s': elapsed, 'raw': raw})
        if len(raw.encode('utf-8')) > MAX_LINE_BYTES:
            raise MeasurementError('SSE event exceeds capture limit')
        if len(self.record['raw_sse_events']) > MAX_EVENTS:
            raise MeasurementError('too many SSE events')
        data = []
        event_type = None
        for line in raw.splitlines():
            if not line or line.startswith(':'):
                continue
            field, _, value = line.partition(':')
            value = value[1:] if value.startswith(' ') else value
            if field == 'data':
                data.append(value)
            elif field == 'event':
                event_type = value
        if not data:
            return
        joined = '\n'.join(data)
        if joined == '[DONE]':
            if self.done or self.final is None:
                raise MeasurementError('SSE DONE arrived before a final result or more than once')
            self.done = True
            return
        if self.done or self.final is not None:
            raise MeasurementError('SSE payload arrived after its final result')
        try:
            event = strict_json(joined)
        except ValueError:
            raise MeasurementError('invalid JSON in SSE event') from None
        self.record['events'].append(event)
        if not isinstance(event, dict) or 'error' in event or event_type == 'error':
            raise MeasurementError('server returned an SSE error')
        content = event.get('content', '')
        tokens = token_list(event.get('tokens', []))
        if not isinstance(content, str):
            raise MeasurementError('SSE content is not text')
        if self.record['ttft_s'] is None and (content or tokens):
            self.record['ttft_s'] = elapsed
        self.record['output_tokens'].extend(tokens)
        self.record['output_text'] += content
        if event.get('stop') is True:
            if not isinstance(event.get('timings'), dict):
                raise MeasurementError('final SSE result has no server timings')
            self.final = event
            self.record['timings'] = event['timings']

    def finish(self):
        try:
            self.pending += self.decoder.decode(b'', final=True)
        except UnicodeError:
            raise MeasurementError('truncated UTF-8 in SSE response') from None
        if self.pending or self.block or self.final is None:
            raise MeasurementError('truncated SSE response or missing final result')

    def retain_tail(self):
        if self.block or self.pending:
            self.record['raw_sse_incomplete_tail'] = self.block + self.pending


def prompts(client):
    parts = []
    for text in (PREFIX, PARAGRAPH, SUFFIX):
        response, _ = client.json('/tokenize', {'content': text, 'add_special': False})
        parts.append(token_list(response.get('tokens')))
    beginning, paragraph, ending = parts
    if not beginning or not paragraph or not ending:
        raise MeasurementError('tokenizer returned an empty fixed component')
    result = {}
    for size in SIZES:
        middle = size - len(beginning) - len(ending)
        if middle < 0:
            raise MeasurementError('prompt components exceed the requested token length')
        tokens = beginning + (paragraph * ((middle + len(paragraph) - 1) // len(paragraph)))[:middle] + ending
        result[str(size)] = {'tokens': tokens, 'token_sha256': token_digest(tokens)}
    return result


def load_prompt(filename, size):
    path = Path(filename)
    if path.stat().st_size > 2 << 20:
        raise MeasurementError('prompt artifact exceeds 2 MiB')
    try:
        saved = json.loads(path.read_text())
        entries = saved.get('prompts', saved)
        entry = entries[str(size)]
        tokens = token_list(entry.get('tokens') if isinstance(entry, dict) else entry)
    except (ValueError, UnicodeError, KeyError, TypeError, AttributeError):
        raise MeasurementError('invalid prompt artifact or missing requested size') from None
    if len(tokens) != size:
        raise MeasurementError('stored prompt token count does not match --size')
    if isinstance(entry, dict) and entry.get('token_sha256') != token_digest(tokens):
        raise MeasurementError('stored prompt token digest does not match')
    return tokens


def completion(client, tokens, n_predict, cache, slot, record):
    record.update(input_tokens=len(tokens), requested_output_tokens=n_predict, cache_prompt=cache)
    body = {'prompt': tokens, 'n_predict': n_predict, 'temperature': 0, 'top_k': 1, 'seed': 42,
            'ignore_eos': True, 'cache_prompt': cache, 'id_slot': slot, 'return_tokens': True, 'stream': True}
    started = time.monotonic()
    parser = SSE(record, started)
    try:
        timing = client.request('/completion', body, parser.feed)
        parser.finish()
        record['response_bytes'] = timing['response_bytes']
        record['sse_capture_complete'] = True
    finally:
        record['elapsed_s'] = time.monotonic() - started
        parser.retain_tail()
    return started


def count(record, key):
    value = record.get('timings', {}).get(key)
    if type(value) is not int or value < 0:
        raise MeasurementError('server timings are missing a nonnegative integer ' + key)
    return value


def check_generation(record, size, expected, cached=False):
    prompt_n, cache_n, predicted_n = (count(record, name) for name in ('prompt_n', 'cache_n', 'predicted_n'))
    if (not cached and (prompt_n != size or cache_n != 0)) or (cached and (cache_n < size - 1 or prompt_n > 1)):
        raise MeasurementError('server prompt/cache counts do not prove the requested execution path')
    if predicted_n != expected or len(record['output_tokens']) != expected or record['ttft_s'] is None:
        raise MeasurementError('server generation count, captured tokens or first-token observation is incomplete')


def slot_count(response, field, expected):
    if type(response.get(field)) is not int or response[field] != expected:
        raise MeasurementError('slot response does not report the expected ' + field)


def bounded_integer(maximum, minimum=1):
    def parse(value):
        try:
            number = int(value)
        except ValueError:
            raise argparse.ArgumentTypeError('expected an integer') from None
        if not minimum <= number <= maximum:
            raise argparse.ArgumentTypeError(f'expected {minimum}..{maximum}')
        return number
    return parse


def duration(value):
    try:
        number = float(value)
    except ValueError:
        raise argparse.ArgumentTypeError('expected seconds') from None
    if not math.isfinite(number) or not 0 < number <= 3600:
        raise argparse.ArgumentTypeError('timeout must be greater than zero and at most 3600 seconds')
    return number


def arguments(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=('prompts', 'baseline', 'prefill', 'decode'))
    parser.add_argument('--url', type=server_url, default='http://127.0.0.1:8080')
    parser.add_argument('--prompts', type=Path, help='saved token artifact; required except for prompts')
    parser.add_argument('--output', type=Path, required=True, help='new private JSON file; parent must exist')
    parser.add_argument('--size', type=int, choices=SIZES, default=512)
    parser.add_argument('--repetitions', type=bounded_integer(100), default=3)
    parser.add_argument('--n-predict', type=bounded_integer(8192), default=128)
    parser.add_argument('--state-name', type=state_basename, help='server slot-state basename, without directories')
    parser.add_argument('--slot', type=bounded_integer(255, 0), default=0, metavar='0..255')
    parser.add_argument('--timeout', type=duration, default=300)
    parser.add_argument('--max-response-bytes', type=bounded_integer(64 << 20), default=16 << 20)
    parser.add_argument('--max-total-response-bytes', type=bounded_integer(256 << 20), default=64 << 20)
    args = parser.parse_args(argv)
    if args.action != 'prompts' and args.prompts is None:
        parser.error('--prompts is required for measurement actions')
    if args.action in ('prefill', 'decode') and args.state_name is None:
        parser.error('--state-name is required for prefill and decode')
    return args


def run(args, client=None):
    # Reserve before any HTTP work; O_EXCL refuses existing files and symlinks.
    fd = os.open(args.output, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    report = {'schema': 1, 'action': args.action, 'pass': False,
              'started_utc': datetime.now(timezone.utc).isoformat(),
              'clock': 'local monotonic; TTFT is first streamed content or token ID after request start',
              'url': args.url, 'slot': args.slot, 'runs': [],
              'host_file_staging': args.action in ('prefill', 'decode'),
              'direct_gpu_kv_transfer': False,
              'transfer_between_hosts_measured': False,
              'warmup_performed': False}
    client = client or Client(args.url, args.timeout, args.max_response_bytes, args.max_total_response_bytes)
    with os.fdopen(fd, 'w') as output:
        def checkpoint():
            output.seek(0)
            json.dump(report, output, indent=2, allow_nan=False)
            output.write('\n'); output.truncate(); output.flush(); os.fsync(output.fileno())
        checkpoint()
        try:
            if args.action == 'prompts':
                report['template'] = {'prefix': PREFIX, 'paragraph': PARAGRAPH, 'suffix': SUFFIX,
                                      'tokenizer_add_special': False, 'component_tokenizations': 3}
                report['prompts'] = prompts(client)
            else:
                tokens = load_prompt(args.prompts, args.size)
                report.update(prompt_tokens=tokens, token_sha256=token_digest(tokens),
                              size=args.size, repetitions=args.repetitions,
                              n_predict=0 if args.action == 'prefill' else args.n_predict)
                for repetition in range(args.repetitions):
                    record = {'repetition': repetition, 'pass': False}
                    report['runs'].append(record)
                    started = time.monotonic()
                    if args.action == 'decode':
                        name = state_for_repeat(args.state_name, repetition, args.repetitions)
                        record['state_name'] = name
                        restored, timing = client.json(f'/slots/{args.slot}?action=restore', {'filename': name})
                        record.update(restore=restored, restore_wall_s=timing['wall_s'])
                        slot_count(restored, 'n_restored', args.size)
                    completion_started = completion(client, tokens, 0 if args.action == 'prefill' else args.n_predict,
                                                    args.action == 'decode', args.slot, record)
                    # This pinned server samples once for n_predict=0; preserve
                    # and check that result instead of labeling it zero output.
                    expected = 1 if args.action == 'prefill' else args.n_predict
                    check_generation(record, args.size, expected, args.action == 'decode')
                    if args.action == 'prefill':
                        name = state_for_repeat(args.state_name, repetition, args.repetitions)
                        record['state_name'] = name
                        saved, timing = client.json(f'/slots/{args.slot}?action=save', {'filename': name})
                        record.update(save=saved, save_wall_s=timing['wall_s'],
                                      prefill_and_save_s=time.monotonic() - started)
                        slot_count(saved, 'n_saved', args.size)
                    elif args.action == 'decode':
                        record['restore_and_decode_s'] = time.monotonic() - started
                        record['restore_to_first_token_s'] = completion_started - started + record['ttft_s']
                    record['pass'] = True
                    checkpoint()
            report['pass'] = True
        except (MeasurementError, OSError, ValueError) as exc:
            report['error'] = str(exc) if isinstance(exc, MeasurementError) else type(exc).__name__
        except KeyboardInterrupt:
            report['error'] = 'interrupted'
        finally:
            report['finished_utc'] = datetime.now(timezone.utc).isoformat()
            report['received_http_bytes'] = client.received
            checkpoint()
    return report


def main(argv=None):
    args = arguments(argv)
    try:
        report = run(args)
    except OSError:
        print('INFERENCE_MEASUREMENT pass=0 reason=cannot-create-or-write-new-private-output')
        return 1
    print('INFERENCE_MEASUREMENT pass=' + str(int(report['pass'])) + ' action=' + args.action)
    return 0 if report['pass'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
