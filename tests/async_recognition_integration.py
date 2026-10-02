"""Independent preparation, bounded admission, cancellation and model diagnostics."""
import json
import socket
import subprocess
import sys
import tempfile
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from server_integration import ready, request, finished


class Backend(BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'
    def log_message(self, *_):
        pass
    def do_POST(self):
        payload = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
        prompt = payload['messages'][-1]['content'][-1]['text']
        with self.server.lock:
            self.server.active += 1
            self.server.peak = max(self.server.peak, self.server.active)
            self.server.calls.append(prompt)
        try:
            self.server.release.wait(8)
            if self.path == '/fallback' and prompt == 'box-3':
                status, body = 503, b'{"error":"busy"}'
            elif self.path == '/timeout':
                time.sleep(1.3)
                status, body = 200, b'{}'
            elif self.path == '/bad-json':
                status, body = 200, b'not json'
            else:
                time.sleep(.025)
                status = 200
                body = json.dumps({'choices': [{'finish_reason': 'stop', 'message': {'content': prompt}}],
                    'usage': {'total_tokens': 110, 'completion_tokens': 10}}).encode()
            self.send_response(status)
            self.send_header('Content-Length', str(len(body)))
            self.end_headers()
            self.wfile.write(body)
        except (BrokenPipeError, ConnectionResetError):
            pass
        finally:
            with self.server.lock:
                self.server.active -= 1


def poll(check, timeout=6):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        value = check()
        if value:
            return value
        time.sleep(.02)
    raise AssertionError('condition not reached')


def run(binary, mode):
    backend = ThreadingHTTPServer(('127.0.0.1', 0), Backend)
    backend.daemon_threads = True
    backend.lock = threading.Lock()
    backend.release = threading.Event()
    backend.active = backend.peak = 0
    backend.calls = []
    thread = threading.Thread(target=backend.serve_forever, daemon=True)
    thread.start()
    try:
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            sample = root / 'sample.ppm'
            sample.write_bytes(b'P6\n20 10\n255\n' + b'x' * 600)
            with socket.socket() as sock:
                sock.bind(('127.0.0.1', 0))
                port = sock.getsockname()[1]
            base = f'http://127.0.0.1:{port}'
            labels = [f'box-{i}' for i in range(24 if mode in ('fixed', 'visual', 'cancel', 'cross-cancel') else 8)]
            path = '/fallback' if mode == 'fallback' else '/timeout' if mode == 'timeout' else '/ocr'
            ocr = {'backend': 'vllm', 'model': 'fixture', 'max_concurrent_requests': 4,
                   'endpoint': f'http://127.0.0.1:{backend.server_port}{path}', 'timeout_seconds': 1}
            if mode in ('visual', 'cancel', 'cross-cancel'):
                ocr['vllm_visual_scheduler'] = {'enabled': True, 'max_num_seqs': 128,
                    'max_model_len': 8192, 'max_num_batched_tokens': 16384,
                    'visual_pixels_per_token': 784, 'max_visual_tokens': 1280,
                    'cudagraph_capture_sizes': [1, 2, 4, 8, 16, 32, 64, 128], 'max_wait_ms': 10}
            async_settings = {'enabled': True, 'workers': 8, 'max_requests': 12,
                              'max_bytes': 7200, 'enqueue_timeout_ms': 3000}
            if mode == 'admission-timeout':
                async_settings.update(workers=1, max_requests=1, enqueue_timeout_ms=25)
            if mode == 'response-limit':
                ocr['max_response_bytes'] = 16
            if mode == 'connection':
                with socket.socket() as sock:
                    sock.bind(('127.0.0.1', 0))
                    unused_port = sock.getsockname()[1]
                ocr['endpoint'] = f'http://127.0.0.1:{unused_port}/ocr'
            if mode == 'oversized':
                async_settings['max_bytes'] = 599
            config = {'version': 1,
                'execution': {'box_workers': 1, 'page_workers': 2 if mode == 'cross-cancel' else 1, 'on_error': 'record',
                              'async_recognition': async_settings},
                'layout': {'provider': 'normalized', 'model': 'layout', 'coordinates': 'normalized'},
                'models': {'layout': {'backend': 'mock', 'response': {'boxes': [
                    {'type': label, 'bbox': [0, 0, 1, 1], 'order': i} for i, label in enumerate(labels)]}},
                    'ocr': ocr},
                'routes': {label: {'model': 'ocr', 'prompt': label} for label in labels},
                'server': {'port': port, 'data_dir': str(root / 'state'), 'allowed_input_root': str(root)}}
            if mode == 'fallback':
                config['models']['backup'] = {'backend': 'mock', 'response': {'text': 'recovered'}}
                config['routes']['box-3'] = {'models': ['ocr', 'backup'], 'prompt': 'box-3'}
            if mode == 'fail':
                config['execution']['on_error'] = 'fail'
                config['models']['ocr']['endpoint'] = f'http://127.0.0.1:{backend.server_port}/bad-json'
            cfg = root / 'config.json'
            cfg.write_text(json.dumps(config))
            with open(root / 'server.log', 'w') as log:
                proc = subprocess.Popen([binary, '--config', str(cfg)], stdout=log, stderr=log)
                try:
                    ready(base, proc)
                    code, submitted = request(base, '/v1/jobs', method='POST',
                        data=json.dumps({'path': str(sample)}).encode())
                    assert code == 202, (code, submitted)
                    second = None
                    if mode == 'cross-cancel':
                        code, second = request(base, '/v1/jobs', method='POST',
                            data=json.dumps({'path': str(sample)}).encode())
                        assert code == 202
                    if mode in ('fixed', 'visual', 'cancel', 'cross-cancel'):
                        def saturated():
                            stats = request(base, '/v1/metrics')[1]
                            return stats if stats['models']['ocr']['inflight'] == 4 and stats['models']['ocr']['queued'] > 0 else None
                        stats = poll(saturated)
                        assert stats['recognition']['admitted'] <= 12 and stats['recognition']['admitted_bytes'] <= 7200
                        assert stats['models']['ocr']['concurrency_limit'] == 4
                        if mode in ('cancel', 'cross-cancel'):
                            assert request(base, f"/v1/jobs/{submitted['id']}/cancel", method='POST', data=b'')[0] in (200, 202)
                            poll(lambda: request(base, f"/v1/jobs/{submitted['id']}")[1]['status'] == 'cancelled')
                    elif mode == 'admission-timeout':
                        poll(lambda: request(base, '/v1/metrics')[1]['recognition']['admission_timeout_total'] > 0)
                    backend.release.set()
                    if second:
                        assert finished(base, second['id'])['status'] == 'succeeded'
                    if mode not in ('cancel', 'cross-cancel'):
                        state = finished(base, submitted['id'])
                        assert state['status'] == ('failed' if mode == 'fail' else 'succeeded'), state
                        if mode != 'fail':
                            blocks = request(base, f"/v1/jobs/{submitted['id']}/result")[1]['pages'][0]['blocks']
                            if mode in ('fixed', 'visual', 'fallback'):
                                expected = ['recovered' if mode == 'fallback' and label == 'box-3' else label for label in labels]
                                assert [block['text'] for block in blocks] == expected, blocks
                    def drained():
                        stats = request(base, '/v1/metrics')[1]
                        return stats if stats['recognition']['admitted'] == 0 else None
                    stats = poll(drained)
                    model = stats['models']['ocr']
                    assert model['inflight'] == model['queued'] == 0, stats
                    assert stats['recognition']['admitted_bytes'] == 0
                    assert stats['recognition']['peak_admitted_bytes'] <= async_settings['max_bytes']
                    assert stats['recognition']['peak_admitted'] <= async_settings['max_requests']
                    if mode in ('fixed', 'visual'):
                        assert backend.peak == 4, backend.peak
                        assert model['successful_total'] == len(labels) and not model['failure_counts'], model
                        assert model['actual_prompt_tokens_total'] == 100 * len(labels)
                        assert model['actual_completion_tokens_total'] == 10 * len(labels)
                        if mode == 'visual':
                            assert sum(model['visual_wave_size_histogram'].values()) == model['visual_waves_total']
                            assert sum(int(k) * n for k, n in model['visual_wave_size_histogram'].items()) == model['visual_wave_requests_total']
                            assert sum(model['visual_capture_size_histogram'].values()) == model['visual_waves_total']
                    elif mode == 'fallback':
                        assert model['failure_counts']['http_503'] == 1 and model['failed_total'] == 1, model
                    elif mode in ('connection', 'response-limit'):
                        kind = 'http_connection' if mode == 'connection' else 'http_response_limit'
                        assert model['failure_counts'][kind] == len(labels), model
                    elif mode == 'timeout':
                        assert model['failure_counts']['http_timeout'] == len(labels), model
                    elif mode == 'oversized':
                        assert stats['recognition']['oversized_total'] == len(labels) and not backend.calls, stats
                    elif mode in ('cancel', 'cross-cancel'):
                        assert stats['recognition']['cancelled_total'] > 0, stats
                        # The same pools must accept another job after cancellation.
                        code, recovered = request(base, '/v1/jobs', method='POST', data=json.dumps({'path': str(sample)}).encode())
                        assert code == 202 and finished(base, recovered['id'])['status'] == 'succeeded'
                    elif mode == 'fail':
                        assert model['failure_counts']['model_error'] >= 1
                finally:
                    backend.release.set()
                    proc.terminate()
                    try:
                        proc.wait(timeout=10)
                    except subprocess.TimeoutExpired:
                        proc.kill(); proc.wait()
    finally:
        backend.release.set()
        backend.shutdown(); backend.server_close(); thread.join()
    print('PASS: async recognition ' + mode)


if __name__ == '__main__':
    binary = str(Path(sys.argv[1]).resolve())
    for mode in ('fixed', 'visual', 'fallback', 'oversized', 'admission-timeout', 'timeout', 'connection', 'response-limit', 'cancel', 'cross-cancel', 'fail'):
        run(binary, mode)
