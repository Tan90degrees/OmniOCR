"""Bounded admission, upload-byte accounting, and terminal metrics under load."""
import json
import concurrent.futures
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

from server_integration import finished, ready, request


class Backend(BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'

    def log_message(self, *_): pass

    def do_POST(self):
        self.rfile.read(int(self.headers['Content-Length']))
        self.server.entered.set()
        self.server.release.wait(10)
        body = b'{"text":"ok"}'
        self.send_response(200)
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        self.wfile.write(body)


def until(predicate, seconds=5):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        if predicate(): return
        time.sleep(.02)
    raise AssertionError('condition timed out')


def run(binary):
    backend = ThreadingHTTPServer(('127.0.0.1', 0), Backend)
    backend.daemon_threads = True
    backend.entered, backend.release = threading.Event(), threading.Event()
    thread = threading.Thread(target=backend.serve_forever, daemon=True)
    thread.start()
    try:
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            image = b'P6\n16 16\n255\n' + bytes([10, 20, 30]) * 256
            (root/'source.ppm').write_bytes(image)
            config = {'version': 1,
                      'layout': {'provider': 'normalized', 'model': 'layout'},
                      'models': {'layout': {'backend': 'mock', 'response': {'boxes': [
                          {'type': 'text', 'bbox': [0, 0, 16, 16]}]}},
                          'ocr': {'backend': 'http_json', 'instances': 2,
                                  'endpoint': f'http://127.0.0.1:{backend.server_port}/ocr'}},
                      'routes': {'text': {'model': 'ocr'}}}
            (root/'config.json').write_text(json.dumps(config))
            with socket.socket() as sock:
                sock.bind(('127.0.0.1', 0))
                port = sock.getsockname()[1]
            base = f'http://127.0.0.1:{port}'
            with open(root/'log', 'w') as log:
                proc = subprocess.Popen([binary, '--config', str(root/'config.json'),
                    '--data-dir', str(root/'state'), '--allowed-input-root', str(root),
                    '--port', str(port), '--page-workers', '2', '--document-workers', '2',
                    '--max-active-jobs', '2', '--max-jobs', '3',
                    '--max-inflight-upload-bytes', '1000'], stdout=log, stderr=log)
                try:
                    ready(base, proc)
                    metrics = lambda: request(base, '/v1/metrics')[1]
                    assert metrics()['active_jobs'] == 0
                    # Aborted uploads return both their slot and charged bytes.
                    partial = socket.create_connection(('127.0.0.1', port))
                    partial.sendall((f'POST /v1/jobs/upload?extension=.ppm HTTP/1.1\r\n'
                        f'Host: 127.0.0.1:{port}\r\nContent-Length: {len(image)}\r\n'
                        'Connection: close\r\n\r\n').encode() + image[:128])
                    until(lambda: metrics()['inflight_upload_bytes'] == 128)
                    assert metrics()['reserved_jobs'] == 1
                    partial.close()
                    until(lambda: metrics()['reserved_jobs'] == 0 and
                          metrics()['inflight_upload_bytes'] == 0)
                    assert not list((root/'state/jobs').iterdir())

                    code, first = request(base, '/v1/jobs/upload?extension=.ppm',
                                          method='POST', data=image)
                    assert code == 202, (code, first)
                    assert backend.entered.wait(5)
                    assert metrics()['inflight_upload_bytes'] == len(image)
                    # Capacity for jobs remains, while the byte budget is full.
                    code, rejected = request(base, '/v1/jobs/upload?extension=.ppm',
                                             method='POST', data=image)
                    assert code == 429 and 'upload byte' in rejected['error'], (code, rejected)
                    code, second = request(base, '/v1/jobs', method='POST',
                                           data=json.dumps({'path': str(root/'source.ppm')}).encode())
                    assert code == 202, (code, second)
                    until(lambda: metrics()['active_jobs'] == 2)
                    req = urllib.request.Request(base+'/v1/jobs', method='POST',
                        data=json.dumps({'path': str(root/'source.ppm')}).encode())
                    try:
                        urllib.request.urlopen(req, timeout=5)
                        raise AssertionError('active capacity should reject')
                    except urllib.error.HTTPError as error:
                        assert error.code == 429 and error.headers['Retry-After'] == '1'
                        assert json.loads(error.read())['error'] == 'active job limit reached'
                    def burst(_):
                        return request(base, '/v1/jobs', method='POST',
                            data=json.dumps({'path': str(root/'source.ppm')}).encode())[0]
                    with concurrent.futures.ThreadPoolExecutor(max_workers=24) as pool:
                        assert list(pool.map(burst, range(48))) == [429] * 48
                    during = metrics()
                    assert during['admitted_total'] == 2 and during['active_jobs'] == 2
                    assert during['inflight_upload_bytes'] == len(image)
                    assert during['rejected_total'] == 50, during
                    backend.release.set()
                    assert finished(base, first['id'])['status'] == 'succeeded'
                    assert finished(base, second['id'])['status'] == 'succeeded'
                    until(lambda: metrics()['active_jobs'] == 0 and
                          metrics()['inflight_upload_bytes'] == 0)
                    assert request(base, '/v1/jobs', method='POST', data=b'{invalid')[0] == 400
                    until(lambda: metrics()['reserved_jobs'] == 0)
                    code, third = request(base, '/v1/jobs/upload?extension=.ppm',
                                          method='POST', data=image)
                    assert code == 202, (code, third)
                    assert finished(base, third['id'])['status'] == 'succeeded'
                    code, exhausted = request(base, '/v1/jobs', method='POST',
                                              data=json.dumps({'path': str(root/'source.ppm')}).encode())
                    assert code == 503 and 'cumulative' in exhausted['error']
                    end = metrics()
                    assert end['admitted_total'] == end['succeeded_total'] == 3, end
                    assert end['failed_total'] == end['active_jobs'] == 0, end
                    assert end['reserved_jobs'] == end['inflight_upload_bytes'] == 0, end
                    assert end['rejected_total'] == 51, end
                    assert len(list((root/'state/jobs').iterdir())) == 3
                finally:
                    backend.release.set()
                    proc.terminate()
                    try: proc.wait(timeout=10)
                    except subprocess.TimeoutExpired: proc.kill(); proc.wait()
                    assert proc.returncode == 0, proc.returncode
    finally:
        backend.release.set()
        backend.shutdown(); backend.server_close(); thread.join()
    print('PASS: active slots, upload bytes, early disconnect, metrics and cumulative limit')


def run_page_budget(binary):
    backend = ThreadingHTTPServer(('127.0.0.1', 0), Backend)
    backend.daemon_threads = True
    backend.entered, backend.release = threading.Event(), threading.Event()
    thread = threading.Thread(target=backend.serve_forever, daemon=True)
    thread.start()
    try:
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            for i in range(3):
                (root/f'{i}.ppm').write_bytes(b'P6\n16 16\n255\n' + bytes([10+i, 0, 0])*256)
            (root/'large.ppm').write_bytes(b'P6\n17 16\n255\n' + bytes([20, 0, 0])*272)
            config = {'version': 1,
                      'layout': {'provider': 'normalized', 'model': 'layout'},
                      'models': {'layout': {'backend': 'mock', 'response': {'boxes': [
                          {'type': 'text', 'bbox': [0, 0, 16, 16]}]}},
                          'ocr': {'backend': 'http_json', 'instances': 1,
                                  'endpoint': f'http://127.0.0.1:{backend.server_port}/ocr'}},
                      'routes': {'text': {'model': 'ocr'}}}
            (root/'config.json').write_text(json.dumps(config))
            with socket.socket() as sock:
                sock.bind(('127.0.0.1', 0)); port = sock.getsockname()[1]
            base = f'http://127.0.0.1:{port}'
            with open(root/'log', 'w') as log:
                proc = subprocess.Popen([binary, '--config', str(root/'config.json'),
                    '--data-dir', str(root/'state'), '--allowed-input-root', str(root),
                    '--port', str(port), '--page-workers', '1', '--document-workers', '1',
                    '--max-queued-pages', '4', '--max-queued-page-bytes', '768'],
                    stdout=log, stderr=log)
                try:
                    ready(base, proc)
                    def submit(name):
                        code, item = request(base, '/v1/jobs', method='POST',
                            data=json.dumps({'path': str(root/name)}).encode())
                        assert code == 202, (code, item)
                        return item['id']
                    first = submit('0.ppm')
                    assert backend.entered.wait(5)
                    second, third, large = (submit(name) for name in
                                            ('1.ppm', '2.ppm', 'large.ppm'))
                    metrics = lambda: request(base, '/v1/metrics')[1]
                    until(lambda: metrics()['queued_page_bytes'] == 768)
                    assert metrics()['queued_pages'] == 1
                    assert metrics()['reserved_page_bytes'] == 0
                    assert request(base, '/v1/jobs/'+third)[1]['status'] in ('queued', 'reading')
                    backend.release.set()
                    for job in (first, second, third):
                        assert finished(base, job)['status'] == 'succeeded'
                    result = finished(base, large)
                    assert result['status'] == 'failed' and 'queued page byte limit' in result['error'], result
                    end = metrics()
                    assert end['queued_page_bytes'] == end['reserved_page_bytes'] == 0, end
                    assert end['active_jobs'] == 0 and end['failed_total'] == 1, end
                finally:
                    backend.release.set(); proc.terminate()
                    try: proc.wait(timeout=10)
                    except subprocess.TimeoutExpired: proc.kill(); proc.wait()
                    assert proc.returncode == 0, proc.returncode
    finally:
        backend.release.set(); backend.shutdown(); backend.server_close(); thread.join()
    print('PASS: page byte backpressure and oversized rendered-page failure')


if __name__ == '__main__':
    server = str(Path(sys.argv[1]).resolve())
    run(server)
    run_page_budget(server)
