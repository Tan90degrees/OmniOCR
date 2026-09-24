"""Different documents and BOX types share one native HTTP batch invocation."""
import base64
import concurrent.futures
import io
import json
import socket
import subprocess
import sys
import tempfile
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from PIL import Image

from server_integration import finished, ready, request


class Handler(BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'

    def log_message(self, *_): pass

    def do_POST(self):
        if self.path != '/batch':
            self.send_error(500, 'unexpected scalar request')
            return
        payload = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
        assert 1 <= len(payload['requests']) <= 4
        pixels = []
        for item in payload['requests']:
            image = Image.open(io.BytesIO(base64.b64decode(item['image'].split(',', 1)[1])))
            pixels.append(image.getpixel((0, 0))[0])
        with self.server.lock:
            self.server.batches.append(pixels)
            sequence = len(self.server.batches)
            self.server.active += 1
            self.server.peak = max(self.server.peak, self.server.active)
        try:
            if sequence <= 2 and self.server.gate:
                self.server.gate.wait(timeout=8)
            time.sleep(.02)
            body = json.dumps({'results': [{'text': str(pixel)} for pixel in pixels]}).encode()
        finally:
            with self.server.lock:
                self.server.active -= 1
        self.send_response(200)
        self.send_header('Content-Type', 'application/json')
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        self.wfile.write(body)


def run(binary):
    backend = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
    backend.daemon_threads = True
    backend.lock = threading.Lock()
    backend.batches = []
    backend.active = backend.peak = 0
    backend.gate = threading.Barrier(2, timeout=8)
    thread = threading.Thread(target=backend.serve_forever, daemon=True)
    thread.start()
    try:
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            config = {'version': 1,
                'layout': {'provider': 'normalized', 'model': 'layout', 'coordinates': 'normalized'},
                'models': {'layout': {'backend': 'mock', 'response': {'boxes': [
                    {'type': 'title', 'bbox': [0, 0, 1, .5], 'order': 0},
                    {'type': 'text', 'bbox': [0, .5, 1, 1], 'order': 1}]}},
                    'ocr': {'backend': 'http_json', 'instances': 1,
                        'max_concurrent_requests': 2, 'batch_size': 4,
                        'max_batch_wait_ms': 100,
                        'endpoint': f'http://127.0.0.1:{backend.server_port}/single',
                        'batch_endpoint': f'http://127.0.0.1:{backend.server_port}/batch'}},
                'routes': {'title': {'model': 'ocr'}, 'text': {'model': 'ocr'}}}
            (root/'config.json').write_text(json.dumps(config))
            for i in range(8):
                top = bytes([i+10, 0, 0]) * (16 * 8)
                bottom = bytes([i+110, 0, 0]) * (16 * 8)
                (root/f'{i}.ppm').write_bytes(b'P6\n16 16\n255\n' + top + bottom)
            with socket.socket() as sock:
                sock.bind(('127.0.0.1', 0)); port = sock.getsockname()[1]
            base = f'http://127.0.0.1:{port}'
            with open(root/'log', 'w') as log:
                proc = subprocess.Popen([binary, '--config', str(root/'config.json'),
                    '--data-dir', str(root/'state'), '--allowed-input-root', str(root),
                    '--port', str(port), '--page-workers', '8', '--document-workers', '4',
                    '--max-queued-pages', '8'], stdout=log, stderr=log)
                try:
                    ready(base, proc)
                    def submit(i):
                        code, response = request(base, '/v1/jobs', method='POST',
                            data=json.dumps({'path': str(root/f'{i}.ppm')}).encode())
                        assert code == 202, (code, response)
                        return response['id']
                    with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
                        identifiers = list(pool.map(submit, range(8)))
                    for i, identifier in enumerate(identifiers):
                        assert finished(base, identifier)['status'] == 'succeeded'
                        code, output = request(base, f'/v1/jobs/{identifier}/result')
                        assert code == 200 and [b['text'] for b in output['pages'][0]['blocks']] == [
                            str(i+10), str(i+110)], (i, output)
                    assert len(backend.batches) < 16, backend.batches
                    assert any(len({value % 100 for value in batch}) > 1 for batch in backend.batches), backend.batches
                    assert sum(map(len, backend.batches)) == 16, backend.batches
                    assert backend.peak == 2, backend.peak
                finally:
                    proc.terminate()
                    try: proc.wait(timeout=10)
                    except subprocess.TimeoutExpired: proc.kill(); proc.wait()
                    assert proc.returncode == 0, proc.returncode
            # One page can now form a full native batch from its own BOXes.
            with backend.lock:
                backend.batches.clear()
                backend.peak = 0
            backend.gate = None
            config['models']['ocr']['max_concurrent_requests'] = 1
            config['models']['layout']['response']['boxes'] = [
                {'type': 'text', 'bbox': [0, i/4, 1, (i+1)/4], 'order': i}
                for i in range(4)]
            (root/'config.json').write_text(json.dumps(config))
            with socket.socket() as sock:
                sock.bind(('127.0.0.1', 0)); port = sock.getsockname()[1]
            base = f'http://127.0.0.1:{port}'
            with open(root/'single-page.log', 'w') as log:
                proc = subprocess.Popen([binary, '--config', str(root/'config.json'),
                    '--data-dir', str(root/'single-page-state'), '--allowed-input-root', str(root),
                    '--port', str(port), '--page-workers', '1', '--box-workers', '4',
                    '--document-workers', '1'], stdout=log, stderr=log)
                try:
                    ready(base, proc)
                    code, response = request(base, '/v1/jobs', method='POST',
                        data=json.dumps({'path': str(root/'3.ppm')}).encode())
                    assert code == 202, (code, response)
                    assert finished(base, response['id'])['status'] == 'succeeded'
                    code, output = request(base, f"/v1/jobs/{response['id']}/result")
                    assert code == 200 and [b['text'] for b in output['pages'][0]['blocks']] == [
                        '13', '13', '113', '113'], (code, output)
                    assert len(backend.batches) == 1 and len(backend.batches[0]) == 4, backend.batches
                finally:
                    proc.terminate()
                    try: proc.wait(timeout=10)
                    except subprocess.TimeoutExpired: proc.kill(); proc.wait()
                    assert proc.returncode == 0, proc.returncode
    finally:
        backend.shutdown(); backend.server_close(); thread.join()
    print('PASS: cross-file and single-page BOX batches preserve ordered results')


if __name__ == '__main__':
    run(str(Path(sys.argv[1]).resolve()))
