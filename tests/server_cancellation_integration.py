"""Queued/running cancellation and managed upload lifecycle over REST."""
import json
import os
import socket
import subprocess
import sys
import tempfile
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

from server_integration import ready, request


class SlowBackend(BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def do_POST(self):
        self.rfile.read(int(self.headers['Content-Length']))
        with self.server.lock:
            self.server.calls += 1
            self.server.entered.set()
        self.server.release.wait(12)
        body = b'{"text":"done"}'
        try:
            self.send_response(200)
            self.send_header('Content-Length', str(len(body)))
            self.end_headers()
            self.wfile.write(body)
        except (BrokenPipeError, ConnectionResetError):
            pass


def until(base, identifier, expected, timeout=5):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        code, state = request(base, f'/v1/jobs/{identifier}')
        assert code == 200, (code, state)
        if state['status'] == expected:
            return state
        time.sleep(.03)
    raise AssertionError((expected, request(base, f'/v1/jobs/{identifier}')))


def run(binary):
    backend = ThreadingHTTPServer(('127.0.0.1', 0), SlowBackend)
    backend.daemon_threads = True
    backend.lock = threading.Lock()
    backend.entered = threading.Event()
    backend.release = threading.Event()
    backend.calls = 0
    thread = threading.Thread(target=backend.serve_forever, daemon=True)
    thread.start()
    try:
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            src = root / 'source.ppm'
            data = b'P6\n20 20\n255\n' + b'\xff' * 1200
            src.write_bytes(data)
            converter = root / 'slow-converter.sh'
            pid_file = root / 'converter.pid'
            converter.write_text(f'#!/bin/sh\necho $$ > "{pid_file}"\nexec sleep 30\n')
            converter.chmod(0o700)
            office = root / 'slow.docx'
            office.write_bytes(b'fake office input')
            with socket.socket() as sock:
                sock.bind(('127.0.0.1', 0))
                port = sock.getsockname()[1]
            base = f'http://127.0.0.1:{port}'
            config = {'version': 1,
                'execution': {'page_workers': 1, 'box_workers': 2, 'document_workers': 1},
                'document': {'soffice': str(converter), 'timeout_seconds': 60},
                'layout': {'provider': 'normalized', 'model': 'layout', 'coordinates': 'normalized'},
                'models': {
                    'layout': {'backend': 'mock', 'response': {'boxes': [
                        {'type': 'text', 'bbox': [0, 0, 1, 1]}]}},
                    'ocr': {'backend': 'http_json', 'instances': 1,
                            'endpoint': f'http://127.0.0.1:{backend.server_port}/ocr'}},
                'routes': {'text': {'model': 'ocr'}},
                'server': {'port': port, 'data_dir': str(root / 'state'),
                           'allowed_input_root': str(root), 'max_jobs': 4}}
            cfg = root / 'config.json'
            cfg.write_text(json.dumps(config))
            with open(root / 'server.log', 'w') as log:
                proc = subprocess.Popen([binary, '--config', str(cfg)], stdout=log, stderr=log)
                try:
                    ready(base, proc)
                    submit_path = lambda path: request(base, '/v1/jobs', method='POST',
                        data=json.dumps({'path': str(path)}).encode())
                    code, running = submit_path(src)
                    assert code == 202
                    assert backend.entered.wait(5), 'first OCR never started'
                    code, queued = request(base, '/v1/jobs/upload?extension=.ppm', method='POST', data=data)
                    assert code == 202
                    upload_id = queued['id']
                    assert request(base, f'/v1/jobs/{upload_id}/source') == (200, data)
                    assert request(base, f"/v1/jobs/{running['id']}/source")[0] == 409
                    assert request(base, f'/v1/jobs/{upload_id}', method='DELETE')[0] == 409
                    code, cancelled = request(base, f'/v1/jobs/{upload_id}/cancel', method='POST', data=b'')
                    assert code in (200, 202) and cancelled['status'] in ('cancelling', 'cancelled'), (code, cancelled)
                    until(base, upload_id, 'cancelled')
                    assert request(base, f'/v1/jobs/{upload_id}/cancel', method='POST', data=b'')[1]['status'] == 'cancelled'
                    assert backend.calls == 1
                    assert request(base, '/v1/jobs?status=cancelled&limit=1&offset=0')[1]['items'][0]['id'] == upload_id
                    assert request(base, '/v1/jobs?limit=0')[0] == 400
                    assert request(base, f'/v1/jobs/{upload_id}/result')[0] == 409
                    assert request(base, f'/v1/jobs/{upload_id}', method='DELETE')[0] == 204
                    assert request(base, f'/v1/jobs/{upload_id}')[0] == 404
                    assert not (root / 'state' / 'jobs' / upload_id).exists()

                    start = time.monotonic()
                    code, state = request(base, f"/v1/jobs/{running['id']}/cancel", method='POST', data=b'')
                    assert code in (200, 202) and state['status'] in ('cancelling', 'cancelled')
                    until(base, running['id'], 'cancelled', timeout=5)
                    assert time.monotonic() - start < 5, 'in-flight HTTP did not abort'
                    assert request(base, f"/v1/jobs/{running['id']}", method='DELETE')[0] == 204
                    assert src.exists(), 'deleting a path job removed the external source'

                    code, converting = submit_path(office)
                    assert code == 202
                    end = time.monotonic() + 5
                    while not pid_file.exists() and time.monotonic() < end:
                        time.sleep(.03)
                    assert pid_file.exists(), 'converter did not start'
                    start = time.monotonic()
                    assert request(base, f"/v1/jobs/{converting['id']}/cancel", method='POST', data=b'')[0] in (200, 202)
                    until(base, converting['id'], 'cancelled', timeout=5)
                    assert time.monotonic() - start < 5, 'conversion child was not interrupted'
                    try:
                        os.kill(int(pid_file.read_text()), 0)
                    except ProcessLookupError:
                        pass
                    else:
                        raise AssertionError('cancelled converter process still exists')
                    assert request(base, f"/v1/jobs/{converting['id']}", method='DELETE')[0] == 204
                    assert office.exists()
                    assert request(base, '/v1/metrics')[1]['cancelled_total'] == 3
                    # Deleting does not reset the cumulative max_jobs admission limit.
                    backend.release.set()
                    code, succeeded = submit_path(src)
                    assert code == 202
                    until(base, succeeded['id'], 'succeeded')
                    assert request(base, f"/v1/jobs/{succeeded['id']}/cancel", method='POST', data=b'')[0] == 409
                    assert request(base, f"/v1/jobs/{succeeded['id']}/result")[0] == 200
                    assert request(base, f"/v1/jobs/{succeeded['id']}", method='DELETE')[0] == 204
                    assert request(base, f"/v1/jobs/{succeeded['id']}/result")[0] == 404
                    assert src.exists()
                    assert submit_path(src)[0] == 503
                finally:
                    backend.release.set()
                    proc.terminate()
                    try: proc.wait(timeout=10)
                    except subprocess.TimeoutExpired: proc.kill(); proc.wait()
    finally:
        backend.release.set()
        backend.shutdown(); backend.server_close(); thread.join()
    print('PASS: queued, HTTP and converter cancellation; list/source/delete lifecycle')


if __name__ == '__main__':
    run(str(Path(sys.argv[1]).resolve()))
