"""Regression for queued-document priority and bounded-memory file responses."""
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
import urllib.error
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from PIL import Image
from server_integration import request, ready, finished


class Handler(BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'

    def log_message(self, *_): pass

    def do_POST(self):
        data = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
        pixel = Image.open(io.BytesIO(base64.b64decode(data['image'].split(',')[1]))).getpixel((0, 0))[0]
        self.server.order.append(pixel)
        if pixel == 10:
            self.server.entered.set()
            self.server.release.wait(15)
        body = json.dumps({'text': str(pixel)}).encode()
        self.send_response(200); self.send_header('Content-Length', str(len(body)))
        self.end_headers(); self.wfile.write(body)


def run(binary):
    backend = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
    backend.daemon_threads = True
    backend.order = []
    backend.entered = threading.Event(); backend.release = threading.Event()
    thread = threading.Thread(target=backend.serve_forever, daemon=True); thread.start()
    try:
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            for i in range(7):
                (root/f'{i}.ppm').write_bytes(b'P6\n16 16\n255\n' + bytes([10+i, 0, 0])*256)
            config = {'version': 1, 'layout': {'provider': 'normalized', 'model': 'layout'},
                      'models': {'layout': {'backend': 'mock', 'response': {'boxes': [
                          {'type': 'text', 'bbox': [0, 0, 16, 16]}]}},
                          'ocr': {'backend': 'http_json', 'endpoint': f'http://127.0.0.1:{backend.server_port}/ocr'}},
                      'routes': {'text': {'model': 'ocr', 'save_crop': True}}}
            path = root/'config.json'; path.write_text(json.dumps(config))
            with socket.socket() as sock:
                sock.bind(('127.0.0.1', 0)); port = sock.getsockname()[1]
            base = f'http://127.0.0.1:{port}'
            with open(root/'log', 'w') as log:
                proc = subprocess.Popen([binary, '--config', str(path), '--data-dir', str(root/'state'),
                    '--allowed-input-root', str(root), '--port', str(port), '--document-workers', '1',
                    '--page-workers', '1', '--max-queued-pages', '1'], stdout=log, stderr=log)
                try:
                    ready(base, proc)
                    def submit(i, priority=0):
                        code, job = request(base, '/v1/jobs', method='POST',
                            data=json.dumps({'path': str(root/f'{i}.ppm'), 'priority': priority}).encode())
                        assert code == 202, (code, job)
                        return job['id']
                    def wait_state(identifier, expected):
                        for _ in range(200):
                            if request(base, '/v1/jobs/'+identifier)[1]['status'] == expected: return
                            time.sleep(.01)
                        raise AssertionError('state not reached: '+expected)
                    ids = [submit(0)]
                    assert backend.entered.wait(5)
                    ids.append(submit(1)); wait_state(ids[1], 'processing')
                    ids.append(submit(2)); wait_state(ids[2], 'reading')
                    # Reader is blocked by the full page queue; these stay queued.
                    ids += [submit(3, 0), submit(4, 100), submit(5, 100), submit(6, 50)]
                    for identifier in ids[3:]: wait_state(identifier, 'queued')
                    backend.release.set()
                    for identifier in ids: assert finished(base, identifier)['status'] == 'succeeded'
                    assert backend.order == [10, 11, 12, 14, 15, 16, 13], backend.order
                    job = ids[0]
                    out = root/'state/jobs'/job/'output'
                    code, result = request(base, f'/v1/jobs/{job}/result')
                    assert code == 200 and result['pages'][0]['blocks'][0]['text'] == '10'
                    asset = result['pages'][0]['blocks'][0]['asset'].split('/')[-1]
                    with urllib.request.urlopen(base+f'/v1/jobs/{job}/assets/{asset}') as response:
                        assert response.headers['Content-Type'] == 'image/png'
                        assert response.read() == (out/'assets'/asset).read_bytes()
                    # Replace only the test fixture's completed output with a large
                    # file. Downloads must match bytes and not leak descriptors.
                    payload = b'large Markdown result\n' * 65536
                    (out/'result.md').write_bytes(payload)
                    def download(_):
                        with urllib.request.urlopen(base+f'/v1/jobs/{job}/result?format=markdown') as response:
                            assert int(response.headers['Content-Length']) == len(payload)
                            assert response.headers['Cache-Control'] == 'no-store'
                            assert response.read() == payload
                    download(0)
                    before = len(list(Path(f'/proc/{proc.pid}/fd').iterdir()))
                    with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
                        list(pool.map(download, range(32)))
                    # Also exercise early disconnect and descriptor release.
                    for _ in range(8):
                        with urllib.request.urlopen(base+f'/v1/jobs/{job}/result?format=markdown') as response:
                            response.read(1)
                    for _ in range(200):
                        after = len(list(Path(f'/proc/{proc.pid}/fd').iterdir()))
                        if after <= before + 2: break
                        time.sleep(.01)
                    assert after <= before + 2, (before, after)
                    with open(out/'result.md', 'wb') as f: f.truncate(64*1024*1024 + 1)
                    assert request(base, f'/v1/jobs/{job}/result?format=markdown')[0] == 400
                    (out/'result.md').unlink()
                    (out/'result.md').symlink_to(root/'config.json')
                    assert request(base, f'/v1/jobs/{job}/result?format=markdown')[0] == 400
                finally:
                    backend.release.set(); proc.terminate()
                    try: proc.wait(timeout=10)
                    except subprocess.TimeoutExpired: proc.kill(); proc.wait()
                    assert proc.returncode == 0, proc.returncode
    finally:
        backend.release.set(); backend.shutdown(); backend.server_close(); thread.join()
    print('PASS: queued priority/FIFO, JSON/assets/large streaming responses, disconnect cleanup and size limits')


if __name__ == '__main__':
    run(str(Path(sys.argv[1]).resolve()))
