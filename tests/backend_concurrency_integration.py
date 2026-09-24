"""One served VLM accepts concurrent requests without duplicating its deployment."""
import json
import subprocess
import sys
import tempfile
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path


class Handler(BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'

    def log_message(self, *_):
        pass

    def do_POST(self):
        try:
            request = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
            assert self.path == '/v1/chat/completions'
            assert request['model'] == 'served-vlm' and request['stream'] is False
            with self.server.lock:
                self.server.requests += 1
                sequence = self.server.requests
                self.server.active += 1
                self.server.peak = max(self.server.peak, self.server.active)
                concurrent = self.server.concurrent
            try:
                # A gate makes the expected four simultaneous backend calls
                # deterministic; a single leased slot would time out here.
                if concurrent and sequence <= 4:
                    self.server.gate.wait(timeout=5)
                time.sleep(.02)
                body = json.dumps({'choices': [{'finish_reason': 'stop',
                    'message': {'content': 'recognized'}}]}).encode()
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


def run(binary):
    backend = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
    backend.daemon_threads = True
    backend.lock = threading.Lock()
    backend.errors = []
    thread = threading.Thread(target=backend.serve_forever, daemon=True)
    thread.start()
    try:
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            sample = root / 'input.ppm'
            sample.write_bytes(b'P6\n16 16\n255\n' + b'\xff' * 768)
            config = {'version': 1, 'execution': {'workers': 8},
                'layout': {'provider': 'normalized', 'model': 'layout',
                           'coordinates': 'normalized'},
                'models': {
                    'layout': {'backend': 'mock', 'response': {'boxes': [
                        {'type': 'text', 'bbox': [0, 0, 1, 1]}]}},
                    'ocr': {'backend': 'vllm', 'instances': 1, 'model': 'served-vlm',
                            'endpoint': f'http://127.0.0.1:{backend.server_port}/v1/chat/completions'}},
                'routes': {'text': {'model': 'ocr'}}}
            manifest = root / 'jobs.json'
            cfg = root / 'config.json'
            for concurrency in (1, 4):
                jobs = {'options': {'page_workers': 8, 'max_active_documents': 8,
                                    'max_queued_pages': 8},
                        'jobs': [{'input': str(sample),
                                  'output': str(root / f'out-{concurrency}-{i}')}
                                 for i in range(8)]}
                backend.requests = backend.active = backend.peak = 0
                backend.concurrent = concurrency > 1
                backend.gate = threading.Barrier(4, timeout=5)
                config['models']['ocr']['max_concurrent_requests'] = concurrency
                cfg.write_text(json.dumps(config))
                manifest.write_text(json.dumps(jobs))
                result = subprocess.run([binary, '--config', str(cfg), '--batch', str(manifest)],
                                        capture_output=True, text=True, timeout=30)
                assert result.returncode == 0, (result.stdout, result.stderr)
                for job in jobs['jobs']:
                    output = Path(job['output']) / 'result.json'
                    assert json.loads(output.read_text())['pages'][0]['blocks'][0]['text'] == 'recognized'
                assert backend.requests == 8 and backend.peak == concurrency, (
                    concurrency, backend.requests, backend.peak)
            assert not backend.errors, backend.errors
    finally:
        backend.shutdown(); backend.server_close(); thread.join()
    print('PASS: one VLM deployment handles configured concurrent backend requests')


if __name__ == '__main__':
    run(str(Path(sys.argv[1]).resolve()))
