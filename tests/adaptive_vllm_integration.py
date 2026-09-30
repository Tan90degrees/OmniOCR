"""Adaptive admission sends individual, variable-size BOX requests to vLLM."""
import base64
import json
import socket
import subprocess
import sys
import tempfile
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

from server_integration import finished, ready, request


class Handler(BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'

    def log_message(self, *_):
        pass

    def do_POST(self):
        try:
            payload = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
            assert self.path == '/v1/chat/completions'
            assert payload['model'] == 'fixture' and payload['stream'] is False
            content = payload['messages'][-1]['content']
            assert len([part for part in content if part['type'] == 'image_url']) == 1
            url = next(part['image_url']['url'] for part in content if part['type'] == 'image_url')
            png = base64.b64decode(url.split(',', 1)[1])
            assert png[:8] == b'\x89PNG\r\n\x1a\n'
            width = int.from_bytes(png[16:20], 'big')
            prompt = next(part['text'] for part in content if part['type'] == 'text')
            with self.server.lock:
                self.server.active += 1
                self.server.peak = max(self.server.peak, self.server.active)
                self.server.calls.append(prompt)
                self.server.widths.add(width)
            try:
                time.sleep(.07)
                body = json.dumps({'choices': [{'finish_reason': 'stop',
                    'message': {'content': prompt}}],
                    'usage': {'total_tokens': 300, 'completion_tokens': 42}}).encode()
                self.send_response(200)
                self.send_header('Content-Length', str(len(body)))
                self.end_headers()
                self.wfile.write(body)
            finally:
                with self.server.lock:
                    self.server.active -= 1
        except Exception as exc:
            with self.server.lock:
                self.server.errors.append(str(exc))
            self.send_error(500)


def run(binary, visual=False):
    backend = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
    backend.daemon_threads = True
    backend.lock = threading.Lock()
    backend.active = backend.peak = 0
    backend.calls = []
    backend.widths = set()
    backend.errors = []
    thread = threading.Thread(target=backend.serve_forever, daemon=True)
    thread.start()
    try:
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            sample = root / 'document.ppm'
            sample.write_bytes(b'P6\n160 160\n255\n' + b'\xff' * (160 * 160 * 3))
            labels = [f'region-{i}' for i in range(20)]
            boxes = [{'type': label, 'bbox': [0, i / 20, (i % 4 + 1) / 4, (i + 1) / 20],
                      'order': i} for i, label in enumerate(labels)]
            with socket.socket() as sock:
                sock.bind(('127.0.0.1', 0))
                port = sock.getsockname()[1]
            base = f'http://127.0.0.1:{port}'
            config = {'version': 1,
                'execution': {'page_workers': 1, 'box_workers': 20,
                              'document_workers': 1, 'max_queued_pages': 2},
                'layout': {'provider': 'normalized', 'model': 'layout', 'coordinates': 'normalized'},
                'models': {
                    'layout': {'backend': 'mock', 'response': {'boxes': boxes}},
                    'ocr': {'backend': 'vllm', 'instances': 1,
                        'max_concurrent_requests': 4, 'model': 'fixture',
                        'endpoint': f'http://127.0.0.1:{backend.server_port}/v1/chat/completions',
                        **({'vllm_visual_scheduler': {'enabled': True, 'max_num_seqs': 4,
                            'max_model_len': 8192, 'max_num_batched_tokens': 4096,
                            'cudagraph_capture_sizes': [1, 2, 4],
                            'visual_pixels_per_token': 16, 'bucket_edges': [32, 128],
                            'max_wait_ms': 2}} if visual else
                          {'adaptive_concurrency': {'enabled': True, 'initial_concurrency': 1,
                            'window_ms': 100, 'min_samples': 2, 'token_budget': 10000}})}},
                'routes': {label: {'model': 'ocr', 'prompt': label} for label in labels},
                'server': {'port': port, 'data_dir': str(root / 'state'),
                           'allowed_input_root': str(root), 'max_jobs': 1}}
            cfg = root / 'config.json'
            cfg.write_text(json.dumps(config))
            with open(root / 'server.log', 'w') as log:
                proc = subprocess.Popen([binary, '--config', str(cfg)], stdout=log, stderr=log)
                try:
                    ready(base, proc)
                    code, submitted = request(base, '/v1/jobs', method='POST',
                                              data=json.dumps({'path': str(sample)}).encode())
                    assert code == 202, (code, submitted)
                    assert finished(base, submitted['id'])['status'] == 'succeeded'
                    code, result = request(base, f"/v1/jobs/{submitted['id']}/result")
                    assert code == 200, (code, result)
                    assert [block['text'] for block in result['pages'][0]['blocks']] == labels
                    code, stats = request(base, '/v1/metrics')
                    assert code == 200, (code, stats)
                    model = stats['models']['ocr']
                    if visual:
                        assert model['strategy'] == 'visual_bucket' and model['concurrency_limit'] == 4, model
                        assert sum(model['visual_bucket_dispatched']) == 20, model
                        assert model['visual_waves_total'] > 0 and model['peak_inflight_visual_tokens'] > 0, model
                    else:
                        assert model['strategy'] == 'adaptive' and 2 <= model['concurrency_limit'] <= 4, model
                    assert model['completed_total'] == 20 and model['inflight'] == 0, model
                    assert 20 * 42 < model['completed_normalized_work_total'] < 20 * 512, model
                    if not visual:
                        assert 0 < model['current_token_budget'] <= model['token_budget'], model
                        assert 0.5 <= model['token_estimate_scale'] < 1.0, model
                    assert sorted(backend.calls) == sorted(labels) and backend.widths == {40, 80, 120, 160}
                    assert 2 <= backend.peak <= 4
                    assert not backend.errors, backend.errors
                finally:
                    proc.terminate()
                    try: proc.wait(timeout=10)
                    except subprocess.TimeoutExpired: proc.kill(); proc.wait()
    finally:
        backend.shutdown(); backend.server_close(); thread.join()
    print('PASS: ' + ('visual bucket' if visual else 'adaptive') + ' vLLM requests and REST metrics')


if __name__ == '__main__':
    run(str(Path(sys.argv[1]).resolve()))
    run(str(Path(sys.argv[1]).resolve()), visual=True)
