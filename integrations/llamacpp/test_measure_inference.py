"""Measurement contract tests use a loopback fake HTTP server, never a model."""
from contextlib import redirect_stdout
import io
import json
from pathlib import Path
import stat
import tempfile
import threading
import time
import unittest
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

import measure_inference as m


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def do_POST(self):
        body = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
        self.server.calls.append((self.path, body))
        if self.path == '/tokenize':
            result = {'tokens': [1, 2, 3]}
        elif '?action=save' in self.path:
            result = {'n_saved': self.server.size - (self.server.fault == 'save-count'), 'filename': body['filename']}
        elif '?action=restore' in self.path:
            result = {'n_restored': self.server.size - (self.server.fault == 'restore-count'),
                      'filename': body['filename']}
        else:
            self.completion(body)
            return
        data = json.dumps(result).encode()
        self.send_response(200); self.send_header('Content-Length', str(len(data))); self.end_headers()
        self.wfile.write(data)

    def completion(self, body):
        self.server.size = len(body['prompt'])
        count = body['n_predict'] or 1
        timing = {'prompt_n': 1 if body['cache_prompt'] else self.server.size,
                  'cache_n': self.server.size - 1 if body['cache_prompt'] else 0,
                  'predicted_n': count, 'prompt_ms': 4.5, 'predicted_ms': 8.5}
        if self.server.fault == 'cache-count':
            timing['cache_n'] = 1
        if self.server.fault == 'decode-reprefill':
            timing['prompt_n'] = self.server.size
        if self.server.fault == 'prediction-count':
            timing['predicted_n'] += 1
        events = [{'content': 'rain ', 'tokens': list(range(10, 10 + count)), 'stop': False},
                  {'content': '', 'tokens': [], 'stop': True, 'timings': timing}]
        wire = ''.join('data: ' + json.dumps(event) + '\n\n' for event in events).encode()
        if self.server.fault == 'truncated':
            wire = wire[:wire.find(b'\n\n') + 2]
        if self.server.fault == 'malformed':
            wire = b'data: {broken}\n\n'
        if self.server.fault == 'nonfinite':
            wire = b'data: {"stop":true,"timings":{"prompt_ms":NaN}}\n\n'
        self.send_response(200); self.send_header('Content-Type', 'text/event-stream'); self.end_headers()
        try:
            if self.server.fault == 'slow-drip':
                for byte in wire:
                    self.wfile.write(bytes([byte])); self.wfile.flush(); time.sleep(.02)
            else:
                # Split UTF-8/SSE arbitrarily to exercise incremental parsing.
                for start in range(0, len(wire), 7):
                    self.wfile.write(wire[start:start + 7]); self.wfile.flush()
        except (OSError, BrokenPipeError):
            pass


class MeasurementTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
        self.server.daemon_threads = True
        self.server.calls = []
        self.server.fault = ''
        self.server.size = 512
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()
        self.url = f'http://127.0.0.1:{self.server.server_port}'
        self.prompts = self.root / 'tokens.json'
        self.prompts.write_text(json.dumps({str(size): [7] * size for size in m.SIZES}))

    def tearDown(self):
        self.server.shutdown(); self.server.server_close(); self.thread.join(timeout=2)
        self.temp.cleanup()

    def execute(self, action='baseline', *extra):
        output = self.root / ('result-' + str(len(list(self.root.glob('result-*')))) + '.json')
        argv = [action, '--url', self.url, '--output', str(output), '--repetitions', '1']
        if action != 'prompts':
            argv += ['--prompts', str(self.prompts)]
        if action in ('prefill', 'decode'):
            argv += ['--state-name', 'trial.state']
        args = m.arguments(argv + list(extra))
        report = m.run(args)
        self.assertEqual(report, json.loads(output.read_text()))
        self.assertEqual(stat.S_IMODE(output.stat().st_mode), 0o600)
        return report

    def test_prompts_tokenize_components_once_and_save_exact_sizes(self):
        report = self.execute('prompts')
        self.assertTrue(report['pass'])
        self.assertEqual(len(self.server.calls), 3)
        for size in m.SIZES:
            entry = report['prompts'][str(size)]
            self.assertEqual(len(entry['tokens']), size)
            self.assertEqual(entry['token_sha256'], m.token_digest(entry['tokens']))
        self.assertTrue(all(body['add_special'] is False for _, body in self.server.calls))

    def test_baseline_captures_raw_events_outputs_and_uncached_counts(self):
        report = self.execute('baseline', '--size', '2048', '--repetitions', '3')
        self.assertTrue(report['pass'])
        self.assertEqual(len(report['runs']), 3)
        for record in report['runs']:
            self.assertFalse(record['cache_prompt'])
            self.assertEqual(len(record['output_tokens']), 128)
            self.assertEqual(record['timings']['prompt_n'], 2048)
            self.assertEqual(record['timings']['cache_n'], 0)
            self.assertEqual(len(record['raw_sse_events']), 2)
            self.assertGreater(record['elapsed_s'], record['ttft_s'])
        self.assertEqual(len(self.server.calls), 3)  # measurement never retokenizes

    def test_prefill_records_zero_requested_one_sample_and_exact_saved_state(self):
        report = self.execute('prefill')
        self.assertTrue(report['pass'])
        record = report['runs'][0]
        self.assertEqual(report['n_predict'], 0)
        self.assertEqual(record['requested_output_tokens'], 0)
        self.assertEqual(record['timings']['predicted_n'], 1)
        self.assertEqual(record['save']['n_saved'], 512)
        self.assertTrue(report['host_file_staging'])
        self.assertFalse(report['direct_gpu_kv_transfer'])

    def test_decode_restores_then_uses_full_prompt_without_reprefill(self):
        report = self.execute('decode')
        self.assertTrue(report['pass'])
        self.assertEqual(self.server.calls[0][0], '/slots/0?action=restore')
        self.assertEqual(len(self.server.calls[1][1]['prompt']), 512)
        self.assertTrue(self.server.calls[1][1]['cache_prompt'])
        record = report['runs'][0]
        self.assertEqual(record['restore']['n_restored'], 512)
        self.assertGreaterEqual(record['timings']['cache_n'], 511)
        self.assertGreater(record['restore_and_decode_s'], record['restore_wall_s'])
        self.assertGreaterEqual(record['restore_to_first_token_s'], record['restore_wall_s'])

    def test_restore_count_failure_stops_before_completion(self):
        self.server.fault = 'restore-count'
        report = self.execute('decode')
        self.assertFalse(report['pass'])
        self.assertEqual(len(self.server.calls), 1)
        self.assertEqual(report['runs'][0]['restore']['n_restored'], 511)

    def test_save_count_failure_preserves_actual_server_count(self):
        self.server.fault = 'save-count'
        report = self.execute('prefill')
        self.assertFalse(report['pass'])
        self.assertEqual(report['runs'][0]['save']['n_saved'], 511)

    def test_prefill_decode_repetitions_use_matching_distinct_basenames(self):
        for action in ('prefill', 'decode'):
            report = self.execute(action, '--repetitions', '3')
            self.assertTrue(report['pass'])
            self.assertEqual([r['state_name'] for r in report['runs']],
                             ['trial-001.state', 'trial-002.state', 'trial-003.state'])

    def test_bad_cache_generation_and_truncated_sse_preserve_failure_evidence(self):
        for fault in ('cache-count', 'prediction-count', 'truncated', 'malformed', 'nonfinite'):
            with self.subTest(fault=fault):
                self.server.fault = fault
                report = self.execute()
                self.assertFalse(report['pass'])
                self.assertFalse(report['runs'][0]['pass'])
                self.assertTrue(report['runs'][0]['raw_sse_events'])
                self.assertIn('error', report)
        self.server.fault = 'decode-reprefill'
        self.assertFalse(self.execute('decode')['pass'])

    def test_slow_drip_obeys_whole_exchange_deadline(self):
        self.server.fault = 'slow-drip'
        started = time.monotonic()
        report = self.execute('baseline', '--timeout', '.12')
        self.assertFalse(report['pass'])
        self.assertIn('deadline', report['error'])
        self.assertLess(time.monotonic() - started, 2)
        self.assertIn('raw_sse_incomplete_tail', report['runs'][0])

    def test_response_capture_is_bounded(self):
        report = self.execute('baseline', '--max-response-bytes', '16')
        self.assertFalse(report['pass'])
        self.assertIn('byte limit', report['error'])
        self.assertFalse(report['runs'][0]['sse_capture_complete'])

    def test_existing_output_is_not_replaced_or_used_for_http_work(self):
        output = self.root / 'keep.json'; output.write_text('prior evidence')
        args = m.arguments(['prompts', '--url', self.url, '--output', str(output)])
        with self.assertRaises(FileExistsError):
            m.run(args)
        self.assertEqual(output.read_text(), 'prior evidence')
        self.assertEqual(self.server.calls, [])

    def test_bad_prompt_digest_fails_before_network(self):
        self.prompts.write_text(json.dumps({'prompts': {'512': {'tokens': [1] * 512, 'token_sha256': 'wrong'}}}))
        self.assertFalse(self.execute()['pass'])
        self.assertEqual(self.server.calls, [])

    def test_state_names_are_basenames_and_urls_have_no_credentials(self):
        import argparse
        for value in ('../state', '/tmp/state', '.hidden', 'a/b', 'a\\b', '', 'name?query'):
            with self.subTest(value=value), self.assertRaises(argparse.ArgumentTypeError):
                m.state_basename(value)
        for value in ('http://user:password@example.invalid', 'ftp://example.invalid',
                      'http://example.invalid/path', 'http://example.invalid?query'):
            with self.subTest(value=value), self.assertRaises(argparse.ArgumentTypeError):
                m.server_url(value)

    def test_console_prints_only_outcome(self):
        output = self.root / 'console.json'
        console = io.StringIO()
        with redirect_stdout(console):
            result = m.main(['baseline', '--url', self.url, '--prompts', str(self.prompts),
                             '--output', str(output), '--repetitions', '1'])
        self.assertEqual(result, 0)
        self.assertEqual(console.getvalue(), 'INFERENCE_MEASUREMENT pass=1 action=baseline\n')


if __name__ == '__main__':
    unittest.main()
